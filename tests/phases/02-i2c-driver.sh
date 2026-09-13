#!/bin/busybox sh
# ============================================================================
# tests/phases/02-i2c-driver.sh - 阶段 02：虚拟 I2C 控制器 + 标准 i2c_driver
#                                  + regmap + 设备树 i2c 子节点匹配
#
# 验证思路（关键是"证明链路真的走通了"，而不是"日志里出现了某个词"）：
#   1. 控制器注册 → 总线设备与 debugfs 接口真实存在
#   2. 子设备来自设备树枚举 → 从设备名必须等于 compatible 去掉厂商前缀后的
#      "sensor-char"。旧实现手工 i2c_new_client_device() 用的是 "sensor_demo"，
#      因此这一项能区分"内核枚举"与"驱动自己造"。
#   3. 驱动绑定 → /sys/.../<dev>/driver 符号链接指向 sensor_char
#   4. regmap 生效 → regmap 自己创建的 debugfs 节点与 registers 文件可读
#   5. 数据与配置 → 设备树 poll-interval-ms 真的生效（读回内核当前值）、
#      温度落在芯片模拟区间（证明换算与传输都对）
#   6. 无内核告警
# ============================================================================

info "== 1) 虚拟 I2C 控制器 =="
dmesg > /tmp/probe.txt 2>/dev/null
check_contains "控制器注册日志（adapter 带 of_node）" /tmp/probe.txt "registered i2c adapter i2c-"
check_exists "故障注入接口 /sys/kernel/debug/virt_i2c/inject_error" /sys/kernel/debug/virt_i2c/inject_error
check_exists "控制器统计接口 /sys/kernel/debug/virt_i2c/stats" /sys/kernel/debug/virt_i2c/stats

info "== 2) 从设备由设备树枚举（不是驱动手工创建）=="
I2CDEV=$(ls -d /sys/bus/i2c/devices/*-0048 2>/dev/null | head -1)
check_nonempty "子节点 sensor@48 被实例化成 i2c 从设备" "$I2CDEV"
cat "$I2CDEV/name" > /tmp/i2cname.txt 2>/dev/null
check_contains "从设备名来自设备树 compatible（sensor-char）" /tmp/i2cname.txt "sensor-char"
info "从设备节点：$I2CDEV，name=$(cat /tmp/i2cname.txt 2>/dev/null)"

info "== 3) 驱动以 i2c_driver 绑定 =="
readlink -f "$I2CDEV/driver" > /tmp/i2cdrv.txt 2>/dev/null
check_contains "driver 符号链接指向 sensor_char" /tmp/i2cdrv.txt "sensor_char"
check_contains "probe 由 i2c 核心调用（含从机地址与 DT 周期）" /tmp/probe.txt "probe: i2c client addr=0x48 interval=500ms"

info "== 4) regmap 生效 =="
REGMAPDIR=$(ls -d /sys/kernel/debug/regmap/*-0048 2>/dev/null | head -1)
check_nonempty "regmap 注册了 debugfs 节点（/sys/kernel/debug/regmap/*-0048）" "$REGMAPDIR"
if [ -n "$REGMAPDIR" ]; then
    ls "$REGMAPDIR" > /tmp/regmapfiles.txt 2>/dev/null
    check_contains "regmap 提供 registers 文件" /tmp/regmapfiles.txt "registers"
    head -3 "$REGMAPDIR/registers" > /tmp/regmapregs.txt 2>/dev/null
    info "registers 前几行：$(tr '\n' ' ' < /tmp/regmapregs.txt)"
    check_nonempty "能用 regmap 读出寄存器内容" "$(cat /tmp/regmapregs.txt)"
fi

info "== 5) 芯片探测、设备树属性与数据换算 =="
check_contains "probe 时读到芯片配置寄存器" /tmp/probe.txt "chip detected: config="
/bin/sensor_stat > /tmp/stat0.txt 2>&1
info "$(cat /tmp/stat0.txt 2>/dev/null)"
check_contains "设备树 poll-interval-ms 生效（读回 interval=500）" /tmp/stat0.txt "interval=500"

/bin/sensor_test > /tmp/utest.txt 2>&1
check_true "sensor_test 退出码为 0" "0" "$?"
if grep -qE 'temp=2[456]\.' /tmp/utest.txt; then
    pass "温度读数落在芯片模拟区间 24.0~26.0 ℃"
else
    fail "温度读数落在芯片模拟区间 24.0~26.0 ℃" "实测 $(grep -o 'temp=[0-9.-]*' /tmp/utest.txt | head -1)"
fi
check_contains "用户态测试正常结束" /tmp/utest.txt "测试结束"

info "== 6) 内核告警检查 =="
W=$(grep -cE 'WARNING:|Call trace:' /tmp/probe.txt)
check_eq "dmesg 无 WARNING/Call trace" "0" "$W"
