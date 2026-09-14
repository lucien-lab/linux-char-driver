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

/*
 * mmap 共享区布局（阶段 03）。内核与用户态共用这一份定义，避免两边布局漂移
 * —— 与 ioctl 结构体同样的理由：布局不一致会导致"看起来正常其实错位"的读数。
 *
 * 为什么用"固定布局 + seqlock"而不是让用户态直接 mmap 整个 kfifo：
 *   kfifo 的内部结构（in/out 索引、环形数组）在内核版本间可能变化，
 *   把它暴露给用户态等于把内核内部实现变成 ABI。这里用稳定的自定义布局
 *   （magic/version 开头，字段语义写死在文档里），内核升级不影响用户态。
 *
 * seq 的语义（seqlock 约定）：偶数=数据一致，奇数=正在写入。
 * 用户态读法：读 seq → 读数据 → 再读 seq；两次相同且为偶数才采信，否则重试。
 */
#define SENSOR_SHM_SAMPLES	32
#define SENSOR_SHM_MAGIC	0x53454E53u	/* "SENS" */
#define SENSOR_SHM_VERSION	1u

struct sensor_shm {
	__u32 magic;			/* SENSOR_SHM_MAGIC */
	__u32 version;			/* SENSOR_SHM_VERSION */
	__u32 count;			/* 有效样本数（<= SENSOR_SHM_SAMPLES） */
	__u32 write_idx;		/* 下一个写入位置 */
	__u32 seq;			/* seqlock 序号：偶数=一致，奇数=写入中 */
	__u32 reserved[3];		/* 预留：后续加字段不改变 samples 的偏移 */
	struct sensor_sample samples[SENSOR_SHM_SAMPLES];
};

/* 统计信息 */
struct sensor_stats {
	__u32 open_count;	/* open() 次数 */
	__u32 read_count;	/* read() 次数 */
	__u32 irq_count;	/* 中断次数 */
	__u32 i2c_errors;	/* I2C 传输失败次数 */
	__u32 interval_ms;	/* 当前采样周期 */
	/*
	 * 阶段 03 追加（ABI 兼容：只追加字段，不改变已有字段的偏移）：
	 *   kfifo_dropped：环形缓冲满而丢弃的新样本数（溢出策略"丢新不丢旧"）
	 *   ring_count   ：当前缓冲中尚未被读走的样本数
	 * 这两个值让"采样速率与读取速率失配"变成可观测量：
	 * 只要 dropped 持续增长，就说明读方跟不上采样节奏。
	 */
	__u32 kfifo_dropped;
	__u32 ring_count;
	/*
	 * 环形缓冲的**实际**容量（样本个数）。
	 * 为什么不是 64：kfifo_alloc 把字节容量向上取整到 2 的幂，
	 * 请求 64×24=1536 字节 → 实际 2048 字节 → 85 个样本。
	 * 暴露它的意义：用户态才能判断"是不是真的满了"
	 * （dropped>0 时必然 ring_count == ring_capacity，这是一个强不变量）。
	 */
	__u32 ring_capacity;
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
