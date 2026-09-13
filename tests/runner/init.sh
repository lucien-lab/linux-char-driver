#!/bin/busybox sh
# ============================================================================
# tests/runner/init.sh - initramfs 的 init 脚本（通用测试入口）
#
# 设计目标：把"每次改动都要人工敲命令验证"变成"一条命令跑完整测试并输出可解析结果"。
#
# 流程：
#   1. 挂载 proc/sys/dev/debugfs/configfs
#   2. 打印 [TEST:START] 标记（供 macOS 侧解析）
#   3. 按 /etc/modules.load 顺序 insmod 内核模块
#   4. 执行 /tests/phase.sh（由 fast-cycle 脚本按 TEST=<阶段> 注入）
#   5. 打印 [TEST:END] pass=<n> fail=<n>，同步磁盘后关机
#
# 结果标记协议（macOS 侧解析用）：
#   [TEST:START] <阶段名>
#   [CHECK:PASS] <检查项>
#   [CHECK:FAIL] <检查项> -- <原因>
#   [TEST:END]   <阶段名> pass=N fail=M
# ============================================================================

mount -t proc     none /proc     2>/dev/null
mount -t sysfs    none /sys      2>/dev/null
mount -t devtmpfs none /dev      2>/dev/null
mkdir -p /sys/kernel/debug /sys/kernel/config
mount -t debugfs  none /sys/kernel/debug  2>/dev/null
mount -t configfs none /sys/kernel/config 2>/dev/null

TEST_NAME=${TEST_NAME:-unknown}
KVER=$(uname -r)

echo ""
echo "============================================================"
echo "[TEST:START] $TEST_NAME"
echo "内核: $KVER / $(uname -m)"
echo "============================================================"

. /tests/lib.sh

# ---------------------------------------------------------------------------
# 加载内核模块
#   /etc/modules.load 每行格式：  <模块文件名> [模块参数...]
#   顺序由构建脚本决定：i2c-stub(虚拟 I2C 总线) -> 传感器驱动
# ---------------------------------------------------------------------------
if [ -f /etc/modules.load ]; then
    while read -r line; do
        [ -n "$line" ] || continue
        case "$line" in \#*) continue ;; esac
        mod=$(echo "$line" | awk '{print $1}')
        args=$(echo "$line" | cut -d' ' -f2-)
        echo "--- insmod $mod $args ---"
        # shellcheck disable=SC2086
        insmod "/lib/modules/$KVER/$mod" $args
        if [ $? -ne 0 ]; then
            fail "insmod $mod" "insmod 返回非 0（模块缺失或参数错误）"
        fi
    done < /etc/modules.load
fi

# ---------------------------------------------------------------------------
# 执行阶段测试脚本
# ---------------------------------------------------------------------------
if [ -f /tests/phase.sh ]; then
    # shellcheck disable=SC1091
    . /tests/phase.sh
else
    fail "phase.sh 存在" "/tests/phase.sh 未注入（构建时未指定 TEST=）"
fi

echo ""
echo "============================================================"
echo "[TEST:END] $TEST_NAME pass=$CHECK_PASS fail=$CHECK_FAIL"
echo "============================================================"

# 保存 dmesg 便于事后分析（busybox dmesg -n 输出全部）
dmesg > /tmp/dmesg.txt 2>/dev/null

sync
poweroff -f
