// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_test.c - 用户态测试程序（运行在目标 ARM 板上）
 *
 * 演示用户态如何通过字符设备接口访问内核驱动：
 *   1. open/read  : 阻塞读取采样数据（内核态 -> 用户态的数据通路）
 *   2. ioctl      : 获取结构化数据、修改采样周期、读统计
 *   3. write      : 下发命令（触发一次立即采样）
 *
 * 交叉编译（在 macOS 开发机上）：
 *   aarch64-linux-musl-gcc -static -O2 -Wall -o sensor_test sensor_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "../driver/sensor_ioctl.h"

#define DEV_PATH "/dev/sensor0"

static void show_sample(struct sensor_sample *s)
{
	printf("  seq=%-4u temp=%d.%03d C  irq=%-4u ts=%llu ns\n",
	       s->seq, s->temp_milli / 1000, abs(s->temp_milli % 1000),
	       s->irq_count, (unsigned long long)s->ts_ns);
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : DEV_PATH;
	char buf[128];
	struct sensor_sample sample;
	struct sensor_stats stats;
	unsigned int interval = 200;
	int fd, i, ret;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open " DEV_PATH);
		fprintf(stderr, "提示：驱动是否已 insmod？设备是否已创建？\n");
		return 1;
	}
	printf("== 打开 %s 成功 ==\n\n", path);

	/* 1. read(): 阻塞读取，每次返回一个新样本（内核自动唤醒） */
	printf("== 1) read() 阻塞读 3 次（默认 500ms 周期）==\n");
	for (i = 0; i < 3; i++) {
		ret = read(fd, buf, sizeof(buf) - 1);
		if (ret < 0) {
			perror("read");
			break;
		}
		buf[ret] = '\0';
		printf("  [read %d] %s", i + 1, buf);
	}

	/* 2. ioctl(): 修改采样周期，让内核立即生效 */
	printf("\n== 2) ioctl(SENSOR_IOC_SET_INTERVAL, 200) ==\n");
	if (ioctl(fd, SENSOR_IOC_SET_INTERVAL, &interval) < 0)
		perror("SET_INTERVAL");

	for (i = 0; i < 3; i++) {
		ret = read(fd, buf, sizeof(buf) - 1);
		if (ret < 0)
			break;
		buf[ret] = '\0';
		printf("  [read %d] %s", i + 1, buf);
	}

	/* 3. ioctl(): 获取结构化样本（比文本解析更规范的做法） */
	printf("\n== 3) ioctl(SENSOR_IOC_GET_SAMPLE) ==\n");
	if (ioctl(fd, SENSOR_IOC_GET_SAMPLE, &sample) == 0)
		show_sample(&sample);

	/* 4. write(): 下发"立即采样"命令 */
	printf("\n== 4) write() 触发一次立即采样 ==\n");
	{
		char cmd = 0x01;
		if (write(fd, &cmd, 1) == 1)
			printf("  已下发命令 0x01\n");
	}

	/* 5. ioctl(): 统计信息 */
	printf("\n== 5) ioctl(SENSOR_IOC_GET_STATS) ==\n");
	if (ioctl(fd, SENSOR_IOC_GET_STATS, &stats) == 0) {
		printf("  open=%u read=%u irq=%u i2c_err=%u interval=%ums\n",
		       stats.open_count, stats.read_count, stats.irq_count,
		       stats.i2c_errors, stats.interval_ms);
	}

	close(fd);
	printf("\n== 测试结束 ==\n");
	return 0;
}
