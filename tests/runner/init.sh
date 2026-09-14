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

# ---------------------------------------------------------------------------
# 选择本次要执行的阶段脚本（优先用内核命令行，见下）
#
# 为什么需要这一步：initramfs 是构建时打包的，如果只把“默认阶段脚本”烤进去，
# 那么不重新构建就切换测试（例如跑完 01-io-models 再跑 smoke 回归）就会
# 重复执行上一次的脚本 —— “回归通过”会变成假结论。
# 因此：
#   1) 内核命令行 test=<名字>（由 scripts/22-macos-run-test.sh 传入）优先；
#   2) 没传时用构建时注入的 /tests/phase.sh（还是可以用 TEST= 固定一个默认值）。
# 这样一次构建就能跑任意阶段测试，且不会把测试跑错。
# ---------------------------------------------------------------------------
PHASE=/tests/phase.sh
CMDLINE_TEST=$(tr ' ' '\n' < /proc/cmdline 2>/dev/null | sed -n 's/^test=//p' | head -1)
if [ -n "$CMDLINE_TEST" ]; then
    # 命令行明确指定了就一律用它：如果脚本不存在，后面的检查会直接报失败，
    # 而不是静默回退到默认脚本（否则会“跑错测试却看起来跑了测试”）。
    PHASE="/tests/phases/$CMDLINE_TEST.sh"
    TEST_NAME="$CMDLINE_TEST"
fi

echo ""
echo "============================================================"
echo "[TEST:START] $TEST_NAME"
echo "内核: $KVER / $(uname -m)   阶段脚本: $PHASE"
echo "============================================================"

. /tests/lib.sh

# ---------------------------------------------------------------------------
# 加载内核模块
#   /etc/modules.load 每行格式：  <模块文件名> [模块参数...]
#   顺序由构建脚本决定：i2c-stub(虚拟 I2C 总线) -> 传感器驱动
# ---------------------------------------------------------------------------
# 模块清单选择：/etc/modules/<阶段名>.load 优先（构建脚本按阶段打包），
# 没有对应清单时回退到 /etc/modules.load（默认清单）。
MODLIST=/etc/modules.load
if [ -f "/etc/modules/$TEST_NAME.load" ]; then
    MODLIST="/etc/modules/$TEST_NAME.load"
fi
echo "--- 模块清单: $MODLIST ---"
if [ -f "$MODLIST" ]; then
    while read -r line; do
        [ -n "$line" ] || continue
        case "$line" in \#*) continue ;; esac
        mod=$(echo "$line" | awk '{print $1}')
        # 注意：不要用 `cut -d' ' -f2-`。行内没有空格时 cut 会把整行原样返回，
        # 于是模块文件名被当成模块参数传给 insmod，内核每次都打印
        # "unknown parameter 'sensor_char.ko' ignored"（既污染日志，又会掩盖真正写错的参数）。
        args=$(echo "$line" | awk '{$1=""; sub(/^[ \t]+/, ""); print}')
        if [ -n "$args" ]; then
            echo "--- insmod $mod $args ---"
        else
            echo "--- insmod $mod ---"
        fi
        # shellcheck disable=SC2086
        insmod "/lib/modules/$KVER/$mod" $args
        if [ $? -ne 0 ]; then
            fail "insmod $mod" "insmod 返回非 0（模块缺失或参数错误）"
        fi
    done < "$MODLIST"
fi

# ---------------------------------------------------------------------------
# 执行阶段测试脚本
# ---------------------------------------------------------------------------
if [ -f "$PHASE" ]; then
    # shellcheck disable=SC1091
    . "$PHASE"
else
    fail "阶段脚本存在" "$PHASE 不存在（tests/phases/ 下没有这个阶段脚本）"
fi

echo ""
echo "============================================================"
echo "[TEST:END] $TEST_NAME pass=$CHECK_PASS fail=$CHECK_FAIL"
echo "============================================================"

# 保存 dmesg 便于事后分析（busybox dmesg -n 输出全部）
dmesg > /tmp/dmesg.txt 2>/dev/null

sync
poweroff -f
