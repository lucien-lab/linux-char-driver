// SPDX-License-Identifier: GPL-2.0
/*
 * iio_read_test.c - 通过 IIO 缓冲字符设备读取传感器样本（用户态）
 *
 * ======================== 为什么需要"算布局"而不是写死偏移 ========================
 *
 * 缓冲区里每个"扫描元素"（scan element）的字节布局由内核在**启用缓冲时**按
 * 启用通道 + 各通道 storagebits 计算，不同启用组合会产生不同的 padding。
 * 用户态必须按同一套规则自己算，才能正确解析。内核的权威算法是
 * drivers/iio/industrialio-buffer.c 的 iio_compute_scan_bytes()：
 *
 *     bytes = 0; largest = 0
 *     for 每个已启用通道（按 scan_index 升序）:
 *         len = storagebits / 8
 *         bytes = ALIGN(bytes, len)      // 对齐到该通道自身的存储宽度
 *         offset(通道) = bytes
 *         bytes += len
 *         largest = max(largest, len)
 *     if 时间戳已启用:
 *         len = 8（时间戳通道 64 bit）
 *         bytes = ALIGN(bytes, len); ts_offset = bytes; bytes += len
 *     stride = ALIGN(bytes, largest)
 *
 * 本程序就是按这个算法算的（我们把它完整镜像了一遍，见 compute_layout()）。
 *
 * ======================== 两个容易踩的坑（本项目都踩过） ========================
 *
 *   坑 1：scan_elements/<通道>_index **不是字节偏移**！
 *        内核 industrialio-buffer.c:362 的 index 属性直接返回 c->scan_index
 *        （通道在扫描里的序号：0、1、2…）。拿它当字节偏移解析会读到错位的数据。
 *        字节偏移必须由上面的算法用 storagebits 算出来。
 *
 *   坑 2：read() 的长度必须是"元素跨度的整数倍"，否则返回 -EINVAL。
 *        所以必须先算对 stride，再按 stride 读。
 *
 * ======================== 用法 ========================
 *   iio_read_test [/dev/iio:deviceN] [/sys/bus/iio/devices/iio:deviceN]
 * 退出码：全部样本合法 → 0；否则非 1。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <stdint.h>

#define MAX_CHANS	8
#define SAMPLE_WANT	5	/* 需要成功读到的样本数 */
#define POLL_TIMEOUT_MS	3000	/* 单个样本的等待上限 */

struct chan {
	char	name[64];	/* 通道名，如 in_temp */
	int	scan_index;	/* 来自 <通道>_index（注意：是序号，不是偏移） */
	int	enabled;
	int	realbits;
	int	storagebits;
	int	shift;
	char	sign;
	char	endian[4];	/* le / be */
	long	byte_offset;	/* 由 compute_layout() 算出 */
};

static long read_sysfs_long(const char *path)
{
	char buf[64];
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return strtol(buf, NULL, 0);
}

/* 解析 <通道>_type，格式：le:s16/16>>0（repeat>1 时还有 X<n>，本项目用不到） */
static int parse_type(const char *path, struct chan *c)
{
	char buf[64];
	FILE *f = fopen(path, "r");
	char sign;

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	if (sscanf(buf, "%3[^:]:%c%d/%d>>%d", c->endian, &sign,
		   &c->realbits, &c->storagebits, &c->shift) != 5)
		return -1;
	c->sign = sign;
	return 0;
}

static int cmp_scan_index(const void *a, const void *b)
{
	const struct chan *ca = a, *cb = b;

	return ca->scan_index - cb->scan_index;
}

/*
 * 镜像内核 iio_compute_scan_bytes()（见文件头说明）。
 * 返回值：stride（一个扫描元素的字节数）；失败返回 -1。
 */
static long compute_layout(struct chan *ch, int n)
{
	long bytes = 0;
	int i, largest = 0, have_ts = 0;

	qsort(ch, n, sizeof(*ch), cmp_scan_index);

	for (i = 0; i < n; i++) {
		int len;

		if (!ch[i].enabled)
			continue;
		/* 时间戳通道（64 位）按内核规则最后处理并做 8 字节对齐 */
		if (ch[i].storagebits == 64 && ch[i].realbits == 64) {
			have_ts = 1;
			continue;
		}
		len = ch[i].storagebits / 8;
		if (len <= 0)
			return -1;
		bytes = (bytes + len - 1) / len * len;	/* ALIGN(bytes, len) */
		ch[i].byte_offset = bytes;
		bytes += len;
		if (len > largest)
			largest = len;
	}

	if (have_ts) {
		bytes = (bytes + 8 - 1) / 8 * 8;
		if (largest < 8)
			largest = 8;
		/* 时间戳通道的真实偏移要写回，否则打印出来恒为 0、文档里的
		 * "offset=8" 就不可复现（阶段 05 验证者发现的观测缺陷）。 */
		for (i = 0; i < n; i++) {
			if (ch[i].enabled && ch[i].storagebits == 64 && ch[i].realbits == 64) {
				ch[i].byte_offset = bytes;
				break;
			}
		}
		bytes += 8;
	}

	if (largest == 0)
		return -1;
	bytes = (bytes + largest - 1) / largest * largest;
	return bytes;
}

/* 按 sign/endianness/storagebits 从元素里取出一个整数值 */
static long extract(const unsigned char *elem, const struct chan *c)
{
	long raw = 0;
	int bytes = c->storagebits / 8, i;

	if (bytes > 8)
		bytes = 8;
	if (strcmp(c->endian, "be") == 0) {
		for (i = 0; i < bytes; i++)
			raw = (raw << 8) | elem[c->byte_offset + i];
	} else {
		for (i = bytes - 1; i >= 0; i--)
			raw = (raw << 8) | elem[c->byte_offset + i];
	}
	if (c->sign == 's') {	/* 符号扩展到 long */
		long m = 1L << (c->realbits - 1);

		if (raw & m)
			raw -= (1L << c->realbits);
	}
	return raw >> c->shift;
}

int main(int argc, char **argv)
{
	const char *dev_path = (argc > 1) ? argv[1] : "/dev/iio:device0";
	const char *sys_dir  = (argc > 2) ? argv[2] : "/sys/bus/iio/devices/iio:device0";
	struct chan chans[MAX_CHANS];
	char scan_dir[256];
	int nchan = 0, i, temp_i = -1, hum_i = -1;
	long stride;
	DIR *d;
	struct dirent *de;
	int fd, ok = 0, bad = 0, timeouts = 0;
	int first_temp_milli = 0, first_hum_milli = 0;
	unsigned char *buf;

	/* ---------- 1) 枚举 scan_elements 里启用的通道 ---------- */
	snprintf(scan_dir, sizeof(scan_dir), "%s/scan_elements", sys_dir);
	d = opendir(scan_dir);
	if (!d) {
		fprintf(stderr, "无法打开 %s\n", scan_dir);
		printf("[IIO] dev_reads=0 temp_milli=0 OVERALL=FAIL\n");
		return 1;
	}
	while ((de = readdir(d)) != NULL && nchan < MAX_CHANS) {
		char path[320];
		int len = strlen(de->d_name);

		if (len < 4 || strcmp(de->d_name + len - 3, "_en") != 0)
			continue;
		memset(&chans[nchan], 0, sizeof(chans[nchan]));
		snprintf(chans[nchan].name, sizeof(chans[nchan].name), "%.*s",
			 len - 3, de->d_name);

		snprintf(path, sizeof(path), "%s/%s_en", scan_dir, chans[nchan].name);
		chans[nchan].enabled = (read_sysfs_long(path) == 1);
		snprintf(path, sizeof(path), "%s/%s_index", scan_dir, chans[nchan].name);
		chans[nchan].scan_index = read_sysfs_long(path);
		snprintf(path, sizeof(path), "%s/%s_type", scan_dir, chans[nchan].name);
		if (parse_type(path, &chans[nchan]) < 0) {
			fprintf(stderr, "解析 %s 失败\n", path);
			continue;
		}
		nchan++;
	}
	closedir(d);

	if (nchan == 0) {
		printf("[IIO] dev_reads=0 temp_milli=0 OVERALL=FAIL\n");
		return 1;
	}

	/* ---------- 2) 按内核算法算布局 ---------- */
	stride = compute_layout(chans, nchan);
	if (stride <= 0) {
		printf("[IIO] dev_reads=0 temp_milli=0 OVERALL=FAIL\n");
		return 1;
	}
	printf("[IIO] layout stride=%ld channels=%d\n", stride, nchan);
	for (i = 0; i < nchan; i++) {
		printf("[IIO]   chan %-24s scan_index=%d en=%d type=%s:%c%d/%d>>%d offset=%ld\n",
		       chans[i].name, chans[i].scan_index, chans[i].enabled,
		       chans[i].endian, chans[i].sign, chans[i].realbits,
		       chans[i].storagebits, chans[i].shift, chans[i].byte_offset);
		if (strcmp(chans[i].name, "in_temp") == 0)
			temp_i = i;
		else if (strcmp(chans[i].name, "in_humidityrelative") == 0)
			hum_i = i;
	}
	if (temp_i < 0) {
		fprintf(stderr, "未找到 in_temp 通道\n");
		printf("[IIO] dev_reads=0 temp_milli=0 OVERALL=FAIL\n");
		return 1;
	}

	buf = calloc(1, stride);
	if (!buf) {
		printf("[IIO] dev_reads=0 temp_milli=0 OVERALL=FAIL\n");
		return 1;
	}

	fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		perror("open iio device");
		printf("[IIO] dev_reads=0 temp_milli=0 OVERALL=FAIL\n");
		free(buf);
		return 1;
	}
	printf("[IIO] dev_path=%s\n", dev_path);

	/* ---------- 3) 读样本（每读一次就是一个扫描元素） ---------- */
	for (i = 0; i < SAMPLE_WANT * 4 && ok < SAMPLE_WANT; i++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&pfd, 1, POLL_TIMEOUT_MS) <= 0) {
			timeouts++;
			continue;
		}
		n = read(fd, buf, stride);
		if (n != stride) {
			fprintf(stderr, "read 返回 %zd（期望 %ld，errno=%d）\n",
				n, stride, errno);
			bad++;
			continue;
		}

		{
			long raw_t = extract(buf, &chans[temp_i]);
			long raw_h = (hum_i >= 0) ? extract(buf, &chans[hum_i]) : 0;
			/* 与驱动 read_raw(SCALE) 的约定一致：温度 LSB = 62.5 m℃ */
			int t_milli = (int)(raw_t * 1000 / 16);
			int h_milli = (int)(raw_h * 10);

			if (t_milli >= 15000 && t_milli <= 45000) {
				if (!ok) {
					first_temp_milli = t_milli;
					first_hum_milli = h_milli;
				}
				ok++;
			} else {
				fprintf(stderr, "样本 %d 温度越界: raw=%ld -> %d m℃\n",
					i, raw_t, t_milli);
				bad++;
			}
		}
	}

	close(fd);
	free(buf);

	printf("[IIO] samples_ok=%d samples_bad=%d timeouts=%d\n", ok, bad, timeouts);
	printf("[IIO] dev_reads=%d temp_milli=%d humidity_milli=%d OVERALL=%s\n",
	       ok, first_temp_milli, first_hum_milli,
	       (ok >= SAMPLE_WANT && bad == 0) ? "PASS" : "FAIL");

	return (ok >= SAMPLE_WANT && bad == 0) ? 0 : 1;
}
