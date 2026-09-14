#!/bin/busybox sh
# ============================================================================
# tests/phases/04-sysfs-debugfs.sh - 阶段 04：sysfs 设备参数 + debugfs 运行统计
#
# 验证什么：
#   1. sysfs 四个属性存在、可读、语义正确（interval_ms 可写且立即生效、边界与非法值）
#   2. sysfs 与 ioctl 两个入口对同一份数据给出一致结果（防止两处判定漂移）
#   3. debugfs 三个文件（stats/regs/ring）内容格式正确
#   4. regs 走的是与采样相同的 regmap 通路（数值语义可校验，能抓字节序类错误）
#   5. 配合 virt_i2c 的故障注入验证驱动的错误处理路径真的被执行到
#   6. 注入后系统仍正常（用户态通路没有被错误处理搞坏）
#
# 设计要点（来自前面阶段的教训）：
#   * 告警检查必须在**测试体执行之后**重新取样，否则测试期间产生的 WARNING
#     落在判定之外，形成假绿（阶段 02 的教训）。
#   * 每次故障注入前先不要读 regs/其它会发起 I2C 事务的接口，
#     否则注入的失败次数会被那些读操作消耗掉，结论就错了。
# ============================================================================

SYS=/sys/class/sensor_char/sensor0
DBG=/sys/kernel/debug/sensor_char

info "== 1) sysfs 属性齐全 =="
check_exists "sysfs interval_ms"   $SYS/interval_ms
check_exists "sysfs seq"           $SYS/seq
check_exists "sysfs i2c_errors"    $SYS/i2c_errors
check_exists "sysfs ring_capacity" $SYS/ring_capacity

info "== 2) sysfs 可读且是数字 =="
IV=$(cat $SYS/interval_ms 2>/dev/null)
SEQ=$(cat $SYS/seq 2>/dev/null)
IE=$(cat $SYS/i2c_errors 2>/dev/null)
RC=$(cat $SYS/ring_capacity 2>/dev/null)
info "读到的值：interval_ms=$IV seq=$SEQ i2c_errors=$IE ring_capacity=$RC"
check_true "interval_ms 是数字（${IV}）"  "$(echo "$IV" | grep -cE '^[0-9]+$')" "1"
check_true "seq 是数字（${SEQ}）"          "$(echo "$SEQ" | grep -cE '^[0-9]+$')" "1"
check_true "i2c_errors 是数字（${IE}）"    "$(echo "$IE" | grep -cE '^[0-9]+$')" "1"
check_true "ring_capacity 是数字（${RC}）" "$(echo "$RC" | grep -cE '^[0-9]+$')" "1"
# 与阶段 03 对齐：容量必须是 kfifo 向上取整后的真实值（85），不能是请求值 64
check_eq "ring_capacity 为真实容量 85（非请求值 64）" "85" "$RC"

# ring_capacity 必须反映 kfifo 的**实际**容量（向上取整到 2 的幂后是 85），
# 而不是驱动请求的 64：用户态要用它判断"满没满"，必须拿到真实值。
check_gt "ring_capacity 反映 kfifo 实际容量（$RC >= 64）" "${RC:-0}" 63

info "== 3) sysfs 可写且立即生效（与 ioctl 看到同一份数据）=="
echo 100 > $SYS/interval_ms 2>/tmp/w1.txt
IV2=$(cat $SYS/interval_ms 2>/dev/null)
/bin/sensor_stat > /tmp/stat04.txt 2>&1
ioctl_iv=$(tag_val /tmp/stat04.txt interval 0)
info "写入 100 后：sysfs=$IV2  ioctl=$ioctl_iv"
check_eq "sysfs 读回新值=100" "100" "$IV2"
check_eq "ioctl(GET_STATS) 读到同一值（两入口一致）" "100" "$ioctl_iv"

info "== 4) 边界与非法值 =="
# 下界 1 必须接受（契约是 1~60000），上界 60000 必须接受
echo 1 > $SYS/interval_ms 2>/tmp/w2.txt
check_eq "下界 1ms 被接受" "1" "$(cat $SYS/interval_ms)"
echo 60000 > $SYS/interval_ms 2>/tmp/w3.txt
check_eq "上界 60000ms 被接受" "60000" "$(cat $SYS/interval_ms)"

# 非法值必须被拒（内核返回 -EINVAL），且不能悄悄改成别的值
echo 0 > $SYS/interval_ms 2>/tmp/w4.txt
check_eq "0ms 被拒绝（值保持不变）" "60000" "$(cat $SYS/interval_ms)"
check_contains "0ms 写入报错信息包含 Invalid 或 EINVAL 线索" /tmp/w4.txt "nvalid"
echo 60001 > $SYS/interval_ms 2>/tmp/w5.txt
check_eq "60001ms 被拒绝（值保持不变）" "60000" "$(cat $SYS/interval_ms)"

# 写非数字也必须被拒（kstrtoul 的报错路径，而不是被当成 0）
echo abc > $SYS/interval_ms 2>/tmp/w6.txt
check_eq "非数字被拒绝（值保持不变）" "60000" "$(cat $SYS/interval_ms)"

# 恢复到较快的采样周期，便于后面的故障注入检查快速看到效果
echo 20 > $SYS/interval_ms 2>/dev/null

info "== 5) debugfs 文件齐全且内容完整 =="
check_exists "debugfs stats" $DBG/stats
check_exists "debugfs ring"  $DBG/ring
check_exists "debugfs regs"  $DBG/regs

cat $DBG/stats > /tmp/dstats.txt 2>&1
sed 's/^/    /' /tmp/dstats.txt
check_contains "stats 含 open="     /tmp/dstats.txt "open="
check_contains "stats 含 irq="      /tmp/dstats.txt "irq="
check_contains "stats 含 i2c_err="  /tmp/dstats.txt "i2c_err="
check_contains "stats 含 dropped="  /tmp/dstats.txt "dropped="
check_contains "stats 含 ring_count=" /tmp/dstats.txt "ring_count="
check_contains "stats 含 seq="      /tmp/dstats.txt "seq="
# stats 里的 interval 必须就是刚写进去的 20（同一份数据，不是另抄一份）
check_contains "stats 的 interval 与 sysfs 一致（interval=20）" /tmp/dstats.txt "interval=20"

info "== 6) debugfs regs：通过 regmap 现场读寄存器 =="
cat $DBG/regs > /tmp/dregs.txt 2>&1
sed 's/^/    /' /tmp/dregs.txt
check_contains "打印了温度寄存器 00" /tmp/dregs.txt "00: "
check_contains "打印了配置寄存器 02" /tmp/dregs.txt "02: "
check_not_contains "寄存器读取没有报错（无 read error）" /tmp/dregs.txt "read error"
# 数值语义：温度 raw 必须是 12 位量级（24.0~26.0℃ → raw 384~416），
# 配置寄存器必须是 probe 写入的 bit0=1。这一条能抓住字节序/换算类错误。
TRAW=$(awk '$1=="00:"{print $2}' /tmp/dregs.txt)
CFG=$(awk '$1=="02:"{print $2}' /tmp/dregs.txt)
info "温度 raw=$TRAW 配置=$CFG"
check_gt "温度 raw 落在模拟区间的量级（$TRAW > 0x0100）" "$((0x$TRAW))" 255
check_true "温度 raw 不超过 16 位（$TRAW <= 0xFFFF）" "1" "$([ "$((0x$TRAW))" -le 65535 ] && echo 1 || echo 0)"
check_eq "配置寄存器为 probe 写入的连续转换使能位" "0001" "$CFG"

info "== 7) debugfs ring：环形缓冲状态与最近样本 =="
cat $DBG/ring > /tmp/dring.txt 2>&1
head -4 /tmp/dring.txt | sed 's/^/    /'
check_contains "ring 含 count="     /tmp/dring.txt "count="
check_contains "ring 含 dropped="   /tmp/dring.txt "dropped="
# 注意：check_contains 的模式是正则，[ 必须转义，否则 busybox grep 报 "bad regex"
check_contains "ring 含样本明细 sample[" /tmp/dring.txt "sample\["
check_contains "样本明细含 seq 与温度" /tmp/dring.txt "temp_milli="

info "== 8) 故障注入：错误处理路径真的被执行 =="
# 阶段 06 起驱动启用了 Runtime PM：无人持有设备时会自动挂起并**停止采样**。
# 本步骤依赖"后台周期性采样在跑"（注入的失败次数要靠周期性 I2C 读来消耗，
# 恢复期也要靠采样推进才能观察到序号增长），所以这里显式打开设备持有一个引用。
# 这不是放宽检查，而是把一个隐含前提写出来：否则本步骤实际测到的是
# "挂起后设备不采样"，与故障注入想验证的错误处理路径毫无关系。
exec 3<>/dev/sensor0
# 顺序很关键：先取基准值，再注入，期间不要读 regs 之类的接口（会消耗注入次数）。
ie_before=$(cat $SYS/i2c_errors 2>/dev/null)
echo 3 > /sys/kernel/debug/virt_i2c/inject_error
sleep 1
ie_after=$(cat $SYS/i2c_errors 2>/dev/null)
delta=$((ie_after - ie_before))
info "注入 3 次：i2c_errors $ie_before → ${ie_after}（增量 ${delta}）"
check_gt "注入 3 次后 i2c_errors 增量 >= 3（实测 ${delta}）" "$delta" 2
# 注入耗尽后驱动必须自己恢复（regmap 报错只在错误处理路径里，不应把设备搞死）
sleep 1
ie_recover_before=$(cat $SYS/i2c_errors 2>/dev/null)
SEQ_R1=$(cat $SYS/seq 2>/dev/null)
sleep 1
SEQ_R2=$(cat $SYS/seq 2>/dev/null)
info "恢复期样本序号：$SEQ_R1 → $SEQ_R2"
check_gt "注入结束后采样仍在推进（序号增长，实测 $SEQ_R1 → ${SEQ_R2}）" "$SEQ_R2" "$SEQ_R1"

info "== 9) 注入后用户态通路仍正常 =="
/bin/sensor_test > /tmp/utest04.txt 2>&1
check_true "sensor_test 退出码为 0" "0" "$?"
check_contains "用户态测试正常结束" /tmp/utest04.txt "测试结束"
# 归还在步骤 8 持有的 runtime PM 引用（之后设备可再次自动挂起）
exec 3<&- 2>/dev/null
exec 3>&- 2>/dev/null

info "== 9b) 卸载/重载的资源清理（覆盖缺口：清理路径此前无回归检查）=="
# 为什么需要：验证者做破坏性负控（删掉 debugfs_remove_recursive）时，内核打印
#   debugfs: Directory 'sensor_char' ... already present!
# 且驱动降级为 debugfs=unavailable，但当时 42 项检查全绿 —— 说明清理路径完全没有回归覆盖。
# 注意：sysfs 属性组挂在类设备上，类销毁会连带清理，所以"漏 sysfs_remove_group"可能不可观测；
# 而 debugfs 目录是独立于设备模型的，漏删一定会在重载时报 "already present"。
KVER=$(uname -r)
rmmod sensor_char 2>/dev/null
sleep 1
if [ -d /sys/kernel/debug/sensor_char ]; then
	fail "卸载后 debugfs 目录已清理" "/sys/kernel/debug/sensor_char 仍存在（debugfs_remove_recursive 缺失？）"
else
	pass "卸载后 debugfs 目录已清理"
fi
if [ -d /sys/class/sensor_char ]; then
	fail "卸载后 sysfs 类目录已清理" "/sys/class/sensor_char 仍存在（类销毁/注销不完整？）"
else
	pass "卸载后 sysfs 类目录已清理"
fi
insmod "/lib/modules/$KVER/sensor_char.ko"
sleep 1
check_exists "重新加载后设备节点恢复" /dev/sensor0
ALREADY=$(dmesg | grep -c "already present")
check_eq "重载时没有 debugfs 重复创建报错（already present 计数）" "0" "$ALREADY"
check_exists "重新加载后 sysfs 属性恢复" $SYS/interval_ms

info "== 10) 内核告警检查（测试体之后重新取样）=="
dmesg > /tmp/dmesg04.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:|BUG:' /tmp/dmesg04.txt)
if [ "$W" != "0" ]; then
    info "告警内容（前 15 行）："
    grep -E 'WARNING:|Call trace:|BUG:' -A6 /tmp/dmesg04.txt | head -15 | sed 's/^/    /'
fi
check_eq "dmesg 无 WARNING/Call trace/BUG" "0" "$W"
