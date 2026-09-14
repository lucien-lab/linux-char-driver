// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_kunit.c - sensor_calc.h 的内核单元测试（KUnit）
 *
 * ============================ 为什么值得单独写单元测试 ============================
 * 这个项目前面的测试（阶段 01~05）都在 QEMU 里"跑系统再观察行为"，
 * 属于集成测试：能证明"整条链路能工作"，但有两个盲区：
 *   1. 边界值很难自然出现（例如 -128 ℃、周期恰好 60000ms、环缓冲恰好回绕）；
 *   2. 失败时只能看到"行为不对"，定位到具体函数要额外排查。
 * KUnit 正好补这两点：直接在目标环境（真实内核）里对纯函数做断言，
 * 失败信息会精确指出哪个用例、哪个表达式、期望值与非期望值。
 *
 * 与用户态单元测试的区别（面试常问）：
 *   * KUnit 跑在**内核里**，用的是内核的类型与宏，测的是真正编译进驱动的代码；
 *   * 不需要为测试而把函数导出成模块符号（本项目用 static inline 头文件共享）；
 *   * 输出是 TAP（ok N - / not ok N -），能被 CI 与 kunit.py 解析。
 *
 * 覆盖范围（只覆盖纯逻辑，这是刻意的：能测什么、不能测什么都写在实现文档里）：
 *   * sensor_raw_to_milli()：零点、12 位补码正/负边界、芯片实际工作区间、
 *     以及"高 4 位是状态位不能参与算术"这条约定；
 *   * sensor_interval_valid()：1/60000 两个端点与越界；
 *   * sensor_fifo_next()：正常推进、回绕、容量为 0 的防御分支。
 * 不覆盖：regmap 传输、中断上下文、runtime PM 时序 —— 这些只能在 QEMU 集成测试里验证。
 * ================================================================================
 */

#include <kunit/test.h>
#include <linux/module.h>

#include "sensor_calc.h"

/*
 * 温度换算：12 位补码的四个边界。
 *   0x0000 -> 0         （零点）
 *   0x07FF -> +127.9375 ℃（12 位能表示的最大正数）
 *   0x0800 -> -128 ℃     （12 位补码的最小值；若忘了符号扩展会算成 +2048*62.5）
 *   0x0FFF -> -0.0625 ℃  （最小负步进；整数除法向零截断得到 -62 m℃）
 */
static void sensor_raw_to_milli_boundaries(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 0, sensor_raw_to_milli(0x0000));
	KUNIT_EXPECT_EQ(test, 127937, sensor_raw_to_milli(0x07FF));
	KUNIT_EXPECT_EQ(test, -128000, sensor_raw_to_milli(0x0800));
	KUNIT_EXPECT_EQ(test, -62, sensor_raw_to_milli(0x0FFF));
}

/*
 * 芯片实际工作区间（virt_i2c 模拟 24.0~26.0 ℃）必须落在预期值上。
 * 这条用例能挡住"换算系数写错"（例如把 1/16 写成 1/10）这类错误：
 * 只要系数错，24.0 ℃ 就不再等于 24000 m℃。
 */
static void sensor_raw_to_milli_chip_range(struct kunit *test)
{
	/* 24.0 ℃ -> 24*16 = 384 = 0x0180 */
	KUNIT_EXPECT_EQ(test, 24000, sensor_raw_to_milli(0x0180));
	/* 26.0 ℃ -> 26*16 = 416 = 0x01A0 */
	KUNIT_EXPECT_EQ(test, 26000, sensor_raw_to_milli(0x01A0));
	/* 25.312 ℃ 这类带小数的值：405 = 25.3125 ℃ */
	KUNIT_EXPECT_EQ(test, 25312, sensor_raw_to_milli(405));
}

/*
 * 约定：只有低 12 位是温度数据，高 4 位是状态/保留位，不参与算术。
 * 人为把高 4 位写成 1（0x8190）后结果必须与 0x0190 完全一致。
 * 若实现里忘了掩码，这条会失败 —— 真机上表现为"某些状态位组合下温度跳变"。
 */
static void sensor_raw_to_milli_ignores_high_bits(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 25000, sensor_raw_to_milli(0x0190));
	KUNIT_EXPECT_EQ(test, 25000, sensor_raw_to_milli(0x8190));
	KUNIT_EXPECT_EQ(test, 25000, sensor_raw_to_milli(0xF190));
}

/* 周期合法性：两个端点都必须合法，端点外一位必须非法 */
static void sensor_interval_valid_bounds(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, sensor_interval_valid(0));
	KUNIT_EXPECT_TRUE(test, sensor_interval_valid(SENSOR_INTERVAL_MIN_MS));
	KUNIT_EXPECT_TRUE(test, sensor_interval_valid(SENSOR_INTERVAL_MIN_MS + 1));
	KUNIT_EXPECT_TRUE(test, sensor_interval_valid(SENSOR_INTERVAL_MAX_MS - 1));
	KUNIT_EXPECT_TRUE(test, sensor_interval_valid(SENSOR_INTERVAL_MAX_MS));
	KUNIT_EXPECT_FALSE(test, sensor_interval_valid(SENSOR_INTERVAL_MAX_MS + 1));
	/* 极端值：绝不能因为溢出而把 ULONG_MAX 判成合法 */
	KUNIT_EXPECT_FALSE(test, sensor_interval_valid(~0UL));
}

/* 环形下标推进：正常推进、回绕、容量为 0 的防御分支 */
static void sensor_fifo_next_wraps(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 1u, sensor_fifo_next(0, 64));
	KUNIT_EXPECT_EQ(test, 32u, sensor_fifo_next(31, 64));
	KUNIT_EXPECT_EQ(test, 0u, sensor_fifo_next(63, 64));	/* 回绕点 */
	KUNIT_EXPECT_EQ(test, 0u, sensor_fifo_next(0, 1));	/* 容量 1：永远是 0 */
	KUNIT_EXPECT_EQ(test, 0u, sensor_fifo_next(7, 0));	/* 容量 0：防御除零 */
}

static struct kunit_case sensor_calc_test_cases[] = {
	KUNIT_CASE(sensor_raw_to_milli_boundaries),
	KUNIT_CASE(sensor_raw_to_milli_chip_range),
	KUNIT_CASE(sensor_raw_to_milli_ignores_high_bits),
	KUNIT_CASE(sensor_interval_valid_bounds),
	KUNIT_CASE(sensor_fifo_next_wraps),
	{}
};

/*
 * 套件名会出现在 TAP 输出里（# Subtest: sensor_calc），
 * 测试脚本按它判断"这一版用例确实跑过了"，而不是"dmesg 里恰好有 ok 字样"。
 */
static struct kunit_suite sensor_calc_test_suite = {
	.name = "sensor_calc",
	.test_cases = sensor_calc_test_cases,
};

kunit_test_suite(sensor_calc_test_suite);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lucien");
MODULE_DESCRIPTION("KUnit unit tests for sensor_calc.h (pure logic of the sensor driver)");
MODULE_VERSION("1.0");
