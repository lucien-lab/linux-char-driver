#!/bin/busybox sh
# ============================================================================
# init-demo.sh - initramfs 的 init 脚本：开机后自动跑完整演示并打印结果
#
# 演示内容（对应项目要求的三块能力）：
#   [1] 内核启动
#   [2] 加载驱动 -> probe 日志（设备树匹配 / I2C / 字符设备 / 中断注册）
#   [3] ftrace 追踪线程化中断处理函数（内核态调试）
#   [4] 用户态程序：read/ioctl/write 访问 /dev/sensor0
#   [5] 手动触发中断验证 + dmesg 检查是否有内核告警
#   [6] rmmod 卸载
# ============================================================================
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mount -t debugfs none /sys/kernel/debug 2>/dev/null

echo ""
echo "=== [1] BOOT: $(uname -r) / $(uname -m) ==="

echo "=== [2] 加载驱动（i2c-stub 提供虚拟 I2C 总线，slave 地址 0x48）==="
insmod /lib/modules/6.6.156/i2c-stub.ko chip_addr=0x48
insmod /lib/modules/6.6.156/sensor_char.ko
echo "--- probe 日志 ---"
dmesg | grep sensor_char | tail -6
echo "--- 设备节点与 sysfs ---"
ls -l /dev/sensor0
ls /sys/class/sensor_char/

echo "=== [3] ftrace 追踪 sensor_irq_thread（线程化中断下半部）==="
echo 0 > /sys/kernel/debug/tracing/tracing_on
echo function > /sys/kernel/debug/tracing/current_tracer
echo sensor_irq_thread > /sys/kernel/debug/tracing/set_ftrace_filter
echo 1 > /sys/kernel/debug/tracing/tracing_on
sleep 2
echo 0 > /sys/kernel/debug/tracing/tracing_on
echo "  捕获次数: $(grep -c sensor_irq_thread /sys/kernel/debug/tracing/trace)"
tail -5 /sys/kernel/debug/tracing/trace
echo nop > /sys/kernel/debug/tracing/current_tracer

echo "=== [4] 用户态测试程序 sensor_test ==="
/bin/sensor_test

echo "=== [5] write(0x01) 手动触发采样 + 检查内核告警 ==="
printf '\001' > /dev/sensor0
sleep 1
echo "  内核告警数量: $(dmesg | grep -c 'WARNING\|Call trace')"

echo "=== [6] rmmod 卸载 ==="
rmmod sensor_char && echo "  rmmod OK"
dmesg | tail -2

echo "=== [7] 演示结束 ==="
poweroff -f
