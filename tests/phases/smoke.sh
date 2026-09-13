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
check_contains "设备树解析到 i2c-bus/sensor-addr" /tmp/probe.txt "DT: i2c-bus=0 sensor-addr=0x48"
check_contains "I2C 客户端创建成功" /tmp/probe.txt "i2c client on bus 0 addr 0x48"
check_contains "虚拟中断请求注册成功" /tmp/probe.txt "irq registered: virq="
check_contains "字符设备注册成功" /tmp/probe.txt "probe done: major="

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
