// SPDX-License-Identifier: GPL-2.0
/*
 * io_models_test.c - 验证字符设备的四种 IO 模型（阶段 01）
 *
 * 内核侧对应实现（driver/sensor_char.c）：
 *   阻塞 read      sensor_read()  -> wait_event_interruptible_timeout()
 *   非阻塞 read    sensor_read()  -> O_NONBLOCK 分支返回 -EAGAIN
 *   多路复用       sensor_poll()  -> poll_wait() + EPOLLIN（电平触发）
 *   异步通知       sensor_fasync()-> fasync_helper()，中断下半部 kill_fasync(SIGIO)
 *
 * ============================ 为什么这样测 ============================
 *
 * 本程序经过一轮"独立验证"，验证者用破坏性变体证明：**最初版本的测试抓不到
 * 「poll() 里漏写 poll_wait()」这个最常见的实现错误**——删掉 poll_wait 后
 * 原套件依然 9 项全 PASS（假通过）。原因是原判定只看"epoll 至少被唤醒一次"，
 * 而 epoll_ctl(ADD) 时的首次 ->poll 调用就足以满足它。
 *
 * 因此本版本增加了三类**只有正确实现才可能通过**的观测：
 *
 *  1) 首次唤醒延迟（epoll_first_wakeup_ms，判 ≤1000ms）
 *     —— 这是发现"漏写 poll_wait"唯一可靠的观测：
 *        正确实现：注册了等待队列 → 下一个样本到达时立刻唤醒 → 延迟 ≈ 一个采样周期；
 *        漏写 poll_wait：epoll_wait 一直睡到超时，退出前最后一次 vfs_poll() 才看到
 *                        新样本而"假装可读" → 延迟 ≈ 超时值（实测 1503ms / 2003ms）。
 *        注意：只看返回值(>0)是分辨不出来的，必须测时间。
 *
 *  2) 唤醒次数双向约束（抽干后 3 秒窗口内 3 ≤ wakeups ≤ 12）
 *     —— 下界 ≥3 掐死上面那个假通过；上界 ≤12 抓"消费后仍报可读"的忙轮询错误
 *        （实测坏实现可达 12 万次）。
 *
 *  3) 电平触发语义三段断言 + 队列可读性
 *     —— 未消费时连续 5 次 poll 都应报 POLLIN（level 语义持续存在，
 *        one-shot 实现只能通过 1 次）；消费后必须立刻变为 0；
 *        新开的 fd 在"缓冲确有未读样本"时必须立刻可读。
 *
 *     【阶段 03 语义变更】可读性判据从"本 fd 是否消费过全局最新样本"
 *     （per-fd last_seq）改为"共享环形缓冲是否非空"——这是引入 kfifo 的必然结果，
 *     也修掉了 per-fd 方案在多进程共享队列下"poll 报可读、read 却阻塞"的矛盾。
 *     本文件里受影响的只有 (a) 的前提（改为先静置两个采样周期），
 *     其余断言的强度不变。
 *
 * 阻塞路径也补了时序断言（blocking_wait_ms）：先与"样本到达沿"对齐，再测第二次
 * read 的耗时，此时应 ≈ 一个完整采样周期；若驱动没真的睡在 waitqueue 上、
 * 而是拿了旧样本直接返回，耗时会接近 0。
 *
 * 输出格式（测试脚本按行解析，键名不要改）：
 *   [IO] blocking_read=ok
 *   [IO] blocking_wait_ms=<第二次阻塞 read 的等待时长>
 *   [IO] nonblock_eagain=<次数>
 *   [IO] epoll_first_wakeup_ms=<首次被唤醒的延迟>
 *   [IO] epoll_wakeups=<3 秒窗口内被唤醒次数>
 *   [IO] epoll_wait_ok=1
 *   [IO] new_fd_immediately_readable=1
 *   [IO] poll_unread_hits=<未消费时 poll 报 POLLIN 的次数，期望 5>
 *   [IO] poll_after_consume=<消费后 poll 是否仍报可读，期望 0>
 *   [IO] sigio_count=<收到的 SIGIO 次数>
 *   [IO] OVERALL=PASS|FAIL
 *
 * 编译（由 scripts/13-vm-fast-cycle.sh 自动完成）：
 *   gcc -static -O2 -Wall -I driver -o io_models_test io_models_test.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include "sensor_ioctl.h"

#define DEV_PATH		"/dev/sensor0"
#define DEFAULT_INTERVAL_MS	500	/* 与设备树 poll-interval-ms 一致 */
#define BLOCK_INTERVAL_MS	1000	/* 阻塞测试用较长周期，让断言阈值有安全余量 */
#define BLOCK_WAIT_MIN_MS	500	/* 第二次阻塞 read 至少要等这么久（≈ 一个周期） */
#define EPOLL_FIRST_TIMEOUT_MS	2500	/* 首次唤醒的等待上限（坏实现会睡满它） */
#define EPOLL_FIRST_MAX_MS	1000	/* 首次唤醒延迟上限：正确实现 ≤ 一个周期(500ms) */
#define EPOLL_WINDOW_MS		3000	/* 统计唤醒次数的时间窗口 */
#define EPOLL_WAKEUP_MIN	3	/* 下界：3 秒 / 500ms ≈ 6 次，取 3 留容差 */
#define EPOLL_WAKEUP_MAX	12	/* 上界：超过说明消费后仍上报 → 忙轮询 */
#define SIGIO_WAIT_MS		3000	/* 等待 SIGIO 的最长时间 */
#define POLL_UNREAD_PROBES	5	/* 未消费时连续 poll 的次数 */

/* 单调时钟毫秒，用于统计时间窗口 */
static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 睡眠指定毫秒数（阶段 03 起需要"静置攒数据"来建立确定性前提） */
static void sleep_ms(long long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (ms % 1000) * 1000000 };

	nanosleep(&ts, NULL);
}

/*
 * 抽干：把 fd 里所有未消费样本读走，直到 EAGAIN。
 * 为什么必须抽干：epoll_ctl(ADD) 会立刻触发一次 ->poll，如果此时还有未消费样本，
 * 即使驱动的 poll 再也不会唤醒用户态，"至少被唤醒一次"也成立 —— 这就是假通过的来源。
 * 调用前 fd 必须是 O_NONBLOCK 的。
 */
static int drain_fd(int fd)
{
	char buf[128];
	int n = 0;

	while (n < 64) {
		ssize_t ret = read(fd, buf, sizeof(buf) - 1);

		if (ret >= 0) {
			n++;
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return n;
		return -1;		/* 其它错误：调用方按失败处理 */
	}
	return n;
}

/* 非阻塞 poll(fd, 0)：返回 1 表示报可读（POLLIN），0 表示不可读，-1 表示出错 */
static int poll_readable_now(int fd)
{
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	if (poll(&pfd, 1, 0) < 0)
		return -1;

	return (pfd.revents & POLLIN) ? 1 : 0;
}

/* ---------------- 1) 阻塞 read：既要读得到，也要真的等过 ---------------- */
static int test_blocking_read_and_wait(long long *wait_ms_out, int *read_ok_out)
{
	char buf[128];
	long long start, elapsed = 0;
	unsigned int iv;
	int fd, ret, flags;

	*wait_ms_out = 0;
	*read_ok_out = 0;

	fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("[IO] blocking_read=fail (open: %s)\n", strerror(errno));
		printf("[IO] blocking_wait_ms=0\n");
		return 0;
	}

	/*
	 * 把周期拉长到 1000ms 再做阻塞测试：
	 *   周期越长，"真的等了"与"拿了旧样本立刻返回"两种实现的耗时差异越大，
	 *   断言阈值（≥500ms）就越稳。测完恢复默认周期，避免影响后面的 epoll 窗口。
	 */
	iv = BLOCK_INTERVAL_MS;
	if (ioctl(fd, SENSOR_IOC_SET_INTERVAL, &iv) < 0)
		printf("[IO] blocking_set_interval_error=%s\n", strerror(errno));

	if (drain_fd(fd) < 0)
		printf("[IO] blocking_drain_error=%s\n", strerror(errno));

	/* 切成阻塞模式（清掉 O_NONBLOCK） */
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	/*
	 * 第一次阻塞 read：目的是与"样本到达沿"对齐 —— 它返回的时刻就是样本刚被
	 * 投递的时刻，但等待时长本身不确定（取决于抽干时离上一个样本过了多久），
	 * 所以这一次的耗时不做断言。
	 */
	ret = read(fd, buf, sizeof(buf) - 1);
	if (ret > 0) {
		buf[ret] = '\0';
		printf("[IO] blocking_read=ok\n");
		printf("[IO] blocking_sample=%s", buf);
		*read_ok_out = 1;

		/* 第二次阻塞 read：此刻刚消费过样本，下一次样本要等约一个完整周期 */
		start = now_ms();
		ret = read(fd, buf, sizeof(buf) - 1);
		elapsed = now_ms() - start;
		if (ret <= 0)
			printf("[IO] blocking_second_read_error=%s\n",
			       ret < 0 ? strerror(errno) : "0 字节");
	} else {
		printf("[IO] blocking_read=fail (read: %s)\n",
		       ret < 0 ? strerror(errno) : "0 字节");
	}

	*wait_ms_out = elapsed;
	printf("[IO] blocking_wait_ms=%lld\n", elapsed);

	/* 恢复默认周期，避免影响后续 epoll 窗口统计 */
	iv = DEFAULT_INTERVAL_MS;
	ioctl(fd, SENSOR_IOC_SET_INTERVAL, &iv);
	close(fd);

	return *read_ok_out && (elapsed >= BLOCK_WAIT_MIN_MS);
}

/* ---------------- 2) 非阻塞 read：必须观察到 EAGAIN ---------------- */
static int test_nonblock_read(int *eagain_count)
{
	char buf[128];
	int fd, i, eagain = 0;

	fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("[IO] nonblock_eagain=0 (open: %s)\n", strerror(errno));
		return 0;
	}

	/*
	 * 连读多次：第一个样本会被立即取走，之后在下一个样本到来之前必须返回 EAGAIN。
	 * 若驱动把非阻塞当阻塞处理，这里会一直等下去（超时失败）；
	 * 若驱动错误地重复返回旧样本，则永远观察不到 EAGAIN。
	 */
	for (i = 0; i < 20; i++) {
		int ret = read(fd, buf, sizeof(buf) - 1);

		if (ret >= 0)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			eagain++;
			break;
		}
		printf("[IO] nonblock_error=%s\n", strerror(errno));
		break;
	}

	close(fd);
	*eagain_count = eagain;
	printf("[IO] nonblock_eagain=%d\n", eagain);
	return eagain >= 1;
}

/* ---------------- 3) epoll：首次唤醒延迟 + 唤醒次数上下界 ---------------- */
static int test_epoll(int *wakeups_out, long long *first_ms_out, int *reads_out)
{
	struct epoll_event ev;
	char buf[128];
	long long start, elapsed;
	int epfd, fd, n;
	int wakeups = 0, reads = 0, ok_first = 0;

	*wakeups_out = 0;
	*first_ms_out = EPOLL_FIRST_TIMEOUT_MS;
	*reads_out = 0;

	fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("[IO] epoll_wait_ok=0 (open: %s)\n", strerror(errno));
		return 0;
	}

	/* 关键前置：抽干未消费样本，否则 epoll_ctl(ADD) 的首轮 ->poll 会造成假通过 */
	if (drain_fd(fd) < 0)
		printf("[IO] epoll_drain_error=%s\n", strerror(errno));

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("[IO] epoll_wait_ok=0 (epoll_create1: %s)\n", strerror(errno));
		close(fd);
		return 0;
	}

	ev.events = EPOLLIN;
	ev.data.fd = fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		printf("[IO] epoll_wait_ok=0 (epoll_ctl: %s)\n", strerror(errno));
		close(epfd);
		close(fd);
		return 0;
	}

	/*
	 * 首次唤醒：此刻 fd 里确定没有未消费样本，所以唯一能让 epoll_wait 提前返回的
	 * 途径就是"驱动在新样本到达时通过 poll_wait 注册的队列把我们唤醒"。
	 * 因此延迟本身就在证明 poll_wait 是否真的登记了。
	 */
	start = now_ms();
	n = epoll_wait(epfd, &ev, 1, EPOLL_FIRST_TIMEOUT_MS);
	*first_ms_out = now_ms() - start;
	if (n < 0 && errno != EINTR) {
		printf("[IO] epoll_first_wakeup_error=%s\n", strerror(errno));
	} else if (n > 0) {
		wakeups++;
		if (read(fd, buf, sizeof(buf) - 1) > 0)
			reads++;
	}
	ok_first = (*first_ms_out <= EPOLL_FIRST_MAX_MS) && reads >= 1;

	/*
	 * 剩余窗口继续统计唤醒次数：每次被唤醒就把样本读走（模拟正常用户态处理），
	 * 这样"下一次唤醒"只可能来自"新样本"。
	 *   - 唤醒次数过少（<3）说明 poll 没有真正上报 → 上面那个假通过的另一种形态；
	 *   - 唤醒次数过多（>12）说明样本被消费后仍上报 EPOLLIN → 用户态忙轮询。
	 */
	start = now_ms();
	for (;;) {
		int remain = EPOLL_WINDOW_MS - (int)(now_ms() - start);

		if (remain <= 0)
			break;

		n = epoll_wait(epfd, &ev, 1, remain);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			printf("[IO] epoll_error=%s\n", strerror(errno));
			break;
		}
		if (n > 0) {
			wakeups++;
			if (read(fd, buf, sizeof(buf) - 1) > 0)
				reads++;
		}
	}
	elapsed = now_ms() - start;

	close(epfd);
	close(fd);

	*wakeups_out = wakeups;
	*reads_out = reads;
	printf("[IO] epoll_first_wakeup_ms=%lld\n", *first_ms_out);
	printf("[IO] epoll_wakeups=%d\n", wakeups);
	printf("[IO] epoll_reads=%d elapsed_ms=%lld\n", reads, elapsed);
	printf("[IO] epoll_wait_ok=%d\n", (wakeups > 0 && reads > 0) ? 1 : 0);

	return ok_first && wakeups >= EPOLL_WAKEUP_MIN && wakeups <= EPOLL_WAKEUP_MAX
	       && reads > 0;
}

/*
 * ---------------- 4) 电平触发语义 + per-open 独立性 ----------------
 *
 * 三段断言（缺一不可）：
 *   a) 新开的 fd 立刻 poll → 必须可读（lf->last_seq=0，全局"已消费"实现会给 0）；
 *   b) 一直不读，连续 5 次 poll → 必须次次可读（电平语义：可读状态持续存在；
 *      one-shot / 边沿式实现只能通过 1 次）；
 *   c) 读走样本后，poll → 必须立刻变为不可读（避免忙轮询）。
 */
static int test_poll_level(int *unread_hits_out, int *after_consume_out,
			   int *new_fd_ok_out)
{
	int fd, hits = 0, after_consume = 1, i, ok;

	*unread_hits_out = 0;
	*after_consume_out = 1;
	*new_fd_ok_out = 0;

	fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("[IO] new_fd_immediately_readable=0 (open: %s)\n", strerror(errno));
		printf("[IO] poll_unread_hits=0\n");
		printf("[IO] poll_after_consume=1\n");
		return 0;
	}

	/*
	 * (a) 全新 fd 应立刻报可读 —— 前提是"缓冲里此刻确实还有未读样本"。
	 *
	 * 阶段 03 起，可读性由"共享环形缓冲是否非空"决定（不再用 per-fd 的 last_seq，
	 * 因为多进程共享同一队列时 per-fd 记录会谎报可读）。因此这里先静置两个采样
	 * 周期：生产者持续投递、期间没有任何人消费，队列必然非空。
	 * 不做这一步，本项就退化成"看上一个测试结束时队列恰好空不空"——那是时序
	 * 碰运气，而不是判定。
	 */
	sleep_ms(2 * DEFAULT_INTERVAL_MS);
	if (poll_readable_now(fd) == 1)
		*new_fd_ok_out = 1;
	printf("[IO] new_fd_immediately_readable=%d\n", *new_fd_ok_out);

	/* (b) 未消费时连续探测，电平语义要求次次可读 */
	for (i = 0; i < POLL_UNREAD_PROBES; i++) {
		if (poll_readable_now(fd) == 1)
			hits++;
	}
	*unread_hits_out = hits;
	printf("[IO] poll_unread_hits=%d\n", hits);

	/*
	 * (c) 消费后应为不可读。
	 *     这里允许重试：抽干到 EAGAIN 后仍有极小概率（微秒级窗口 vs 500ms 周期）
	 *     恰好又有新样本到达，重试 3 次可把这种假失败压到可以忽略。
	 */
	for (i = 0; i < 3; i++) {
		int r;

		if (drain_fd(fd) < 0)
			break;
		r = poll_readable_now(fd);
		if (r == 0) {
			after_consume = 0;
			break;
		}
	}
	*after_consume_out = after_consume;
	printf("[IO] poll_after_consume=%d\n", after_consume);

	close(fd);

	ok = (*new_fd_ok_out == 1) && (hits == POLL_UNREAD_PROBES) && (after_consume == 0);
	return ok;
}

/* ---------------- 5) SIGIO 异步通知 ---------------- */
static int test_sigio(int *count_out)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
	sigset_t mask, oldmask;
	long long start;
	int fd, flags, sigio = 0;

	fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("[IO] sigio_count=0 (open: %s)\n", strerror(errno));
		*count_out = 0;
		return 0;
	}

	/*
	 * 用 sigtimedwait 同步等待而不是信号处理函数：
	 *   - 处理函数里不能方便地做统计（异步信号安全限制多）；
	 *   - sigtimedwait 要求信号处于阻塞状态（否则会先走默认动作/处理函数）。
	 * 注意标准信号不排队：多个 SIGIO 会合并成一个 pending，所以计数通常为 1，
	 * 因此判定标准是"收到过"（>=1）而不是"收到 N 次"。
	 */
	sigemptyset(&mask);
	sigaddset(&mask, SIGIO);
	if (sigprocmask(SIG_BLOCK, &mask, &oldmask) < 0) {
		printf("[IO] sigio_count=0 (sigprocmask: %s)\n", strerror(errno));
		close(fd);
		*count_out = 0;
		return 0;
	}

	/* F_SETOWN：指定接收 SIGIO 的进程；O_ASYNC：打开异步通知开关 */
	if (fcntl(fd, F_SETOWN, getpid()) < 0)
		printf("[IO] sigio_setown_error=%s\n", strerror(errno));

	flags = fcntl(fd, F_GETFL);
	if (fcntl(fd, F_SETFL, flags | O_ASYNC) < 0) {
		printf("[IO] sigio_setfl_error=%s\n", strerror(errno));
	} else {
		start = now_ms();
		while (now_ms() - start < SIGIO_WAIT_MS) {
			int s = sigtimedwait(&mask, NULL, &ts);

			if (s == SIGIO) {
				sigio++;
				break;		/* 收到即证明链路通，不必等满 3 秒 */
			}
			if (s < 0 && errno != EAGAIN && errno != EINTR)
				break;
		}
	}

	/* 清理：先关掉 O_ASYNC（触发驱动 fasync(on=0)），再恢复信号掩码 */
	fcntl(fd, F_SETFL, flags & ~O_ASYNC);
	sigprocmask(SIG_SETMASK, &oldmask, NULL);
	close(fd);

	*count_out = sigio;
	printf("[IO] sigio_count=%d\n", sigio);
	return sigio >= 1;
}

int main(int argc, char **argv)
{
	int eagain = 0, wakeups = 0, sigio = 0, epoll_reads = 0;
	int unread_hits = 0, after_consume = 1, new_fd_ok = 0;
	long long first_ms = 0, block_wait_ms = 0;
	int ok_block_read = 0, ok_block_wait, ok_nonblock, ok_epoll, ok_first;
	int ok_wakeup_min, ok_wakeup_max, ok_level, ok_sigio, overall;

	(void)argc;
	(void)argv;

	printf("[IO] device=%s\n", DEV_PATH);

	test_blocking_read_and_wait(&block_wait_ms, &ok_block_read);
	ok_block_wait = (ok_block_read && block_wait_ms >= BLOCK_WAIT_MIN_MS);
	ok_nonblock  = test_nonblock_read(&eagain);
	ok_epoll     = test_epoll(&wakeups, &first_ms, &epoll_reads);
	ok_first     = (first_ms <= EPOLL_FIRST_MAX_MS) && (epoll_reads >= 1);
	ok_wakeup_min = (wakeups >= EPOLL_WAKEUP_MIN);
	ok_wakeup_max = (wakeups <= EPOLL_WAKEUP_MAX);
	ok_level     = test_poll_level(&unread_hits, &after_consume, &new_fd_ok);
	ok_sigio     = test_sigio(&sigio);

	overall = ok_block_read && ok_block_wait && ok_nonblock && ok_epoll && ok_first &&
		  ok_wakeup_min && ok_wakeup_max && ok_level && ok_sigio;

	printf("[IO] checks block_read=%d block_wait=%d nonblock=%d epoll=%d "
	       "first_wakeup=%d wakeup_min=%d wakeup_max=%d level=%d sigio=%d\n",
	       ok_block_read, ok_block_wait, ok_nonblock, ok_epoll, ok_first,
	       ok_wakeup_min, ok_wakeup_max, ok_level, ok_sigio);
	printf("[IO] OVERALL=%s\n", overall ? "PASS" : "FAIL");

	return overall ? 0 : 1;
}
