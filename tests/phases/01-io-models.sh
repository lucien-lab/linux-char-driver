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
# 关键判定（为什么这样判定）：
#   * "非阻塞"必须能观察到 EAGAIN：若驱动把非阻塞当阻塞处理，用户态就无法
#     用 epoll/线程池组织 IO；若驱动错误地重复返回旧样本，则永远看不到 EAGAIN。
#   * "poll 必须电平触发"：消费掉样本后 poll 不能再报可读，否则 epoll_wait
#     会立即返回、用户态忙轮询。用"3 秒内的唤醒次数 ≤ 12"来量化这一语义
#     （500ms 采样周期 → 期望 6~7 次；错误的实现会达到数千次）。
#   * SIGIO 链路任一环断掉都收不到信号，所以用"收到过"作为判定。
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
        info "该内核无法对 kill_fasync 做函数过滤，跳过该项直接证据"
    fi
else
    info "$TR 不存在，跳过 ftrace 证据"
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

# ---------------------------------------------------------------------------
# 1) 阻塞 read
# ---------------------------------------------------------------------------
check_contains "阻塞 read 成功返回样本" /tmp/io.txt "\[IO\] blocking_read=ok"

# ---------------------------------------------------------------------------
# 2) 非阻塞 read：必须观察到 EAGAIN
# ---------------------------------------------------------------------------
EAGAIN_N=$(sed -n 's/^\[IO\] nonblock_eagain=//p' /tmp/io.txt | tr -d '\r' | head -1)
[ -n "$EAGAIN_N" ] || EAGAIN_N=0
check_gt "非阻塞 read 返回 EAGAIN（实测 $EAGAIN_N 次）" "$EAGAIN_N" 0

# ---------------------------------------------------------------------------
# 3) epoll：超时时间内报告可读 + 程序整体退出码
# ---------------------------------------------------------------------------
check_contains "epoll 在 3 秒窗口内报告可读" /tmp/io.txt "\[IO\] epoll_wait_ok=1"
check_true "io_models_test 退出码为 0" "0" "$RC"

# ---------------------------------------------------------------------------
# 4) poll 电平触发语义（消费后不再上报 → 不会忙轮询）
# ---------------------------------------------------------------------------
WAKES=$(sed -n 's/^\[IO\] epoll_wakeups=//p' /tmp/io.txt | tr -d '\r' | head -1)
[ -n "$WAKES" ] || WAKES=9999
if [ "$WAKES" -le 12 ]; then
    pass "poll 为电平触发、无忙轮询（3 秒内仅唤醒 $WAKES 次）"
else
    fail "poll 为电平触发、无忙轮询" "3 秒内唤醒 $WAKES 次（>12），疑似样本被消费后仍上报 EPOLLIN"
fi

# ---------------------------------------------------------------------------
# 5) SIGIO 异步通知
# ---------------------------------------------------------------------------
SIGIO_N=$(sed -n 's/^\[IO\] sigio_count=//p' /tmp/io.txt | tr -d '\r' | head -1)
[ -n "$SIGIO_N" ] || SIGIO_N=0
check_gt "SIGIO 异步通知到达（实测 $SIGIO_N 次）" "$SIGIO_N" 0

# ---------------------------------------------------------------------------
# 6) 程序整体结论
# ---------------------------------------------------------------------------
check_contains "程序整体结论为 PASS" /tmp/io.txt "\[IO\] OVERALL=PASS"

# ---------------------------------------------------------------------------
# 7) kill_fasync 的直接证据（ftrace 函数过滤）
# ---------------------------------------------------------------------------
if [ "$FT_OK" = "1" ]; then
    check_gt "ftrace 捕获到 kill_fasync 调用（实测 $KF 次）" "$KF" 0
fi

# ---------------------------------------------------------------------------
# 8) 内核告警
# ---------------------------------------------------------------------------
dmesg > /tmp/dmesg-io.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:' /tmp/dmesg-io.txt)
check_eq "dmesg 无 WARNING/Call trace" "0" "$W"
