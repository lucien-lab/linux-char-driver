#!/usr/bin/env bash
# ============================================================================
# 22-macos-run-test.sh - 【在 macOS 上执行】跑一次 QEMU 阶段测试并给出结论
#
# 用法：
#   bash scripts/22-macos-run-test.sh <阶段名> [超时秒数]
#   例：bash scripts/22-macos-run-test.sh 01-io-models 120
#
# 做的事：
#   1. 用 TCG + cortex-a72 启动 QEMU（与项目原文档一致；HVF + -cpu host 下 ftrace 会挂死）
#   2. 串口输出保存到 logs/<时间>-<阶段>.log
#   3. 检测 [TEST:END] 标记或超时后杀进程
#   4. 解析 [CHECK:PASS] / [CHECK:FAIL] 统计，打印结论，失败则以非 0 退出
#
# 退出码：0=全部通过；1=有 FAIL 或未跑到结束；2=缺少产物
# ============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."
PROJ=$PWD
TEST=${1:-smoke}
TIMEOUT=${2:-180}

IMAGE="$PROJ/artifacts/Image"
DTB="$PROJ/artifacts/virt-sensor.dtb"
INITRD="$PROJ/artifacts/initramfs.cpio.gz"
for f in "$IMAGE" "$DTB" "$INITRD"; do
    [ -f "$f" ] || { echo "缺少产物: $f（先跑 13-vm-fast-cycle.sh + 21-macos-sync-artifacts.sh）"; exit 2; }
done

mkdir -p "$PROJ/logs"
LOG="$PROJ/logs/$(date '+%Y%m%d-%H%M%S')-${LANE:-main}-$TEST.log"

echo "==> 启动 QEMU（TCG / cortex-a72 / ${TIMEOUT}s 超时）"
echo "    串口日志: $LOG"

# QEMU 参数说明：
#   -accel tcg,thread=multi 强制 MTTCG：多个 vCPU 真正并行执行。
#     为什么要显式指定：默认 TCG 可能是"轮转切换"模型（同一时刻只有一个 vCPU 在跑）。
#     实测证据：quick 模式 shm_seq_changes=300（读到 300 次序号变化）但 shm_seq_odd_seen=0
#     （从未撞上 1ms 写入窗口）—— 只有串行执行才会出现这种组合。
#     本项目的"并发无竞态"结论依赖真实并行，所以必须开 MTTCG。
#   -smp 2：单核下所有 SMP 竞态不可触发（阶段 03 验证报告 S2）。
qemu-system-aarch64 \
    -M virt -cpu cortex-a72 -accel tcg,thread=multi -smp 2 -m 1G -display none -serial stdio \
    -dtb "$DTB" -kernel "$IMAGE" -initrd "$INITRD" \
    -append "console=ttyAMA0 test=$TEST" > "$LOG" 2>&1 &
QPID=$!

elapsed=0
while [ $elapsed -lt "$TIMEOUT" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    if grep -q '\[TEST:END\]' "$LOG" 2>/dev/null; then
        sleep 1
        break
    fi
    kill -0 $QPID 2>/dev/null || break
done
kill -9 $QPID 2>/dev/null
wait $QPID 2>/dev/null

# ---------------------------------------------------------------------------
# 解析结果
# ---------------------------------------------------------------------------
PASS_N=$(grep -c '\[CHECK:PASS\]' "$LOG" 2>/dev/null || true)
FAIL_N=$(grep -c '\[CHECK:FAIL\]' "$LOG" 2>/dev/null || true)
ENDED=$(grep -c '\[TEST:END\]' "$LOG" 2>/dev/null || true)
PASS_N=${PASS_N:-0}; FAIL_N=${FAIL_N:-0}; ENDED=${ENDED:-0}

echo
echo "================= 检查项明细 ================="
grep -E '\[CHECK:(PASS|FAIL)\]' "$LOG" | sed 's/^/  /' || true
echo

# 内核层面的异常信号（不一定算 FAIL，但必须人工关注）
PANIC=$(grep -cE 'Kernel panic|Unable to handle kernel' "$LOG" 2>/dev/null || true)
WARN=$(grep -cE 'WARNING:|BUG:|Call trace:' "$LOG" 2>/dev/null || true)
PANIC=${PANIC:-0}; WARN=${WARN:-0}

echo "================= 结论 ================="
echo "  阶段      : ${TEST}"
echo "  检查通过  : ${PASS_N}"
echo "  检查失败  : ${FAIL_N}"
echo "  跑到结束  : $([ "${ENDED}" -gt 0 ] && echo 是 || echo '否（超时或中途卡死）')"
echo "  内核 panic: ${PANIC}"
echo "  内核告警  : ${WARN}"
echo "  日志      : $LOG"

if [ "${FAIL_N}" -gt 0 ] || [ "${ENDED}" -eq 0 ]; then
    echo
    echo "  ❌ 未通过"
    echo "  --- 失败项 ---"
    grep '\[CHECK:FAIL\]' "$LOG" | sed 's/^/    /'
    echo "  --- 日志末尾 30 行 ---"
    tail -30 "$LOG" | sed 's/^/    /'
    exit 1
fi

if [ "${PANIC}" -gt 0 ]; then
    echo
    echo "  ❌ 出现内核 panic"
    exit 1
fi

echo
echo "  ✅ 全部检查项通过（内核告警数: ${WARN}）"
exit 0
