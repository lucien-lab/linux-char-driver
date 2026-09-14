#!/bin/busybox sh
# ============================================================================
# tests/phases/06-runtime-pm-kunit.sh - 阶段 06：Runtime PM + KUnit 单元测试
#
# 【关键概念】runtime PM 的状态属性挂在**硬件设备**上，不是挂在字符设备接口上：
#   /sys/bus/i2c/devices/<bus>-0048/power/{control,runtime_status,runtime_suspended_time}
#   原因：电源管理针对的是"那颗传感器"，而不是"给用户态看的 /dev 节点"。
#   字符设备（/sys/class/sensor_char/sensor0）是一个纯粹的软件接口，
#   它自己的 power/ 目录永远是 unsupported（disable_depth=1）。
#   —— 这正是本阶段第一次跑测试时踩到的坑：读错路径会得到 "unsupported"，
#      看起来像"PM 没启用"，其实是找错了设备。
#
# 验证什么：
#   1. runtime PM 已启用（该设备的 runtime_status 不是 unsupported）
#   2. probe 交还引用后无人使用时自动挂起（停采样省电）
#   3. 打开设备 -> active（resume 会重新启动采样）
#   4. 关闭设备 -> autosuspend 延迟后 suspended
#   5. **挂起期间真的停止采样**（seq/irq 计数冻结）——专抓"假省电"
#   6. 挂起期间改周期只"记账"（deferred），不会偷偷把定时器启动起来
#   7. 重新打开后采样恢复，且挂起期间设置的周期生效
#   8. get/put 引用计数平衡（反复开关后仍能自动挂起）
#   9. 核心记录了挂起时间（runtime_suspended_time > 0）
#  10. KUnit 用例全部通过（TAP: ok N <用例名>，且无 not ok）
#  11. 内核无 WARNING / Call trace（测试体执行之后重新取样）
#
# 设计要点（来自前面阶段的教训）：
#   * 每次判定都用**当场重新取样**的文件/变量，不复用较早的快照
#     （阶段 06 第一版把 resume 日志与旧快照比，得到假失败）。
#   * 告警检查必须在测试体之后重新取样（阶段 02 的假绿教训）。
#   * KUnit 判定既看用例名（确认真跑了），也看 fail 计数（不能只看有 ok）。
# ============================================================================

KVER=$(uname -r)

# ---------------------------------------------------------------------------
# 定位挂载 runtime PM 的设备：i2c 从设备（由设备树 reg = <0x48> 决定）
# ---------------------------------------------------------------------------
I2CDEV=$(ls -d /sys/bus/i2c/devices/*-0048 2>/dev/null | head -1)
PWR=$I2CDEV/power
SYS=/sys/class/sensor_char/sensor0
DBG=/sys/kernel/debug/sensor_char

info "runtime PM 属性路径：$PWR"
check_nonempty "找到 i2c 从设备节点" "$I2CDEV"
check_exists "power/runtime_status 存在" $PWR/runtime_status
check_exists "power/runtime_suspended_time 存在" $PWR/runtime_suspended_time

info "== 1) runtime PM 已启用 =="
ST0=$(cat $PWR/runtime_status 2>/dev/null)
info "runtime_status = $ST0（unsupported 表示驱动没调 pm_runtime_enable）"
case "$ST0" in
	active|suspended) pass "runtime PM 已启用（runtime_status=$ST0，不是 unsupported）" ;;
	*) fail "runtime PM 已启用（runtime_status 不应是 unsupported）" "实际=$ST0" ;;
esac

info "== 2) 无人使用 -> 自动挂起（probe 交还引用，且真的停掉采样）=="
sleep 3	# autosuspend 延迟 1s，留足余量
ST_IDLE=$(cat $PWR/runtime_status 2>/dev/null)
info "空闲 3 秒后 runtime_status = $ST_IDLE"
check_eq "无使用者时自动挂起" "suspended" "$ST_IDLE"

info "== 3) 打开设备 -> active（resume 重新启动采样）=="
dmesg -c > /dev/null 2>/dev/null
if exec 3<>/dev/sensor0; then
	pass "打开设备成功（fd 3 持有引用）"
else
	fail "打开设备成功（fd 3 持有引用）" "无法打开 /dev/sensor0"
fi
sleep 1
ST_OPEN=$(cat $PWR/runtime_status 2>/dev/null)
info "持有 fd 时 runtime_status = $ST_OPEN"
check_eq "打开期间保持 active" "active" "$ST_OPEN"
dmesg > /tmp/dmesg_resume.txt 2>/dev/null
check_contains "出现 runtime resume 日志" /tmp/dmesg_resume.txt "runtime resume: sampling restarted"

info "== 4) 关闭设备 -> 自动挂起 =="
exec 3<&- 2>/dev/null
exec 3>&- 2>/dev/null
sleep 3	# autosuspend 1s + 余量
ST_CLOSED=$(cat $PWR/runtime_status 2>/dev/null)
info "关闭并等待 3 秒后 runtime_status = $ST_CLOSED"
check_eq "关闭后自动挂起" "suspended" "$ST_CLOSED"
dmesg > /tmp/dmesg_suspend.txt 2>/dev/null
check_contains "出现 runtime suspend 日志" /tmp/dmesg_suspend.txt "runtime suspend: sampling stopped"

info "== 5) 挂起期间真的停止采样（假省电检测）=="
SEQ_A=$(cat $SYS/seq 2>/dev/null)
IRQ_A=$(sed -n 's/.* irq=\([0-9]*\) .*/\1/p' $DBG/stats 2>/dev/null | head -1)
check_gt "挂起前确实产生过样本（seq=$SEQ_A > 0）" "$SEQ_A" 0
sleep 2
SEQ_B=$(cat $SYS/seq 2>/dev/null)
IRQ_B=$(sed -n 's/.* irq=\([0-9]*\) .*/\1/p' $DBG/stats 2>/dev/null | head -1)
info "挂起 2 秒：seq $SEQ_A -> $SEQ_B，irq $IRQ_A -> $IRQ_B"
check_eq "挂起期间样本序号冻结" "$SEQ_A" "$SEQ_B"
check_eq "挂起期间中断计数冻结" "$IRQ_A" "$IRQ_B"

info "== 6) 挂起期间改周期只记账，不弄醒设备 =="
echo 200 > $SYS/interval_ms
sleep 1
ST_AFTER_WRITE=$(cat $PWR/runtime_status 2>/dev/null)
check_eq "挂起期间写 interval_ms 不会把设备唤醒" "suspended" "$ST_AFTER_WRITE"
# 必须当场重新取样：用步骤 4 的旧快照会得到假失败（那条 deferred 日志此时才产生）
dmesg > /tmp/dmesg_defer.txt 2>/dev/null
check_contains "驱动记录了被延迟的周期设置" /tmp/dmesg_defer.txt "deferred: device runtime-suspended"

info "== 7) 重新打开 -> 恢复采样，且挂起期间的设置生效 =="
if exec 3<>/dev/sensor0; then
	pass "重新打开设备成功"
else
	fail "重新打开设备成功" "无法打开 /dev/sensor0"
fi
sleep 1
ST_REOPEN=$(cat $PWR/runtime_status 2>/dev/null)
check_eq "重新打开后回到 active" "active" "$ST_REOPEN"
IV_NOW=$(cat $SYS/interval_ms 2>/dev/null)
check_eq "挂起期间设置的周期已生效（200ms）" "200" "$IV_NOW"
SEQ_C=$(cat $SYS/seq 2>/dev/null)
sleep 1
SEQ_D=$(cat $SYS/seq 2>/dev/null)
info "恢复采样后：seq $SEQ_C -> $SEQ_D"
check_gt "恢复后采样继续推进" "$SEQ_D" "$SEQ_C"
exec 3<&- 2>/dev/null
exec 3>&- 2>/dev/null

info "== 8) get/put 引用计数平衡（反复开关后仍能挂起）=="
i=0
while [ $i -lt 3 ]; do
	exec 3<>/dev/sensor0
	exec 3<&- 2>/dev/null
	exec 3>&- 2>/dev/null
	i=$((i + 1))
done
sleep 3
ST_AFTER_3=$(cat $PWR/runtime_status 2>/dev/null)
info "open/close 3 轮后 runtime_status = $ST_AFTER_3"
check_eq "反复开关后仍能自动挂起（引用计数已配平）" "suspended" "$ST_AFTER_3"

info "== 9) 核心记录了挂起时间（证明真的进入过低功耗）=="
SUSP_TIME=$(cat $PWR/runtime_suspended_time 2>/dev/null)
info "runtime_suspended_time = ${SUSP_TIME}ms"
check_gt "累计挂起时间 > 0" "${SUSP_TIME:-0}" 0

info "== 10) KUnit 单元测试 =="
dmesg -c > /dev/null 2>/dev/null	# 清空缓冲区，只留本模块输出，避免误判
insmod "/lib/modules/$KVER/sensor_kunit.ko"
sleep 1
dmesg > /tmp/kunit.txt 2>/dev/null
check_contains "KUnit 套件已执行（Subtest: sensor_calc）" /tmp/kunit.txt "Subtest: sensor_calc"
# 本内核的 KUnit 输出是 KTAP v1 格式： "ok 1 <用例名>"（用例名与序号之间是空格，不是 "-"）
check_contains "零点用例通过" /tmp/kunit.txt "ok .* sensor_raw_to_milli_boundaries"
check_contains "芯片工作区间用例通过" /tmp/kunit.txt "ok .* sensor_raw_to_milli_chip_range"
check_contains "高 4 位状态位用例通过" /tmp/kunit.txt "ok .* sensor_raw_to_milli_ignores_high_bits"
check_contains "周期边界用例通过" /tmp/kunit.txt "ok .* sensor_interval_valid_bounds"
check_contains "环缓冲回绕用例通过" /tmp/kunit.txt "ok .* sensor_fifo_next_wraps"
check_contains "套件汇总为 5 通过 0 失败" /tmp/kunit.txt "sensor_calc: pass:5 fail:0"
NOTOK=$(grep -c "not ok" /tmp/kunit.txt)
check_eq "KUnit 没有失败用例（not ok 计数）" "0" "$NOTOK"
KUNIT_MOD=$(lsmod 2>/dev/null | grep -c sensor_kunit)
check_gt "sensor_kunit 模块已加载（lsmod 可见）" "$KUNIT_MOD" 0

info "== 11) 内核告警检查（测试体之后重新取样）=="
dmesg > /tmp/dmesg_final.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:' /tmp/dmesg_final.txt)
check_eq "dmesg 无 WARNING/Call trace" "0" "$W"
