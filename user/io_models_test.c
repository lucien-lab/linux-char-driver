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
 * 为什么要专门测这四件事：
 *   1) 只看"read 能读到数据"是不够的：阻塞与非阻塞必须是两种可区分的行为，
 *      否则用户态无法用 epoll/线程池等方式组织 IO；
 *   2) poll 必须是电平触发语义（消费后不再上报），否则 epoll_wait 会立刻返回，
 *      用户态陷入忙轮询 —— 本程序用"3 秒内被唤醒次数"把这个错误暴露出来；
 *   3) SIGIO 链路（F_SETOWN + O_ASYNC + kill_fasync）任何一环断了都收不到信号。
 *
 * 输出格式（测试脚本按行解析，键名不要改）：
 *   [IO] blocking_read=ok
 *   [IO] nonblock_eagain=<次数>
 *   [IO] epoll_wait_ok=1
 *   [IO] epoll_wakeups=<3 秒内被唤醒次数>
 *   [IO] sigio_count=<收到的 SIGIO 次数>
 *   [IO] OVERALL=PASS|FAIL
 *
 * 编译（由 scripts/13-vm-fast-cycle.sh 自动完成）：
 *   gcc -static -O2 -Wall -o io_models_test io_models_test.c
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
#include <sys/epoll.h>

#define DEV_PATH	"/dev/sensor0"
#define EPOLL_WINDOW_MS	3000	/* 统计 epoll 唤醒次数的时间窗口 */
#define SIGIO_WAIT_MS	3000	/* 等待 SIGIO 的最长时间 */
#define EPOLL_WAKEUP_MAX 12	/* 3 秒 / 500ms 周期 ≈ 6~7 次，给足余量；超过即认为在忙轮询 */

/* 单调时钟毫秒，用于统计时间窗口 */
static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 1) 阻塞 read ---------------- */
static int test_blocking_read(void)
{
	char buf[128];
	int fd, ret;

	fd = open(DEV_PATH, O_RDWR);		/* 不带 O_NONBLOCK = 阻塞模式 */
	if (fd < 0) {
		printf("[IO] blocking_read=fail (open: %s)\n", strerror(errno));
		return 0;
	}

	ret = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (ret > 0) {
		buf[ret] = '\0';
		printf("[IO] blocking_read=ok\n");
		printf("[IO] blocking_sample=%s", buf);	/* 附带打印实测样本，便于人工核对 */
		return 1;
	}

	printf("[IO] blocking_read=fail (read: %s)\n",
	       ret < 0 ? strerror(errno) : "0 字节");
	return 0;
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
	 * 连读多次：第一个样本会被立即取走，之后在下一个 500ms 样本到来之前
	 * 必须返回 EAGAIN。若驱动把非阻塞当阻塞处理，这里会一直等下去（超时失败）；
	 * 若驱动错误地返回旧样本，则永远观察不到 EAGAIN。
	 */
	for (i = 0; i < 20; i++) {
		int ret = read(fd, buf, sizeof(buf) - 1);

		if (ret >= 0)
			continue;			/* 读到了样本，继续 */
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

/* ---------------- 3) epoll 多路复用 + 电平触发判定 ---------------- */
static int test_epoll(int *wakeups_out)
{
	struct epoll_event ev;
	char buf[128];
	long long start, elapsed;
	int epfd, fd, wakeups = 0, reads = 0;

	fd = open(DEV_PATH, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("[IO] epoll_wait_ok=0 (open: %s)\n", strerror(errno));
		*wakeups_out = 0;
		return 0;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("[IO] epoll_wait_ok=0 (epoll_create1: %s)\n", strerror(errno));
		close(fd);
		*wakeups_out = 0;
		return 0;
	}

	ev.events = EPOLLIN;
	ev.data.fd = fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		printf("[IO] epoll_wait_ok=0 (epoll_ctl: %s)\n", strerror(errno));
		close(epfd);
		close(fd);
		*wakeups_out = 0;
		return 0;
	}

	/*
	 * 统计窗口：3 秒。每次被唤醒了就把样本读走（模拟正常用户态处理），
	 * 这样"下一次唤醒"只可能来自"新样本"。
	 * 若驱动 poll 实现错误（始终报可读），间隔会被 epoll_wait 立刻返回，
	 * 3 秒内唤醒次数会达到数千次 —— 这正是本项要抓的 bug。
	 */
	start = now_ms();
	for (;;) {
		int remain = EPOLL_WINDOW_MS - (int)(now_ms() - start);
		int n;

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
	printf("[IO] epoll_wakeups=%d\n", wakeups);
	printf("[IO] epoll_reads=%d elapsed_ms=%lld\n", reads, elapsed);
	printf("[IO] epoll_wait_ok=%d\n", (wakeups > 0 && reads > 0) ? 1 : 0);

	return (wakeups > 0 && reads > 0);
}

/* ---------------- 4) SIGIO 异步通知 ---------------- */
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
	int eagain = 0, wakeups = 0, sigio = 0;
	int ok_block, ok_nonblock, ok_epoll, ok_sigio, ok_wakeup, overall;

	(void)argc;
	(void)argv;

	printf("[IO] device=%s\n", DEV_PATH);

	ok_block    = test_blocking_read();
	ok_nonblock = test_nonblock_read(&eagain);
	ok_epoll    = test_epoll(&wakeups);
	ok_sigio    = test_sigio(&sigio);

	/* 忙轮询判定：唤醒次数必须接近"样本数"而不是"系统调用次数" */
	ok_wakeup = (wakeups <= EPOLL_WAKEUP_MAX);

	overall = ok_block && ok_nonblock && ok_epoll && ok_wakeup && ok_sigio;
	printf("[IO] checks blocking=%d nonblock=%d epoll=%d level_triggered=%d sigio=%d\n",
	       ok_block, ok_nonblock, ok_epoll, ok_wakeup, ok_sigio);
	printf("[IO] OVERALL=%s\n", overall ? "PASS" : "FAIL");

	return overall ? 0 : 1;
}
