#!/usr/bin/env bash
# ============================================================================
# 21-macos-sync-artifacts.sh - 【在 macOS 上执行】把虚拟机内构建的产物拷回项目
#
# 背景：Lima 把宿主 $HOME 以【只读】方式挂进虚拟机，所以虚拟机编译出的东西
#       无法直接写到项目目录，必须用 limactl copy 拷出来。
#
# 用法：bash scripts/21-macos-sync-artifacts.sh
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")/.."
PROJ=$PWD
VM=${VM:-dev}

GUEST_HOME=$(limactl shell "$VM" bash -c 'echo $HOME')
mkdir -p "$PROJ/artifacts"

echo "==> 从 $VM:$GUEST_HOME/lab/out/ 同步产物到 artifacts/"
for f in initramfs.cpio.gz Image i2c-stub.ko; do
    if limactl copy "$VM:$GUEST_HOME/lab/out/$f" "$PROJ/artifacts/$f" 2>/dev/null; then
        printf '  %-22s %s\n' "$f" "$(du -h "$PROJ/artifacts/$f" | cut -f1)"
    else
        echo "  !! 同步失败（不存在？）: $f"
    fi
done

# 构建补丁与日志（便于写文档时引用）
mkdir -p "$PROJ/artifacts/logs"
for f in patches/0001-host-tools-newer-glibc.patch BUILD-INFO.txt build-image.log; do
    limactl copy "$VM:$GUEST_HOME/$f" "$PROJ/artifacts/logs/$(basename "$f")" 2>/dev/null && \
        echo "  日志/补丁: $(basename "$f")" || true
done

echo
echo "==> artifacts/ 当前内容："
ls -lh "$PROJ/artifacts/" | sed 's/^/  /'
