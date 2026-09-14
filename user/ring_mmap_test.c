// SPDX-License-Identifier: GPL-2.0
/*
 * ring_mmap_test.c - 阶段 03 测试：kfifo 环形缓冲 + mmap 零拷贝
 *
 * 验证四件事（每件都对应一个可能出错的实现细节）：
 *
 *   1) 溢出策略真的生效
 *      把周期调到 10ms 后 3 秒不读 → 缓冲必然被填满 → kfifo_dropped 必须 > 0。
 *      只断言"计数增长"，而不是断言某个精确数值：具体丢多少取决于调度抖动，
 *      但"读方跟不上就必须有丢弃"是确定的。
 *
 *   2) mmap 内容与内核的一致（seqlock 约定）
 *      读 seq → 读数据 → 重读 seq，两次相同且为偶数才采信；否则重试。
 *      校验：magic/version、样本 seq 严格递增（不回退）、温度落在模拟区间。
 *      为什么必须校验"递增"：如果驱动没用 seqlock（或忘了写屏障），
 *      用户态就有机会读到"写了一半"的环形数组 —— 表现为样本错位/乱序，
 *      而不是崩溃，所以必须靠数据自身的单调性把它抓出来。
 *
 *   3) 只读映射真的只读
 *      往映射区写必须触发 SIGSEGV；映射长度超过一页必须返回 EINVAL。
 *      这两条都是驱动的显式校验，不验证就等于不知道它是否生效。
 *
 *   4) 队列语义：有缓冲数据时 read() 立即返回（不睡一个采样周期）
 *      这是"环形缓冲解耦采样与读取速率"最直接的证据：
 *      如果 read 还是"等下一个样本"，在 10ms 周期下也会很快返回，区分度不够；
 *      因此这里同时报告耗时，由测试脚本按阈值判定。
 *
 * 输出格式（测试脚本按行解析，键名不要改）：
 *   [RING] dropped=<n>
 *   [RING] ring_count=<n>
 *   [RING] ring_capacity=<n>
 *   [RING] mmap_ok=1
 *   [RING] mmap_magic_ok=1 version_ok=1
 *   [RING] mmap_reads=100 mmap_retries=<n> mmap_invalid=<n>
 *   [RING] shm_seq_min=<n> shm_seq_max=<n> shm_seq_odd_seen=<n>
 *   [RING] mmap_seq_monotonic=1
 *   [RING] mmap_ro_sigsegv=1
 *   [RING] mmap_oversize_errno=22
 *   [RING] buffered_read_ms=<n>
 *   [RING] new_fd_sees_buffered=1
 *   [RING] OVERALL=PASS|FAIL
 *
 * 【两种运行模式】（阶段 03 整改新增，用于解决"seqlock 无法被观测"的问题）
 *   默认（无参数）：完整功能回归，mmap 一致性读固定 100 轮。
 *   quick        ：只跑 mmap 一致性读，且在 AMPLIFY_MS 内**紧凑循环**尽可能多地采样。
 *                 配合驱动的 shm_publish_delay_us 参数（用 fsleep 把"写入中"窗口放大到毫秒级），
 *                 使读者真的能撞上"正在写入"窗口 —— 于是：
 *                   重试次数 > 0：证明协议路径真的被执行（正向证据）；
 *                   撕裂样本 = 0：证明协议真的有效（一致性成立）。
 *                 这是"受控放大器"方法：没有它，1µs/10ms ≈ 10⁻⁴ 的窗口
 *                 永远撞不上，"seqlock 生效"根本无法被验证。
 *                 为什么驱动侧必须是睡眠延时：自旋会占满 CPU，读者若被调度到同一 CPU
 *                 就永远看不到窗口（实测同一代码 3 次运行 2 次失败）。
 *                 若本次仍未命中（宿主调度所限），测试脚本会把这几项记为
 *                 [CHECK:SKIP]（未取得证据），而不是当成通过。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "sensor_ioctl.h"

#define DEV_PATH		"/dev/sensor0"
#define PAGE_SIZE_BYTES		4096
#define MMAP_READS		100	/* seqlock 一致性读的轮数 */
/*
 * 测"队列有数据时 read 立即返回"之前，先把采样周期拉大到 2 秒。
 * 为什么必须这样：如果周期还是 10ms，那么"等下一个样本"的实现最多也就等 10ms，
 * 用 100ms 阈值根本区分不出来（弱检查）。周期 2s 时两种实现相差三个数量级：
 *   从队列取 → ~1ms（立即返回）；等下一个样本 → ~2000ms。
 */
#define BUFFERED_READ_INTERVAL_MS 2000
#define DRAIN_CHECK_SAMPLES	10	/* 连读 10 次，验证历史样本被保留（而不是只有最新一个） */
#define DROP_INTERVAL_MS	10	/* 溢出测试用的采样周期 */
#define DROP_WAIT_MS		3000	/* 不读的时长：10ms × 300 >> 队列容量 */
#define TEMP_MIN_MILLI		15000	/* 芯片模拟区间（virt_i2c 产生 24~26 ℃，放宽到 15~45） */
#define TEMP_MAX_MILLI		45000
/*
 * 放大器模式的采样时长与上限。
 * 2000ms 内、采样周期 10ms、写入窗口被 fsleep 放大到 1~2ms（占空比 10~20%），
 * 紧凑循环下能稳定撞上多次"写入中"窗口，于是 retries > 0 可期；
 * 实测（三轮）：iters 426~445 万、odd_seen 161~200、retries 2.9~3.5 亿、invalid=0。
 * 注意：窗口不能太大（接近 100% 占空比时读者找不到"偶数"时刻，会长时间自旋），
 * 重试上限也必须与窗口时长匹配（见 shm_snapshot 里的注释）。
 */
#define AMPLIFY_MS		2000
#define AMPLIFY_MAX_READS	20000000

static volatile sig_atomic_t segv_seen;
static sigjmp_buf segv_jb;

static void on_segv(int sig)
{
	(void)sig;
	segv_seen = 1;
	siglongjmp(segv_jb, 1);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (ms % 1000) * 1000000 };

	nanosleep(&ts, NULL);
}

/*
 * seqlock 一致性快照：把共享区里"当前有效的样本窗口"抄到本地缓冲。
 *
 * 用户态没有内核的 smp_rmb()/read_seqbegin()，等价做法是：
 *   __sync_synchronize()（全屏障）+ 两次读 seq 比较。
 * 屏障的必要性：编译器/CPU 可能把"读数据"重排序到"读 seq"之前，
 * 那样即使 seq 检查通过，读到的也可能是旧数据 —— 这种错误极难复现，
 * 只能靠屏障从根上避免。
 *
 * 返回值：0 成功；-1 重试次数超限（说明写方一直在覆盖，属于异常）。
 */
static int shm_snapshot(const volatile struct sensor_shm *shm,
			struct sensor_sample *out, unsigned int out_max,
			unsigned int *count_out, long long *retries_out)
{
	unsigned int s1, s2, count, write_idx, start, i;
	long long retries = 0;

	for (;;) {
		s1 = shm->seq;
		if (s1 & 1u) {		/* 奇数 = 写方正在更新，重试 */
			retries++;
			continue;
		}
		__sync_synchronize();

		count = shm->count;
		write_idx = shm->write_idx;

		/* 防御：count 不可能超过环形数组长度，超了说明内核侧布局不对 */
		if (count > SENSOR_SHM_SAMPLES || write_idx >= SENSOR_SHM_SAMPLES)
			return -2;
		if (count > out_max)
			count = out_max;

		/* 窗口：最近 count 个样本，最旧的一条在 (write_idx - count) 处 */
		start = (write_idx + SENSOR_SHM_SAMPLES - count) % SENSOR_SHM_SAMPLES;
		for (i = 0; i < count; i++)
			out[i] = shm->samples[(start + i) % SENSOR_SHM_SAMPLES];

		__sync_synchronize();
		s2 = shm->seq;
		if (s1 == s2)
			break;		/* 期间没有写入 → 本次抄写一致 */

		retries++;
		/*
		 * 重试上限：防止写者异常时无限自旋。
		 * 需要与"窗口时长"匹配：放大器窗口现在是睡眠延时 fsleep(1000)（实际 1~2ms），
		 * 一次快照至多等一个窗口即可完成；上限太小会把"没等到"误记为 invalid，
		 * 上限太大则可能在异常情况下长时间自旋（实测 200 万会明显拖慢）。
		 * 20 万次在 TCG 下远大于 2ms 窗口对应的重试次数。
		 */
		if (retries > 200000)
			return -1;
	}

	*count_out = count;
	*retries_out = retries;
	return 0;
}

/* 校验快照：seq 不得回退、温度必须在模拟区间内 */
static int snapshot_valid(const struct sensor_sample *s, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (s[i].temp_milli < TEMP_MIN_MILLI || s[i].temp_milli > TEMP_MAX_MILLI)
			return 0;
		if (i > 0 && s[i].seq <= s[i - 1].seq)
			return 0;	/* 回退 = 读到了错位/撕裂的数据 */
	}
	return 1;
}

int main(int argc, char **argv)
{
	struct sensor_stats st;
	unsigned int interval = DROP_INTERVAL_MS;
	volatile struct sensor_shm *shm = MAP_FAILED;
	volatile unsigned int *ro_probe;
	struct pollfd pfd;
	void *oversize;
	int fd, rc, i, invalid = 0, snapshots = 0;
	long long retries_total = 0;	/* 放大器模式下重试次数可达 10^9 量级，必须用 long long */
	int magic_ok = 0, version_ok = 0, monotonic = 1, ro_ok = 0;
	int oversize_errno = 0, new_fd_ok = 0, new_fd = -1;
	long long t0, buffered_ms = -1;
	int ok_dropped, ok_mmap, ok_ro, ok_oversize, ok_buffered, ok_newfd, overall;
	int seq_advances = 0;	/* 共享区 seq 是否真的在推进（探针结果） */
	/* quick 模式：只跑 mmap 一致性读，用于放大器验证（见文件头说明） */
	int quick = (argc > 1 && strcmp(argv[1], "quick") == 0);
	unsigned int shm_seq_min = 0xffffffffu, shm_seq_max = 0, shm_seq_odd = 0;
	unsigned int shm_seq_changes = 0;	/* 观察到 seq 发生变化的次数（证明读者真的在跟踪写方） */
	unsigned int prev_seq_seen = 0;
	int have_prev_seq = 0;
	unsigned int reads_target = MMAP_READS;
	long long t_quick = 0;

	printf("[RING] device=%s\n", DEV_PATH);
	printf("[RING] mode=%s\n", quick ? "quick" : "full");

	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		printf("[RING] open_error=%s\n", strerror(errno));
		printf("[RING] OVERALL=FAIL\n");
		return 1;
	}

	/* ---------------- 1) 溢出：调周期 + 不读 ---------------- */
	if (ioctl(fd, SENSOR_IOC_SET_INTERVAL, &interval) < 0)
		printf("[RING] set_interval_error=%s\n", strerror(errno));
	else
		printf("[RING] interval_set=%u\n", interval);

	/* quick 模式只关心 mmap 一致性（放大器验证），不花 3 秒去造溢出 */
	{
		int wait_ms = quick ? 300 : DROP_WAIT_MS;

		sleep_ms(wait_ms);
		printf("[RING] slept_ms=%d\n", wait_ms);
	}

	memset(&st, 0, sizeof(st));
	if (ioctl(fd, SENSOR_IOC_GET_STATS, &st) < 0) {
		printf("[RING] get_stats_error=%s\n", strerror(errno));
	}
	printf("[RING] dropped=%u\n", st.kfifo_dropped);
	printf("[RING] ring_count=%u\n", st.ring_count);
	printf("[RING] irq_count=%u\n", st.irq_count);
	printf("[RING] ring_capacity=%u\n", st.ring_capacity);
	/*
	 * 强不变量：一旦发生过丢弃，缓冲必须是满的（不然就说明样本无故蒸发）。
	 * 旧实现 copy_to_user 失败时直接把样本丢掉且不计入 dropped，
	 * 正好会被这个不变量抓住（见驱动 read() 里的回队处理）。
	 */
	printf("[RING] dropped_at_capacity=%d\n",
	       (st.kfifo_dropped == 0) || (st.ring_count == st.ring_capacity));
	printf("[RING] conservation_delta=%d\n",
	       (int)st.irq_count - (int)(st.ring_count + st.kfifo_dropped));

	/*
	 * ---------------- 1b) 连读多个样本：证明"历史被保留" ----------------
	 *
	 * 这是环形缓冲相对旧实现（只缓存最新一个样本）最本质的差别：
	 * 旧实现连读 10 次会拿到同一个样本 10 遍；正确实现必须给出 10 个
	 * 序号严格递增的不同样本。因此这一项能直接区分"有队列"和"没队列"。
	 */
	{
		unsigned int prev = 0, first = 0, last = 0;
		int distinct = 1, got = 0;
		char buf[128];
		int k;

		if (quick) {
			/* quick 模式跳过连读校验（由默认模式的完整跑覆盖） */
		} else {
		for (k = 0; k < DRAIN_CHECK_SAMPLES; k++) {
			ssize_t n = read(fd, buf, sizeof(buf) - 1);
			unsigned int seq = 0;

			if (n <= 0)
				break;
			buf[n] = '\0';
			if (sscanf(buf, "seq=%u", &seq) != 1)
				break;
			if (k == 0)
				first = seq;
			else if (seq <= prev)
				distinct = 0;	/* 序号没增长 = 拿到了重复样本 */
			prev = seq;
			last = seq;
			got++;
		}
		if (got < DRAIN_CHECK_SAMPLES)
			distinct = 0;
		printf("[RING] drained_samples=%d drained_seq_distinct=%d seq_span=%u\n",
		       got, distinct, (last >= first) ? last - first : 0);
		}
	}

	/* ---------------- 2) mmap + seqlock 一致性 ---------------- */
	shm = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
		printf("[RING] mmap_error=%s\n", strerror(errno));
		printf("[RING] mmap_ok=0\n");
	} else {
		struct sensor_sample buf[SENSOR_SHM_SAMPLES];

		printf("[RING] mmap_ok=1\n");
		magic_ok = (shm->magic == SENSOR_SHM_MAGIC);
		version_ok = (shm->version == SENSOR_SHM_VERSION);
		printf("[RING] mmap_magic_ok=%d version_ok=%d\n", magic_ok, version_ok);

		/*
		 * seq 推进探针（确定性检查）：等待共享区 seq 发生变化，最多 500ms。
		 *
		 * 为什么不直接用下面 100 轮循环里的 min/max 判断：
		 *   100 轮一致性读总共只花几毫秒，跨不过一个 10ms 采样周期，
		 *   于是 min == max 是常态 —— 那是「窗口太短」，不是「seq 没在动」。
		 *   本阶段第一版整改就因此误判过（min=max=600 被当成假 seqlock）。
		 *   单独等一次变化是确定性的：10ms 周期下 500ms 内有 50 次写入机会。
		 *
		 * 为什么这一项能抓住「假 seqlock」：
		 *   初版只在内核侧用 seqlock_t，从不写共享区的 seq → 这个探针会等满
		 *   500ms 也等不到变化（实测恒为 0），直接 FAIL。
		 */
		{
			unsigned int s_first = shm->seq, s_last = s_first;
			int waited = 0;

			while (waited < 500) {
				s_last = shm->seq;
				if (s_last != s_first)
					break;
				sleep_ms(5);
				waited += 5;
			}
			seq_advances = (s_last != s_first);
			printf("[RING] shm_seq_probe_first=%u shm_seq_probe_last=%u waited_ms=%d\n",
			       s_first, s_last, waited);
			printf("[RING] shm_seq_advances=%d\n", seq_advances);
		}

		/*
		 * quick 模式：在 AMPLIFY_MS 内紧凑循环，尽可能多地落在“写入中”窗口里；
		 * 默认模式：固定 100 轮（保证 mmap_reads=100 这个既有检查项不变）。
		 */
		reads_target = quick ? AMPLIFY_MAX_READS : MMAP_READS;
		t_quick = now_ms();

		for (i = 0; i < (int)reads_target; i++) {
			unsigned int count = 0;
			unsigned int seq_now;
			long long r = 0;

			/* quick 模式：到时间就停（在 AMPLIFY_MS 内尽可能多地采样） */
			if (quick && (now_ms() - t_quick) >= AMPLIFY_MS)
				break;

			/*
			 * 直接观测共享区里的 seq（这就是用户态可见的 seqlock 序号）。
			 * 它必须真的在动：如果驱动只在内核侧加锁、从不写这个字段，
			 * seq_max 就会等于 seq_min，下面的检查会直接把它抓出来。
			 */
			seq_now = shm->seq;
			if (!have_prev_seq) {
				prev_seq_seen = seq_now;
				have_prev_seq = 1;
			} else if (seq_now != prev_seq_seen) {
				shm_seq_changes++;
				prev_seq_seen = seq_now;
			}
			if (seq_now < shm_seq_min)
				shm_seq_min = seq_now;
			if (seq_now > shm_seq_max)
				shm_seq_max = seq_now;
			if (seq_now & 1u)
				shm_seq_odd++;

			if (shm_snapshot(shm, buf, SENSOR_SHM_SAMPLES, &count, &r) != 0) {
				invalid++;
				continue;
			}
			retries_total += r;
			snapshots++;

			/* 快照里的样本 seq 必须严格递增、温度必须合法 */
			if (!snapshot_valid(buf, count)) {
				invalid++;
				monotonic = 0;
			}
		}
		printf("[RING] mmap_reads=%d mmap_retries=%lld mmap_invalid=%d\n",
		       snapshots, retries_total, invalid);
		printf("[RING] shm_seq_min=%u shm_seq_max=%u shm_seq_odd_seen=%u shm_seq_changes=%u\n",
		       (shm_seq_min == 0xffffffffu) ? 0u : shm_seq_min,
		       shm_seq_max, shm_seq_odd, shm_seq_changes);
		printf("[RING] loop_iters=%d loop_elapsed_ms=%lld\n",
		       snapshots + invalid, now_ms() - t_quick);
		printf("[RING] mmap_seq_monotonic=%d\n", monotonic);

		/* ---------------- 3) 只读映射必须只读 ---------------- */
		if (quick) {
			/* quick 模式不做这几项（默认模式已覆盖） */
		} else if (signal(SIGSEGV, on_segv) == SIG_ERR) {
			printf("[RING] sigaction_error=%s\n", strerror(errno));
		} else if (sigsetjmp(segv_jb, 1) == 0) {
			ro_probe = (volatile unsigned int *)shm;
			*ro_probe = 0xdeadbeef;	/* 必须触发 SIGSEGV */
			printf("[RING] mmap_ro_sigsegv=0 (写入居然成功)\n");
		} else {
			ro_ok = segv_seen ? 1 : 0;
			printf("[RING] mmap_ro_sigsegv=%d\n", ro_ok);
		}
	}

	/* 超过一页的映射必须被拒绝（驱动返回 -EINVAL → errno=EINVAL） */
	oversize = mmap(NULL, 2 * PAGE_SIZE_BYTES, PROT_READ, MAP_SHARED, fd, 0);
	if (oversize != MAP_FAILED) {
		printf("[RING] mmap_oversize_errno=0 (居然映射成功)\n");
		munmap(oversize, 2 * PAGE_SIZE_BYTES);
	} else {
		oversize_errno = errno;
		printf("[RING] mmap_oversize_errno=%d\n", oversize_errno);
	}

	/* ---------------- 4) 队列有数据时 read 立即返回 ---------------- */
	if (quick) {
		/* quick 模式跳过 4/5 两项（默认模式已覆盖） */
	} else {
	{
		char buf[128];
		ssize_t n;
		unsigned int big = BUFFERED_READ_INTERVAL_MS;

		/*
		 * 把周期拉到 2s：此刻队列里还有样本，read 必须"立即返回"（~ms 级）；
		 * 若实现是"等下一个样本"，这里会等近 2 秒，阈值 100ms 一目了然。
		 */
		if (ioctl(fd, SENSOR_IOC_SET_INTERVAL, &big) < 0)
			printf("[RING] set_big_interval_error=%s\n", strerror(errno));
		printf("[RING] buffered_read_interval_ms=%u\n", big);

		t0 = now_ms();
		n = read(fd, buf, sizeof(buf) - 1);
		buffered_ms = now_ms() - t0;
		if (n > 0) {
			buf[n] = '\0';
			printf("[RING] buffered_read=%s", buf);
		} else {
			printf("[RING] buffered_read_error=%s\n",
			       n < 0 ? strerror(errno) : "0 字节");
		}
		printf("[RING] buffered_read_ms=%lld\n", buffered_ms);
	}

	/* ---------------- 5) 新 fd 应能立刻读到已缓冲的样本 ---------------- */
	new_fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (new_fd < 0) {
		printf("[RING] new_fd_open_error=%s\n", strerror(errno));
	} else {
		pfd.fd = new_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
			new_fd_ok = 1;
		printf("[RING] new_fd_sees_buffered=%d\n", new_fd_ok);
		close(new_fd);
	}
	}

	/* ---------------- 6) 判定 ---------------- */
	if (quick) {
		/*
		 * 放大器模式：只判定 seqlock 相关项。
		 * 除了“不一致样本为 0”之外，还要求：
		 *   - retries_total > 0：真的撞上过写入窗口（否则本轮什么都没验证到，
		 *     必须显式失败，而不是把一个“空转”当成通过 —— 这是阶段 01 的教训）；
		 *   - shm_seq_max > shm_seq_min：共享区里的 seq 真的在推进
		 *     （能拓 「内核从不写 seq」的假实现）。
		 */
		ok_dropped = 1; ok_ro = 1; ok_oversize = 1; ok_buffered = 1; ok_newfd = 1;
		ok_mmap = (shm != MAP_FAILED) && magic_ok && version_ok &&
			  (snapshots > 0) && (invalid == 0) && monotonic &&
			  (retries_total > 0) && seq_advances &&
			  (shm_seq_changes >= 3) &&
			  (shm_seq_max > shm_seq_min);
	} else {
		ok_dropped   = (st.kfifo_dropped >= 1);
		ok_mmap      = (shm != MAP_FAILED) && magic_ok && version_ok &&
			       (snapshots == MMAP_READS) && (invalid == 0) && monotonic &&
			       seq_advances;
		ok_ro        = ro_ok;
		ok_oversize  = (oversize_errno == EINVAL);
		ok_buffered  = (buffered_ms >= 0 && buffered_ms < 100);
		ok_newfd     = new_fd_ok;
	}

	overall = quick ? ok_mmap
			: (ok_dropped && ok_mmap && ok_ro && ok_oversize &&
			   ok_buffered && ok_newfd);
	printf("[RING] checks dropped=%d mmap=%d readonly=%d oversize=%d buffered=%d newfd=%d\n",
	       ok_dropped, ok_mmap, ok_ro, ok_oversize, ok_buffered, ok_newfd);
	printf("[RING] OVERALL=%s\n", overall ? "PASS" : "FAIL");

	if (shm != MAP_FAILED)
		munmap((void *)shm, PAGE_SIZE_BYTES);
	close(fd);
	return overall ? 0 : 1;
}
