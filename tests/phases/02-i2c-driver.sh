#!/bin/busybox sh
# ============================================================================
# tests/phases/02-i2c-driver.sh - 阶段 02：虚拟 I2C 控制器 + 标准 i2c_driver
#                                  + regmap + 设备树 i2c 子节点匹配
#
# 验证思路（关键是"证明链路真的走通了"，而不是"日志里出现了某个词"）：
#   1. 控制器注册：adapter_of_node 必须真的指向设备树节点（这是 i2c 核心枚举
#      子节点的前提），并按契约暴露 debugfs 故障注入/统计接口。
#   2. 子设备来自设备树枚举：从设备名必须等于 compatible 去掉厂商前缀后的
#      "sensor-char"。旧实现手工 i2c_new_client_device() 用的是 "sensor_demo"，
#      因此这一项能区分"内核枚举"与"驱动自己造"。
#   3. 驱动绑定：/sys/.../<dev>/driver 符号链接指向 sensor_char。
#   4. regmap 生效：不只看 debugfs registers 文件存在，还要校验寄存器**数值语义**
#      （温度/湿度 raw 换算后必须落在芯片模拟区间，CONFIG 的 bit0 必须是使能）。
#      这一项能抓住"regmap 字节序配错导致 16 位值被 swab16"这类错误。
#   5. 数据与配置：设备树 poll-interval-ms 真的生效（读回内核当前值）、
#      用户态读到的温度落在芯片模拟区间（证明换算与传输都对）。
#   6. 无内核告警：**必须在测试体执行之后重新取样**，否则测试体期间产生的
#      WARNING 不参与判定（取样窗口为 0 的假绿，已在验证中实证）。
# ============================================================================

info "== 1) 虚拟 I2C 控制器 =="
dmesg > /tmp/probe.txt 2>/dev/null
# virt_i2c 用 %pOF 打印的是 chip->adap.dev.of_node（NULL 时打印 (null)，实测如此；
# 注意 of_node_full_name(NULL) 才是返回 <no-node>，%pOF 不走那条路径），
# 所以这一行能真正证明“用于枚举子节点的那个字段”被设置了；
# 删掉 virt_i2c.c 里的赋值后本项会失败（负控 NC1 已实证）。
check_contains "adapter.of_node 已指向设备树节点（枚举子设备的前提）" /tmp/probe.txt "adapter.of_node=/virt-i2c"
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

info "== 4) regmap 生效（含寄存器数值语义校验）=="
REGMAPDIR=$(ls -d /sys/kernel/debug/regmap/*-0048 2>/dev/null | head -1)
check_nonempty "regmap 注册了 debugfs 节点（/sys/kernel/debug/regmap/*-0048）" "$REGMAPDIR"
if [ -n "$REGMAPDIR" ]; then
    ls "$REGMAPDIR" > /tmp/regmapfiles.txt 2>/dev/null
    check_contains "regmap 提供 registers 文件" /tmp/regmapfiles.txt "registers"
    head -3 "$REGMAPDIR/registers" > /tmp/regmapregs.txt 2>/dev/null
    info "registers 内容：$(tr '\n' ' ' < /tmp/regmapregs.txt)"

    # 逐位校验数值语义（只判"非空"是不够的：字节交换后的值同样非空）：
    #   0x00 温度 raw，LSB = 1/16 ℃，芯片模拟区间 24.0~26.0 ℃
    #        → 换算后应在 24000~26000 毫摄氏度（raw 384~416）
    #   0x01 湿度 raw，LSB = 0.01 %RH，芯片模拟区间 40.00~41.99 %RH → 4000~4199
    #   0x02 CONFIG，bit0=1 表示连续转换使能（驱动 probe 已写入使能）
    RAW_T=$(awk '$1=="0:" {print $2; exit}' "$REGMAPDIR/registers" 2>/dev/null)
    RAW_H=$(awk '$1=="1:" {print $2; exit}' "$REGMAPDIR/registers" 2>/dev/null)
    RAW_C=$(awk '$1=="2:" {print $2; exit}' "$REGMAPDIR/registers" 2>/dev/null)

    if [ -z "$RAW_T" ] || [ -z "$RAW_H" ] || [ -z "$RAW_C" ]; then
        fail "regmap registers 三个寄存器值可解析" \
             "raw_temp='$RAW_T' raw_humidity='$RAW_H' raw_config='$RAW_C'"
    else
        T_MILLI=$((0x$RAW_T * 1000 / 16))
        if [ "$T_MILLI" -ge 24000 ] && [ "$T_MILLI" -le 26000 ]; then
            pass "regmap 温度 raw 语义正确（0x$RAW_T → ${T_MILLI} m℃ 落在 24.0~26.0 ℃）"
        else
            fail "regmap 温度 raw 语义正确" \
                 "0x$RAW_T → ${T_MILLI} m℃ 超出 24.0~26.0 ℃（regmap 字节序配错会得到这种结果）"
        fi

        H_RAW_DEC=$((0x$RAW_H))
        if [ "$H_RAW_DEC" -ge 4000 ] && [ "$H_RAW_DEC" -le 4199 ]; then
            pass "regmap 湿度 raw 语义正确（0x$RAW_H → ${H_RAW_DEC}，即 40.00~41.99 %RH）"
        else
            fail "regmap 湿度 raw 语义正确" "0x$RAW_H → ${H_RAW_DEC} 超出 40.00~41.99 %RH 区间"
        fi

        if [ "$RAW_C" = "0001" ]; then
            pass "regmap CONFIG 语义正确（0x$RAW_C：bit0=1 连续转换已使能）"
        else
            fail "regmap CONFIG 语义正确" "0x$RAW_C 的 bit0 不是 1（期望 0x0001 使能连续转换）"
        fi
    fi
fi

info "== 5) 芯片探测、设备树属性与数据换算 =="
check_contains "probe 时读到芯片配置寄存器" /tmp/probe.txt "chip detected: config="
check_contains "probe 把芯片配置成连续转换（bit0=1）" /tmp/probe.txt "continuous conversion enabled (config=0x0001)"
/bin/sensor_stat > /tmp/stat0.txt 2>&1
info "$(cat /tmp/stat0.txt 2>/dev/null)"
check_contains "设备树 poll-interval-ms 生效（读回 interval=500）" /tmp/stat0.txt "interval=500"

/bin/sensor_test > /tmp/utest.txt 2>&1
check_true "sensor_test 退出码为 0" "0" "$?"
# 把用户态实际读到的温度回显到串口日志里：事后审计时不用只靠 regmap 反推温度
info "sensor_test 温度读数：$(grep -o 'temp=[0-9.-]*' /tmp/utest.txt | tr '\n' ' ')"
if grep -qE 'temp=2[456]\.' /tmp/utest.txt; then
    pass "温度读数落在芯片模拟区间 24.0~26.0 ℃"
else
    fail "温度读数落在芯片模拟区间 24.0~26.0 ℃" "实测 $(grep -o 'temp=[0-9.-]*' /tmp/utest.txt | head -1)"
fi
check_contains "用户态测试正常结束" /tmp/utest.txt "测试结束"

info "== 6) 内核告警检查（测试体之后重新取样）=="
# 必须在测试体执行完之后重新 dmesg：若复用脚本开头抓的 /tmp/probe.txt，
# 测试体（read/regmap/中断触发）期间产生的 WARNING 就完全落在取样窗口之外，
# 会出现"内核有 WARNING 却报检查通过"的假绿（已用 WARN_ON_ONCE 实证）。
dmesg > /tmp/dmesg-end.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:' /tmp/dmesg-end.txt)
check_eq "dmesg 无 WARNING/Call trace（取样覆盖整段测试）" "0" "$W"
if [ "$W" != "0" ]; then
    info "告警上下文（最多 5 行）："
    grep -nE 'WARNING:|Call trace:' /tmp/dmesg-end.txt | head -5 | sed 's/^/        /'
fi
