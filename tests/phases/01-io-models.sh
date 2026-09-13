#!/bin/busybox sh
# ============================================================================
# tests/phases/01-io-models.sh - 阶段 01：四种 IO 模型验证
#
# 验证目标（对应 driver/sensor_char.c 的 fops 补全）：
#   1. 阻塞 read        —— 没有新样本时睡在 waitqueue 上，有新样本被唤醒
#   2. 非阻塞 read      —— O_NONBLOCK 下无新样本立即返回 -EAGAIN
#   3. poll/select/epoll—— poll_wait 注册等待队列 + EPOLLIN 上报
#   4. fasync/SIGIO     —— 中断下半部 kill_fasync 异步通知用户态
#
# ============================ 判定为什么要这么设计 ============================
#
# 本套件经过一轮"破坏性变体验证"：验证者把驱动里的 poll_wait() 删掉后，
# **上一版套件仍然 9 项全 PASS（假通过）**。原因是上一版只用
# "epoll_wait 至少返回过一次可读"作为判定，而 epoll_ctl(ADD) 时的首轮 ->poll
# 就足以满足它 —— 即使驱动此后再也不会唤醒用户态。
#
# 因此本版增加了三组"只有正确实现才可能通过"的检查（缺一项都可能重新变成假通过）：
#
#   [首次唤醒延迟 ≤ 1000ms]  这是发现漏写 poll_wait 的唯一可靠观测：
#       正确实现注册了等待队列 → 新样本到达即唤醒 → 延迟 ≈ 一个采样周期(500ms)；
#       漏写 poll_wait 时 epoll_wait 会睡满超时，退出前最后一次 vfs_poll 才看到
#       新样本而"假装可读" → 延迟 ≈ 超时值(实测 1503/2003ms)。
#       注意：只看返回值(>0)完全分辨不出来，必须看时间。
#
#   [唤醒次数下界 ≥ 3]  同一次运行里，下界掐死"只唤醒一次"的假通过，
#       上界（≤12）保留原用途：抓"样本被消费后仍上报 EPOLLIN"的忙轮询错误
#       （实测坏实现可达 121642 次）。
#
#   [电平语义三段 + per-open 独立性]  未消费时连续 5 次 poll 都应报可读
#       （one-shot 实现只能通过 1 次）；消费后必须立刻变 0；新开的 fd 必须
#       立刻可读（证明 last_seq 是按 fd 记的，不是全局标志）。
#
#   [blocking_wait_ms ≥ 500]  证明阻塞 read 真的睡在 waitqueue 上，
#       而不是因为缓存里恰有样本所以立刻返回（消除原版的时序巧合）。
#
# 注意：grep 用的是基本正则，`[IO]` 必须写成 `\[IO\]`，否则会被当成字符集。
# ============================================================================

info "== 前置：开启 ftrace 过滤 kill_fasync（为 SIGIO 提供直接证据）=="
TR=/sys/kernel/debug/tracing
FT_OK=0
if [ -d "$TR" ]; then
    echo 0 > $TR/tracing_on
    echo function > $TR/current_tracer
    echo kill_fasync > $TR/set_ftrace_filter 2>/dev/null
    if grep -q kill_fasync $TR/set_ftrace_filter 2>/dev/null; then
        FT_OK=1
        echo 1 > $TR/tracing_on
        info "已设置 set_ftrace_filter=kill_fasync"
    else
        info "该内核无法对 kill_fasync 做函数过滤"
    fi
else
    info "$TR 不存在"
fi

info "== 运行 /bin/io_models_test（四种 IO 模型）=="
/bin/io_models_test > /tmp/io.txt 2>&1
RC=$?

KF=0
if [ "$FT_OK" = "1" ]; then
    echo 0 > $TR/tracing_on
    # 注意：把 current_tracer 切回 nop 会清空 trace 缓冲区，
    # 所以必须在切回之前把计数取出来（否则永远读到 0）。
    KF=$(grep -c kill_fasync $TR/trace 2>/dev/null)
    [ -n "$KF" ] || KF=0
    echo nop > $TR/current_tracer
fi

info "--- 程序原始输出 ---"
sed 's/^/    /' /tmp/io.txt

# 取值辅助：取不到时用"必然导致失败"的默认值，避免"缺输出=放行"
val_or() {	# val_or <键> <默认值>
	v=$(sed -n "s/^\[IO\] $1=//p" /tmp/io.txt | tr -d '\r' | head -1)
	[ -n "$v" ] && echo "$v" || echo "$2"
}

# ---------------------------------------------------------------------------
# 1) 阻塞 read：读得到 + 真的等过（消除时序巧合）
# ---------------------------------------------------------------------------
check_contains "阻塞 read 成功返回样本" /tmp/io.txt "\[IO\] blocking_read=ok"
BW=$(val_or blocking_wait_ms 0)
check_gt "阻塞 read 真的等了约一个采样周期（实测 ${BW}ms ≥ 500ms）" "$BW" 499

# ---------------------------------------------------------------------------
# 2) 非阻塞 read：必须观察到 EAGAIN
# ---------------------------------------------------------------------------
EAGAIN_N=$(val_or nonblock_eagain 0)
check_gt "非阻塞 read 返回 EAGAIN（实测 $EAGAIN_N 次）" "$EAGAIN_N" 0

# ---------------------------------------------------------------------------
# 3) epoll：窗口内可读 + 首次唤醒延迟（证明 poll_wait 真的登记了等待队列）
# ---------------------------------------------------------------------------
check_contains "epoll 在 3 秒窗口内报告可读" /tmp/io.txt "\[IO\] epoll_wait_ok=1"
check_true "io_models_test 退出码为 0" "0" "$RC"

FIRST_MS=$(val_or epoll_first_wakeup_ms 99999)
if [ "$FIRST_MS" -le 1000 ]; then
    pass "首次唤醒延迟 ${FIRST_MS}ms ≤ 1000ms（poll_wait 已登记等待队列）"
else
    fail "首次唤醒延迟 ≤ 1000ms" "实测 ${FIRST_MS}ms：疑似 poll() 漏写 poll_wait()——epoll_wait 睡满超时后才靠最后一次 vfs_poll 假装可读"
fi

# ---------------------------------------------------------------------------
# 4) 唤醒次数：下界（掐死"只唤醒一次"的假通过）+ 上界（掐死忙轮询）
# ---------------------------------------------------------------------------
WAKES=$(val_or epoll_wakeups 9999)
check_gt "poll 唤醒次数下界（实测 3 秒内 $WAKES 次 ≥ 3）" "$WAKES" 2
if [ "$WAKES" -le 12 ]; then
    pass "poll 为电平触发、无忙轮询（3 秒内仅唤醒 $WAKES 次）"
else
    fail "poll 为电平触发、无忙轮询" "3 秒内唤醒 $WAKES 次（>12），疑似样本被消费后仍上报 EPOLLIN"
fi

# ---------------------------------------------------------------------------
# 5) 电平触发语义三段 + per-open 独立性
# ---------------------------------------------------------------------------
AFTER=$(val_or poll_after_consume 1)
check_eq "消费后 poll 立刻不再报可读（避免忙轮询）" "0" "$AFTER"

HITS=$(val_or poll_unread_hits 0)
check_eq "未消费时连续 5 次 poll 都报可读（电平语义持续存在）" "5" "$HITS"

NEWFD=$(val_or new_fd_immediately_readable 0)
check_eq "新开的 fd 立刻可读（last_seq 按 fd 独立记录）" "1" "$NEWFD"

# ---------------------------------------------------------------------------
# 6) SIGIO 异步通知
# ---------------------------------------------------------------------------
SIGIO_N=$(val_or sigio_count 0)
check_gt "SIGIO 异步通知到达（实测 $SIGIO_N 次）" "$SIGIO_N" 0

# ---------------------------------------------------------------------------
# 7) 程序整体结论
# ---------------------------------------------------------------------------
check_contains "程序整体结论为 PASS" /tmp/io.txt "\[IO\] OVERALL=PASS"

# ---------------------------------------------------------------------------
# 8) kill_fasync 的直接证据（ftrace 函数过滤）
#    前一版在 ftrace 不可用时静默跳过该项（检查项从 9 降到 8 仍显示全绿），
#    这里改为显式检查：能力缺失要体现在结论里，而不是消失。
# ---------------------------------------------------------------------------
check_eq "该内核支持对 kill_fasync 做 ftrace 函数过滤" "1" "$FT_OK"
check_gt "ftrace 捕获到 kill_fasync 调用（实测 $KF 次）" "$KF" 0

# ---------------------------------------------------------------------------
# 9) 内核告警
# ---------------------------------------------------------------------------
dmesg > /tmp/dmesg-io.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:' /tmp/dmesg-io.txt)
check_eq "dmesg 无 WARNING/Call trace" "0" "$W"
