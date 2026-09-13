#!/bin/busybox sh
# ============================================================================
# tests/phases/smoke.sh - 基线回归测试（改造前的原始驱动必须通过这些检查）
#
# 作用：每次改动驱动后先跑这个，确保没有把已有功能改坏。
#       它是整个测试体系的地基——如果 smoke 挂了，后面阶段的结论都不可信。
#
# 检查内容：
#   1. 设备节点 /dev/sensor0 由驱动创建
#   2. probe 全流程日志（设备树解析 → I2C 客户端 → 中断注册 → 字符设备注册）
#   3. 用户态程序 sensor_test 的 read/ioctl/write 通路可用
#   4. ftrace 能抓到线程化中断下半部 sensor_irq_thread
#   5. 内核无 WARNING / Call trace
# ============================================================================

info "== 1) 设备节点 =="
check_exists "设备节点 /dev/sensor0 存在" /dev/sensor0

info "== 2) probe 流程日志 =="
dmesg > /tmp/probe.txt 2>/dev/null
check_contains "从设备树读到从机地址与采样周期" /tmp/probe.txt "probe: i2c client addr=0x48 interval=500ms"
check_contains "芯片在位探测成功（regmap 读配置寄存器）" /tmp/probe.txt "chip detected: config="
check_contains "虚拟中断请求注册成功" /tmp/probe.txt "irq registered: virq="
check_contains "字符设备注册成功" /tmp/probe.txt "probe done: major="

info "== 2b) I2C 从设备由设备树枚举并绑定到驱动 =="
# 阶段 02 把平台驱动改成了标准 i2c_driver，这里的检查项相应地从
# "看某句日志" 换成 "看内核真的建出了 i2c 从设备并绑定了驱动"：
# 功能性等价（甚至更强），因为它验证的是内核对象而不是 printk。
I2CDEV=$(ls -d /sys/bus/i2c/devices/*-0048 2>/dev/null | head -1)
check_nonempty "i2c 从设备节点存在（<bus>-0048）" "$I2CDEV"
if [ -n "$I2CDEV" ]; then
    cat "$I2CDEV/name" > /tmp/i2cname.txt 2>/dev/null
    check_contains "从设备名来自设备树 compatible（sensor-char）" /tmp/i2cname.txt "sensor-char"
    readlink -f "$I2CDEV/driver" > /tmp/i2cdrv.txt 2>/dev/null
    check_contains "从设备已绑定到 sensor_char 驱动" /tmp/i2cdrv.txt "sensor_char"
fi

info "== 3) 用户态数据通路（read / ioctl / write）=="
/bin/sensor_test > /tmp/utest.txt 2>&1
check_true "sensor_test 退出码为 0" "0" "$?"
check_contains "read() 返回带 seq 的样本" /tmp/utest.txt "seq="
check_contains "ioctl(GET_SAMPLE) 成功" /tmp/utest.txt "== 3)"
check_contains "ioctl(GET_STATS) 输出统计" /tmp/utest.txt "open="
check_contains "测试正常结束" /tmp/utest.txt "测试结束"

info "== 4) ftrace 追踪线程化中断下半部 =="
TR=/sys/kernel/debug/tracing
if [ -d "$TR" ]; then
    echo 0 > $TR/tracing_on
    echo function > $TR/current_tracer
    echo sensor_irq_thread > $TR/set_ftrace_filter 2>/dev/null
    echo 1 > $TR/tracing_on
    sleep 2
    echo 0 > $TR/tracing_on
    N=$(grep -c sensor_irq_thread $TR/trace 2>/dev/null)
    check_gt "ftrace 抓到 sensor_irq_thread（次数 $N）" "${N:-0}" 0
    echo nop > $TR/current_tracer
else
    fail "debugfs/tracing 可用" "$TR 不存在"
fi

info "== 5) 内核告警检查 =="
dmesg > /tmp/dmesg2.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:' /tmp/dmesg2.txt)
check_eq "dmesg 无 WARNING/Call trace" "0" "$W"
