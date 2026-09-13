#!/usr/bin/env bash
# ============================================================================
# 12-vm-build-image.sh - 【在 Lima Linux 虚拟机内执行】全量编译内核 Image + modules
#
# 前置：先跑 scripts/11-vm-prepare-pristine.sh（解干净源码 + 打补丁 + 生成 .config）
# 耗时：约 20-30 分钟（4 vCPU）。建议 nohup 后台执行：
#   limactl shell dev bash -c 'cd ~ && nohup bash <项目路径>/scripts/12-vm-build-image.sh > ~/build-image.log 2>&1 &'
# ============================================================================
set -euo pipefail

KVER=${KVER:-6.6.156}
KSRC="$HOME/kernel-build/linux-$KVER"
JOBS=${JOBS:-$(nproc)}

command -v gcc-13 >/dev/null 2>&1 && export CC=gcc-13 HOSTCC=gcc-13

cd "$KSRC"
[ -f .config ] || { echo "缺少 .config，请先跑 scripts/11-vm-prepare-pristine.sh"; exit 1; }

echo "==================== 编译内核 ===================="
echo "  源码树  : $KSRC"
echo "  编译器  : ${CC:-gcc} / 宿主 ${HOSTCC:-gcc}"
echo "  并行度  : -j$JOBS"
echo "  开始时间: $(date '+%F %T')"
make -j"$JOBS" Image modules
echo "  结束时间: $(date '+%F %T')"
ls -lh arch/arm64/boot/Image arch/arm64/boot/Image.gz 2>/dev/null || true
ls -lh drivers/i2c/i2c-stub.ko

cat > "$HOME/BUILD-INFO.txt" <<EOF
构建时间   : $(date '+%F %T')
内核版本   : $KVER
源码树     : $KSRC
编译器     : ${CC:-gcc}
Image      : $KSRC/arch/arm64/boot/Image
i2c-stub.ko: $KSRC/drivers/i2c/i2c-stub.ko
EOF
echo "==> 完成。下一步：bash scripts/20-macos-sync-artifacts.sh（macOS 侧同步产物）"
