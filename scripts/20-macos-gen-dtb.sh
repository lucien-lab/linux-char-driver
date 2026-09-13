#!/usr/bin/env bash
# ============================================================================
# 20-macos-gen-dtb.sh - 【在 macOS 上执行】生成带 sensor 节点的设备树
#
# 原理：
#   1. 让 QEMU 把它自己生成的 virt 平台设备树 dump 出来（dumpdtb）
#   2. 反编译成 dts
#   3. 把 dts/sensor-node.dts.inc 里的 sensor 节点插到根节点 compatible 属性之后
#      （必须插在根节点的子节点之前，否则设备树语法错误）
#   4. 重新编译成 dtb，供 QEMU 用 -dtb 传入
#
# 为什么在 macOS 上生成：真正跑 QEMU 的是 macOS 侧，用同一版本 QEMU dump 出来的
# 设备树与运行时硬件描述严格一致（虚拟机里装的 QEMU 版本可能不同）。
#
# 依赖：qemu-system-aarch64、dtc（brew install dtc）
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")/.."
PROJ=$PWD
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

command -v dtc >/dev/null || { echo "缺少 dtc：brew install dtc"; exit 1; }
command -v qemu-system-aarch64 >/dev/null || { echo "缺少 qemu-system-aarch64：brew install qemu"; exit 1; }

echo "==> [1/4] dump QEMU virt 平台设备树"
qemu-system-aarch64 -M virt,dumpdtb="$TMP/virt.dtb" -m 1G -display none -serial null -S >/dev/null 2>&1 || true
[ -f "$TMP/virt.dtb" ] || { echo "dumpdtb 失败"; exit 1; }

echo "==> [2/4] 反编译 dtb -> dts"
dtc -I dtb -O dts -o "$TMP/virt.dts" "$TMP/virt.dtb" 2>/dev/null

echo "==> [3/4] 插入 sensor_char 节点"
python3 - "$TMP/virt.dts" "$PROJ/dts/sensor-node.dts.inc" "$TMP/virt-sensor.dts" <<'PY'
import re, sys

dts_path, inc_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(dts_path, encoding='utf-8').read().splitlines(True)
inc = open(inc_path, encoding='utf-8').read().rstrip('\n')

# 定位根节点 "/ {"
start = None
for i, ln in enumerate(lines):
    if re.match(r'^\s*/\s*\{\s*$', ln):
        start = i
        break
if start is None:
    sys.exit('未找到根节点 / {')

# 花括号配对，找到根节点自身的闭合"};"
# 为什么要插在这里而不是 compatible 后面：
#   设备树语法要求"属性必须写在子节点之前"。QEMU 不同版本 dump 出的根节点里，
#   compatible 之后可能还有其它属性（如 interrupt-parent），若直接插在 compatible
#   之后就会变成"属性-子节点-属性"从而报错：
#       Properties must precede subnodes
#   放在根节点末尾（闭合括号前）一定合法。
depth = 0
insert_at = None
for i in range(start, len(lines)):
    line = lines[i]
    depth += line.count('{') - line.count('}')
    if i > start and depth == 0 and line.lstrip().startswith('}'):
        insert_at = i
        break
if insert_at is None:
    sys.exit('未找到根节点闭合括号')

out = lines[:insert_at] + ['\n', inc + '\n'] + lines[insert_at:]
open(out_path, 'w', encoding='utf-8').writelines(out)
print('  插入位置：根节点第 %d 行闭合括号之前' % (insert_at + 1))
PY

echo "==> [4/4] 编译 dts -> dtb 并输出到 artifacts/"
mkdir -p "$PROJ/artifacts"
if ! dtc -I dts -O dtb -o "$PROJ/artifacts/virt-sensor.dtb" "$TMP/virt-sensor.dts" 2>"$TMP/dtc.err"; then
    echo "dtc 编译失败："; cat "$TMP/dtc.err"; exit 1
fi
cp "$TMP/virt-sensor.dts" "$PROJ/artifacts/virt-sensor.dts"

echo
echo "生成的传感器节点（I2C 控制器 + 子设备，由 i2c 核心从设备树枚举）："
grep -A12 -E 'virt-i2c \{' "$PROJ/artifacts/virt-sensor.dts" | sed 's/^/  /'
ls -lh "$PROJ/artifacts/virt-sensor.dtb"
