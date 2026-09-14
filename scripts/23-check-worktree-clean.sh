#!/usr/bin/env bash
# ============================================================================
# 23-check-worktree-clean.sh - 【合并门禁】交付树污染扫描
#
# 为什么需要这个脚本（两次真实事故）：
#   1) 阶段 05：实现者在交付工作树里做完负控实验（把 trigger_handler 的温/湿度通道互换），
#      忘记还原就"汇报全绿"。该文件是**未跟踪文件**，git diff/git status 看不到内容差异，
#      所以靠 git 状态无法发现；独立验证者重建后得到 19 PASS / 3 FAIL 才暴露。
#   2) 阶段 06：验证者在交付工作树里做负控（去掉上界校验、ring_capacity 返回固定值），
#      工具返回错误但其实已写入，残留了两处 /* NC-... */ 改动。
#
#   共同点：交付树（会被合并/被引用为最终成果的树）与实验树必须是两棵。
#   本脚本是合并前的最后一道门禁：扫描交付树里是否残留实验代码，并检查工作区是否干净。
#
# 用法：
#   bash scripts/23-check-worktree-clean.sh [项目目录]      # 默认当前项目的上级
#   退出码 0 = 干净；1 = 发现残留（会打印命中位置）
# ============================================================================
set -uo pipefail

PROJ=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
cd "$PROJ" || exit 1

FAIL=0

echo "==> 扫描目录：$PROJ"
echo

# ---------------------------------------------------------------------------
# 1) 实验代码残留：只在**代码文件**里找（测试脚本里的说明性注释是合法的，
#    因为它们会引用负控编号来解释"这条检查为什么这么设计"）
# ---------------------------------------------------------------------------
echo "---- [1/3] 代码文件里的实验残留标记 ----"
CODE_FILES=$(ls driver/*.c driver/*.h user/*.c 2>/dev/null)
HITS=$(grep -nE "NC-|NEGCTRL|TODO-EXPERIMENT|BREAK-HERE|force-fail" $CODE_FILES 2>/dev/null)
if [ -n "$HITS" ]; then
    echo "$HITS" | sed 's/^/  ✗ /'
    FAIL=1
else
    echo "  ✓ 未发现 NC-/NEGCTRL/EXPERIMENT 标记"
fi
echo

# ---------------------------------------------------------------------------
# 2) 交付文件里的"被禁用代码"迹象：注释掉的 return/赋值常见于负控残留
# ---------------------------------------------------------------------------
echo "---- [2/3] 可疑的注释掉的关键语句 ----"
SUS=$(grep -nE "/\*\s*(NC|NEG|DEBUG)[^)]*\*/|//\s*(NC|NEG)[:-]" $CODE_FILES 2>/dev/null)
if [ -n "$SUS" ]; then
    echo "$SUS" | sed 's/^/  ✗ /'
    FAIL=1
else
    echo "  ✓ 未发现注释掉的关键语句"
fi
echo

# ---------------------------------------------------------------------------
# 3) git 工作区是否干净：跟踪文件不应有未提交改动；
#    未跟踪文件必须列出来人工确认（阶段 05 的事故正是"未跟踪文件内容被改"）
# ---------------------------------------------------------------------------
echo "---- [3/3] git 工作区状态 ----"
if git rev-parse --git-dir >/dev/null 2>&1; then
    MODIFIED=$(git status --porcelain | awk '$1=="M" || $1=="MM" {print $2}')
    if [ -n "$MODIFIED" ]; then
        echo "  ⚠ 以下跟踪文件有未提交改动（交付前必须确认是否为预期改动）："
        echo "$MODIFIED" | sed 's/^/    - /'
    else
        echo "  ✓ 跟踪文件无未提交改动"
    fi
    UNTRACKED=$(git status --porcelain | awk '$1=="??" {print $2}')
    if [ -n "$UNTRACKED" ]; then
        echo "  ⚠ 未跟踪文件（git 看不到内容差异，必须人工核对/记录 sha256）："
        echo "$UNTRACKED" | sed 's/^/    - /'
        echo "    → 建议：对交付关键的未跟踪文件记录 sha256，合并时比对"
        for f in $UNTRACKED; do [ -f "$f" ] && shasum -a 256 "$f" | sed 's/^/      /'; done
    fi
else
    echo "  （非 git 仓库，跳过）"
fi
echo

if [ "$FAIL" -eq 1 ]; then
    echo "==> 门禁不通过：交付树里存在实验残留，请先还原（git checkout / 隔离副本里做实验）"
    exit 1
fi
echo "==> 门禁通过（未发现实验代码残留；git 状态请人工确认上方的 ⚠ 项）"
exit 0
