/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sensor_ioctl.h - 驱动与用户态共享的 ioctl 接口定义
 *
 * 规范做法：接口定义只写一份，内核模块和用户态程序都 include 它，
 * 避免两边结构体/命令号不一致导致"数据看起来正常但其实是错的"。
 */
#ifndef _SENSOR_IOCTL_H
#define _SENSOR_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>	/* 提供 __u32/__s32/__u64（内核态与用户态都能找到） */

#define SENSOR_IOC_MAGIC	'S'

/* 一个采样点：read()/ioctl 返回的内核态数据结构 */
struct sensor_sample {
	__u32 seq;		/* 采样序号，从 1 递增 */
	__s32 temp_milli;	/* 温度，单位 0.001 摄氏度（25.312C -> 25312） */
	__u32 irq_count;	/* 累计中断次数 */
	__u64 ts_ns;		/* 采样时间戳（CLOCK_MONOTONIC，纳秒） */
};

/* 统计信息 */
struct sensor_stats {
	__u32 open_count;	/* open() 次数 */
	__u32 read_count;	/* read() 次数 */
	__u32 irq_count;	/* 中断次数 */
	__u32 i2c_errors;	/* I2C 传输失败次数 */
	__u32 interval_ms;	/* 当前采样周期 */
};

/* 读一个采样点（立即返回缓存值） */
#define SENSOR_IOC_GET_SAMPLE	_IOR(SENSOR_IOC_MAGIC, 1, struct sensor_sample)
/* 设置采样周期，参数为 __u32 毫秒 */
#define SENSOR_IOC_SET_INTERVAL	_IOW(SENSOR_IOC_MAGIC, 2, __u32)
/* 读统计信息 */
#define SENSOR_IOC_GET_STATS	_IOR(SENSOR_IOC_MAGIC, 3, struct sensor_stats)
/* 清空统计计数、重置采样序号 */
#define SENSOR_IOC_RESET	_IO(SENSOR_IOC_MAGIC, 4)

#endif /* _SENSOR_IOCTL_H */
