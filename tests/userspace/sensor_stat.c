// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_stat.c - 读取驱动统计信息的小工具（供阶段测试脚本使用）
 *
 * 为什么需要它：
 *   阶段测试要断言"行为"而不是"日志"——例如"设备树里的 poll-interval-ms
 *   是否真的生效"，最可靠的方式是把内核里的当前值读回来比较，
 *   而不是在 dmesg 里 grep 一行 printk。
 *   （user/sensor_test.c 是给人看的演示程序，它中途会把周期改成 200ms，
 *     不适合用来断言初始值，所以单独提供这个只读的小工具。）
 *
 * 用法：/bin/sensor_stat [设备路径]     默认 /dev/sensor0
 * 输出：一行可解析文本
 *   [STAT] open=N read=N irq=N i2c_err=N interval=N ring_count=N dropped=N
 *   （ring_count/dropped 为阶段 03 新增的环形缓冲统计）
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "sensor_ioctl.h"

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "/dev/sensor0";
	struct sensor_stats st;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (ioctl(fd, SENSOR_IOC_GET_STATS, &st) < 0) {
		perror("GET_STATS");
		close(fd);
		return 1;
	}

	printf("[STAT] open=%u read=%u irq=%u i2c_err=%u interval=%u ring_count=%u ring_capacity=%u dropped=%u\n",
	       st.open_count, st.read_count, st.irq_count,
	       st.i2c_errors, st.interval_ms,
	       st.ring_count, st.ring_capacity, st.kfifo_dropped);

	close(fd);
	return 0;
}
