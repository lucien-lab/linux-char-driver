// SPDX-License-Identifier: GPL-2.0
/*
 * concurrency_test.c - 阶段 03 测试：多进程并发访问同一个驱动
 *
 * 为什么必须做多进程测试：
 *   单进程测试只能证明"一次调用是对的"，证明不了"并发时不出错"。
 *   本驱动里三处状态是共享的：
 *     1) kfifo 环形缓冲 —— 多个读者同时出队（这是最容易出错的地方：
 *        kfifo 的免锁性质只覆盖"单读者+单写者"，多读者必须串行化）
 *     2) latest/统计计数 —— 读写并发
 *     3) mmap 共享区 —— 内核单写、多个进程同时读（seqlock 约定）
 *
 * 测试方式：8 个子进程各自 open 一次，循环 200 轮做三件事：
 *   read（非阻塞）/ ioctl(GET_SAMPLE) / mmap 快照校验
 * 每个子进程把自己的统计写进一块 MAP_SHARED|MAP_ANONYMOUS 内存，
 * 父进程汇总并判定：
 *   - 不能有子进程失败（退出码非 0）
 *   - 不能读到非法样本（温度越界、seq 回退、mmap 快照不一致）
 *   - 不能有非预期 errno（EAGAIN 是正常的"暂时没数据"，不算错误）
 *   - 至少真的读到过样本（否则等于什么都没验）
 *
 * 关于"证明无竞态"的诚实说明（写进文档也算结论的一部分）：
 *   200 轮 × 8 进程跑通 ≠ 数学上证明无竞态，它只能提高置信度；
 *   QEMU 单核 + TCG 还会削弱并发交错的机会。因此本测试的价值在于
 *   "一旦实现有明显错误（例如漏锁、seqlock 用错）就会高概率暴露"，
 *   而不是"通过即证明正确"。真正的工具是 KCSAN/lockdep（见知识点文档）。
 *
 * 输出格式（测试脚本按行解析）：
 *   [CONC] children=8 failed=<n>
 *   [CONC] child=<i> samples=<n> invalid=<n> eagain=<n> child_io_errors=<n> child_snapshots=<n>
 *   [CONC] samples_read=<n> eagain=<n> io_errors=<n> invalid_samples=<n> snapshots=<n> i2c_errors=<n>
 *   [CONC] consumed_seqs=<n> dup_seqs=<n> seq_min=<n> seq_max=<n> seq_gaps=<n>
 *   [CONC] OVERALL=PASS|FAIL
 *
 * 注意：子进程行里的键名一律加 child_ 前缀（child_io_errors= / child_snapshots=），
 * 保证汇总行的同名键是**唯一**匹配 —— 否则按行解析的测试脚本会取到 child=0 的值，
 * 导致「无非预期 errno」这类检查项实际只看了一个子进程（阶段 03 验证报告 S3）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "sensor_ioctl.h"

#define DEV_PATH		"/dev/sensor0"
#define PAGE_SIZE_BYTES		4096
#define NCHILD			8
#define NITER			200
#define TEMP_MIN_MILLI		15000
#define TEMP_MAX_MILLI		45000
#define CONC_INTERVAL_MS	20	/* 让样本持续产生，同时保留 EAGAIN 的可能 */
/*
 * 出队全局唯一性检查（阶段 03 整改新增，多核下最强的一条并发证据）：
 *   每个子进程把自己 read() 到的样本 seq 写进一块共享内存，父进程检查全局无重复。
 *   为什么这能抓竞态：kfifo 出队是"取出即移走"，正确实现下一个读者不可能拿到同一条样本；
 *   一旦读侧锁缺失或 out 索引被并发覆盖，同一样本就会被读出两次 —— 重复 seq 就是铁证。
 *   原版实现里子进程解析出 seq 后直接丢弃，于是"重复/丢失样本"完全不可见（验证报告 S2）。
 */
#define SEQ_CAP			(NCHILD * NITER * 2)	/* 收集上限（实际远低于此） */

/* 子进程统计（放在共享匿名内存里，父进程汇总） */
struct child_result {
	unsigned int samples;
	unsigned int invalid;
	unsigned int eagain;
	unsigned int io_errors;
	unsigned int snapshots;
	unsigned int retries;
};

/* 把 read() 返回的一行文本里的温度与序号解析出来 */
static int parse_sample(const char *buf, int *milli_out, unsigned int *seq_out)
{
	int whole = 0, frac = 0;
	unsigned int seq = 0;

	if (sscanf(buf, "seq=%u temp=%d.%3dC", &seq, &whole, &frac) != 3)
		return -1;

	*milli_out = (whole < 0) ? (whole * 1000 - frac) : (whole * 1000 + frac);
	if (seq_out)
		*seq_out = seq;
	return 0;
}

/*
 * 与 ring_mmap_test.c 相同的 seqlock 一致性快照（用户态侧）。
 * 这里重复实现是有意的：并发测试要验证的正是"多个读者同时做这个动作"，
 * 共用一份代码会掩盖"读者之间彼此干扰"的可能。
 */
static int shm_snapshot(const volatile struct sensor_shm *shm,
			struct sensor_sample *out, unsigned int *count_out,
			int *retries_out)
{
	unsigned int s1, s2, count, write_idx, start, i;
	int retries = 0;

	for (;;) {
		s1 = shm->seq;
		if (s1 & 1u) {
			retries++;
			continue;
		}
		__sync_synchronize();
		count = shm->count;
		write_idx = shm->write_idx;
		if (count > SENSOR_SHM_SAMPLES || write_idx >= SENSOR_SHM_SAMPLES)
			return -2;
		start = (write_idx + SENSOR_SHM_SAMPLES - count) % SENSOR_SHM_SAMPLES;
		for (i = 0; i < count; i++)
			out[i] = shm->samples[(start + i) % SENSOR_SHM_SAMPLES];
		__sync_synchronize();
		s2 = shm->seq;
		if (s1 == s2)
			break;
		retries++;
		if (retries > 100000)
			return -1;
	}
	*count_out = count;
	*retries_out = retries;
	return 0;
}

static int snapshot_valid(const struct sensor_sample *s, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (s[i].temp_milli < TEMP_MIN_MILLI || s[i].temp_milli > TEMP_MAX_MILLI)
			return 0;
		if (i > 0 && s[i].seq <= s[i - 1].seq)
			return 0;
	}
	return 1;
}

static void child_work(int idx, struct child_result *res,
		       unsigned int *seqs, unsigned int *seq_idx,
		       unsigned int *seq_overflow)
{
	struct sensor_sample buf[SENSOR_SHM_SAMPLES];
	volatile struct sensor_shm *shm;
	char line[128];
	int fd, i;

	fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		/* 用退出码把"打不开设备"这种致命错误告诉父进程 */
		_exit(2);
	}

	shm = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED)
		shm = NULL;

	for (i = 0; i < NITER; i++) {
		ssize_t n;

		/*
		 * 每轮让出 1ms：让 8 个进程的 200 轮真正跨越多个采样周期，
		 * 否则整轮测试几百微秒就跑完，采样与读取得不到重叠，
		 * 并发就没有被真正验证。
		 */
		usleep(1000);

		/* 1) read()：共享环形缓冲出队 */
		n = read(fd, line, sizeof(line) - 1);
		if (n > 0) {
			int milli = 0;
			unsigned int seq = 0;

			line[n] = '\0';
			if (parse_sample(line, &milli, &seq) == 0) {
				res->samples++;
				if (milli < TEMP_MIN_MILLI || milli > TEMP_MAX_MILLI)
					res->invalid++;
				/*
				 * 把消费到的 seq 记到共享数组：父进程据此检查全局无重复。
				 * __sync_fetch_and_add 保证多进程同时追加时槽位不重叠
				 * （否则"重复 seq"可能是我们自己的记账错误而不是驱动的竞态）。
				 */
				if (seqs && seq_idx) {
					unsigned int slot = __sync_fetch_and_add(seq_idx, 1);

					if (slot < SEQ_CAP)
						seqs[slot] = seq;
					else if (seq_overflow)
						(*seq_overflow)++;
				}
			} else {
				res->invalid++;
			}
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			res->eagain++;	/* 队列暂时为空：正常情况，不算错误 */
		} else if (n < 0) {
			res->io_errors++;
		}

		/* 2) ioctl(GET_SAMPLE)：读"最新样本"（与 read 走不同的出口） */
		{
			struct sensor_sample s;

			if (ioctl(fd, SENSOR_IOC_GET_SAMPLE, &s) == 0) {
				if (s.temp_milli < TEMP_MIN_MILLI ||
				    s.temp_milli > TEMP_MAX_MILLI)
					res->invalid++;
			} else {
				res->io_errors++;
			}
		}

		/* 3) mmap 快照：多进程同时读内核单写的共享区 */
		if (shm) {
			unsigned int count = 0;
			int r = 0;

			if (shm_snapshot(shm, buf, &count, &r) == 0) {
				res->snapshots++;
				res->retries += r;
				if (!snapshot_valid(buf, count))
					res->invalid++;
			} else {
				res->invalid++;	/* 连重试上限都超了：写入过于频繁 */
			}
		}
	}

	if (shm)
		munmap((void *)shm, PAGE_SIZE_BYTES);

	/*
	 * 每个子进程都校验 i2c_errors：并发读不应该引入 I2C 传输错误
	 * （一旦有，说明锁没保护住总线访问）。
	 */
	{
		struct sensor_stats st;

		if (ioctl(fd, SENSOR_IOC_GET_STATS, &st) == 0 && st.i2c_errors) {
			res->io_errors++;
			close(fd);
			_exit(3);
		}
	}

	close(fd);
	_exit(0);
}

/* qsort 比较函数：用于父进程统计重复 seq */
static int cmp_u32(const void *a, const void *b)
{
	unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;

	return (x > y) - (x < y);
}

int main(void)
{
	struct child_result *res;
	unsigned int interval = CONC_INTERVAL_MS;
	unsigned int samples = 0, eagain = 0, io_errors = 0, invalid = 0, snapshots = 0;
	unsigned int i2c_errors = 0;
	unsigned int *seqs, *seq_idx, *seq_overflow;
	unsigned int consumed = 0, dup_seqs = 0, seq_min = 0, seq_max = 0, seq_gaps = 0;
	size_t shm_sz;
	void *shm_base;
	pid_t pids[NCHILD];
	int fd, i, failed = 0, overall;

	/*
	 * 子进程结果与"消费到的样本 seq"都通过共享匿名内存回传
	 * （父进程 fork 前映射，子进程继承）。
	 * 布局：[child_result × NCHILD][seqs × SEQ_CAP][seq_idx][seq_overflow]
	 */
	shm_sz = sizeof(struct child_result) * NCHILD +
		 sizeof(unsigned int) * (SEQ_CAP + 2);
	shm_base = mmap(NULL, shm_sz, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shm_base == MAP_FAILED) {
		printf("[CONC] shm_alloc_error=%s\n", strerror(errno));
		printf("[CONC] OVERALL=FAIL\n");
		return 1;
	}
	memset(shm_base, 0, shm_sz);
	res = shm_base;
	seqs = (unsigned int *)(res + NCHILD);
	seq_idx = seqs + SEQ_CAP;
	seq_overflow = seq_idx + 1;

	/* 预热：先让环形缓冲里有数据，否则所有子进程只会看到 EAGAIN */
	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		printf("[CONC] open_error=%s\n", strerror(errno));
		printf("[CONC] OVERALL=FAIL\n");
		return 1;
	}
	if (ioctl(fd, SENSOR_IOC_SET_INTERVAL, &interval) < 0)
		printf("[CONC] set_interval_error=%s\n", strerror(errno));
	if (ioctl(fd, SENSOR_IOC_RESET, 0) < 0)
		printf("[CONC] reset_error=%s\n", strerror(errno));
	close(fd);
	printf("[CONC] warmup_ms=300 interval=%u\n", interval);

	{
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 300 * 1000000 };

		nanosleep(&ts, NULL);
	}

	printf("[CONC] forking %d children x %d iters\n", NCHILD, NITER);
	for (i = 0; i < NCHILD; i++) {
		pids[i] = fork();

		if (pids[i] < 0) {
			printf("[CONC] fork_error=%s\n", strerror(errno));
			pids[i] = 0;
			failed++;
			continue;
		}
		if (pids[i] == 0)
			child_work(i, &res[i], seqs, seq_idx, seq_overflow);	/* 不会返回 */
	}

	/* 逐个精确回收：waitpid 指定 pid，避免"收错孩子"导致失败数统计失真 */
	for (i = 0; i < NCHILD; i++) {
		int status = 0;
		pid_t r;

		if (pids[i] == 0)
			continue;	/* fork 就失败了，上面已计入 failed */

		do {
			r = waitpid(pids[i], &status, 0);
		} while (r < 0 && errno == EINTR);

		if (r < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("[CONC] child_pid=%d exit_status=0x%x\n", pids[i], status);
			failed++;
		}
	}

	for (i = 0; i < NCHILD; i++) {
		printf("[CONC] child=%d samples=%u invalid=%u eagain=%u child_io_errors=%u child_snapshots=%u retries=%u\n",
		       i, res[i].samples, res[i].invalid, res[i].eagain,
		       res[i].io_errors, res[i].snapshots, res[i].retries);
		samples += res[i].samples;
		invalid += res[i].invalid;
		eagain += res[i].eagain;
		io_errors += res[i].io_errors;
		snapshots += res[i].snapshots;
	}

	/*
	 * 出队全局唯一性：把所有子进程消费到的 seq 排序后相邻比较。
	 * 正确实现下，一条样本只能被一个读者取走一次 → 不可能出现重复。
	 * 出现重复就说明 out 索引被并发覆盖（读侧锁缺失/错用），这是真正的并发 bug。
	 */
	consumed = *seq_idx;
	if (consumed > SEQ_CAP)
		consumed = SEQ_CAP;
	if (consumed > 0) {
		seq_min = seq_max = seqs[0];
		qsort(seqs, consumed, sizeof(unsigned int), cmp_u32);
		for (i = 0; i < (int)consumed; i++) {
			if (i > 0 && seqs[i] == seqs[i - 1])
				dup_seqs++;
			if (seqs[i] < seq_min)
				seq_min = seqs[i];
			if (seqs[i] > seq_max)
				seq_max = seqs[i];
		}
		/* 缺口数：区间内未被任何子进程读到的序号个数。
		 * 注意它**不是错误**（队列满时的丢弃、EAGAIN 都会造成缺口），
		 * 仅作为“并发下确实发生了丢弃/竞争”的现场描述。 */
		seq_gaps = (seq_max - seq_min + 1) - consumed;
	}
	printf("[CONC] consumed_seqs=%u dup_seqs=%u seq_min=%u seq_max=%u seq_gaps=%u seq_overflow=%u\n",
	       consumed, dup_seqs, seq_min, seq_max, seq_gaps, *seq_overflow);

	/* 并发结束后读一次驱动侧统计：I2C 错误必须是 0 */
	fd = open(DEV_PATH, O_RDWR);
	if (fd >= 0) {
		struct sensor_stats st;

		if (ioctl(fd, SENSOR_IOC_GET_STATS, &st) == 0)
			i2c_errors = st.i2c_errors;
		close(fd);
	}

	printf("[CONC] children=%d failed=%d\n", NCHILD, failed);
	printf("[CONC] samples_read=%u eagain=%u io_errors=%u invalid_samples=%u snapshots=%u i2c_errors=%u\n",
	       samples, eagain, io_errors, invalid, snapshots, i2c_errors);

	overall = (failed == 0) && (invalid == 0) && (io_errors == 0) &&
		  (i2c_errors == 0) && (samples > 0) && (snapshots > 0) &&
		  (dup_seqs == 0) && (consumed > 0);
	printf("[CONC] OVERALL=%s\n", overall ? "PASS" : "FAIL");

	munmap(shm_base, shm_sz);
	return overall ? 0 : 1;
}
