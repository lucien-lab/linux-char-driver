#!/bin/busybox sh
# ============================================================================
# tests/runner/lib.sh - 测试断言库（被 init.sh 与各阶段 phase.sh source）
#
# 用法（在各阶段脚本里）：
#   check_contains "probe 日志包含驱动名" /tmp/probe.txt "sensor_char"
#   check_eq       "模块版本号" "6.6.156" "$(uname -r)"
#   check_cmd      "epoll 测试程序退出码为 0" /bin/io_models_test
#   check_true     "自行判断的条件" "$result" "1"
#   pass "自定义检查项"
#   fail "自定义检查项" "原因"
#
# 计数变量 CHECK_PASS / CHECK_FAIL 由本文件初始化，init.sh 在最后汇总打印。
# 注意：busybox sh 没有 [[ ]]，统一用 [ ]；字符串比较用 = 。
# ============================================================================

CHECK_PASS=0
CHECK_FAIL=0

# 成功 / 失败：统一输出可解析标记
pass() {
    CHECK_PASS=$((CHECK_PASS + 1))
    echo "[CHECK:PASS] $1"
}

fail() {
    CHECK_FAIL=$((CHECK_FAIL + 1))
    echo "[CHECK:FAIL] $1 -- $2"
}

# 条件判断：check_true <名称> <期望> <实际>
check_true() {
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1" "期望='$2' 实际='$3'"
    fi
}

# 文件内容包含：check_contains <名称> <文件> <模式>
#   注意：<模式> 走 grep，是**正则**。想匹配字面量里的 [ ] ( ) . * 等必须转义
#   （busybox grep 对未闭合的 [ 会直接报 "bad regex" 并判定失败）。
check_contains() {
    if [ ! -f "$2" ]; then
        fail "$1" "文件不存在: $2"
        return
    fi
    if grep -q -- "$3" "$2"; then
        pass "$1"
    else
        fail "$1" "在 $2 中未找到 '$3'"
    fi
}

# 文件内容不包含：check_not_contains <名称> <文件> <子串>
check_not_contains() {
    if [ ! -f "$2" ]; then
        fail "$1" "文件不存在: $2"
        return
    fi
    if grep -q -- "$3" "$2"; then
        fail "$1" "在 $2 中不应出现 '$3'"
    else
        pass "$1"
    fi
}

# 字符串相等：check_eq <名称> <期望> <实际>
check_eq() {
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1" "期望='$2' 实际='$3'"
    fi
}

# 字符串非空：check_nonempty <名称> <值>
check_nonempty() {
    if [ -n "$2" ]; then
        pass "$1"
    else
        fail "$1" "值为空"
    fi
}

# 数值大于：check_gt <名称> <实际> <阈值>  （整数）
check_gt() {
    if [ "$2" -gt "$3" ] 2>/dev/null; then
        pass "$1"
    else
        fail "$1" "期望 > $3，实际=$2"
    fi
}

# 文件/设备存在：check_exists <名称> <路径>
check_exists() {
    if [ -e "$2" ]; then
        pass "$1"
    else
        fail "$1" "路径不存在: $2"
    fi
}

# 运行命令并检查退出码：check_cmd <名称> <命令...>
check_cmd() {
    name=$1
    shift
    "$@" > /tmp/cmd-out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        pass "$name"
    else
        fail "$name" "退出码=${rc}，输出: $(tail -3 /tmp/cmd-out.txt | tr '\n' ' ')"
    fi
    return $rc
}

# 输出信息（不计入检查项）
info() {
    echo "      · $1"
}

# 从 "<键>=<值>" 形式的输出里取某个键的值：tag_val <文件> <键> <默认值>
# 先按空格拆成 token，因此同一行里的多个键都能取到
# （例如 [STAT] open=1 interval=20 或 [RING] mmap_reads=100 mmap_retries=3）。
tag_val() {
	v=$(tr ' ' '\n' < "$1" 2>/dev/null | sed -n "s/^$2=//p" | tr -d '\r' | head -1)
	[ -n "$v" ] && echo "$v" || echo "$3"
}
