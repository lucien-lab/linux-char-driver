#!/usr/bin/env bash
# ============================================================================
# 10-vm-build-kernel.sh - 【在 Lima Linux 虚拟机内执行】一次性全量构建
#
# 与 03-build-all-vm.sh 的区别：
#   03 是最初的"从零构建"；本脚本是在 03 的基础上补齐后续阶段需要的配置项：
#     - IIO 子系统 + triggered buffer + sw/hrtimer trigger + configfs
#       （用于第 4 阶段：把字符设备驱动重写为 IIO 驱动）
#     - KUnit（用于第 6 阶段：内核单元测试）
#     - PM_ADVANCED_DEBUG（用于第 5 阶段：Runtime PM 调试）
#
# 用法（在 macOS 上）：
#   limactl shell dev bash /Users/lucien/workspace/self-study/projects/linux-char-driver/scripts/10-vm-build-kernel.sh
#
# 耗时：约 20-40 分钟（4 vCPU / 4GB 虚拟机）。建议用 nohup 后台跑。
# ============================================================================
set -euo pipefail

PROJ=${PROJ:-/Users/lucien/workspace/self-study/projects/linux-char-driver}
KVER=${KVER:-6.6.156}
KSRC_HOST="$PROJ/kernel-src/linux-$KVER"
KSRC="$HOME/kernel-build/linux-$KVER"

echo "==================== [1/4] 准备内核源码 ===================="
mkdir -p "$HOME/kernel-build"
if [ ! -d "$KSRC" ]; then
    echo "  从只读挂载拷贝内核源码（约 2GB，40 秒左右）..."
    cp -a "$KSRC_HOST" "$HOME/kernel-build/"
else
    echo "  已存在：$KSRC"
fi

echo "==================== [2/4] 基于已验证的 .config 追加配置 ===================="
cd "$KSRC"
# 保留 host 侧 kernel-src/.config（它正是产出 artifacts/Image 的配置），只做增量修改，
# 避免 defconfig 把已验证可通过的配置项冲掉。
[ -f .config ] || make defconfig
cp .config .config.pre-10stage

./scripts/config \
    -e IIO -e IIO_BUFFER -e IIO_TRIGGER -e IIO_TRIGGERED_BUFFER -e IIO_KFIFO_BUF \
    -e IIO_SW_TRIGGER -e IIO_CONFIGFS -e IIO_HRTIMER_TRIGGER \
    -e CONFIGFS_FS -e REGMAP -e REGMAP_I2C \
    -e KUNIT -e PM_ADVANCED_DEBUG -e DEBUG_FS

echo "  olddefconfig 展开依赖..."
make olddefconfig >/dev/null

echo "  --- 关键配置核对（=y 表示编进内核镜像，模块可直接链接；=m 需要额外 insmod）---"
for s in IIO IIO_BUFFER IIO_TRIGGER IIO_TRIGGERED_BUFFER IIO_KFIFO_BUF \
         IIO_SW_TRIGGER IIO_CONFIGFS IIO_HRTIMER_TRIGGER CONFIGFS_FS \
         REGMAP REGMAP_I2C KUNIT DEBUG_FS I2C_STUB FUNCTION_TRACER DEBUG_INFO; do
    printf '  %-26s %s\n' "$s" "$(grep -E "^CONFIG_$s=|^# CONFIG_$s is not set" .config | head -1 | sed 's/# //')"
done

# ---------------------------------------------------------------------------
# 【踩坑】编译器版本：Ubuntu 25.10 默认 gcc 15，内核 6.6 的宿主工具
# (scripts/mod/file2alias.c 等) 在 gcc 15(默认 C23) 下会编译失败：
#   scripts/mod/file2alias.c:1353: error: request for member 'b' in something
#   not a structure or union   （uuid->b 处报错）
# 内核 6.6 LTS 官方支持的 gcc 上限为 13.x，因此显式指定 gcc-13。
# 安装：sudo apt-get install -y gcc-13
# ---------------------------------------------------------------------------
if command -v gcc-13 >/dev/null 2>&1; then
    export CC=gcc-13 HOSTCC=gcc-13
    echo "  使用 gcc-13（内核 6.6 官方支持上限）"
fi

NPROC_JOBS=${JOBS:-$(nproc)}
echo "==================== [3/4] 编译内核 Image + modules ===================="
echo "  编译器  : ${CC:-gcc} / 宿主工具 ${HOSTCC:-gcc}"
echo "  并行度  : -j$NPROC_JOBS"
echo "  开始时间：$(date '+%F %T')"
make -j"$NPROC_JOBS" Image modules
echo "  结束时间：$(date '+%F %T')"
ls -lh arch/arm64/boot/Image

echo "==================== [4/4] 记录产物清单 ===================="
cat > "$HOME/BUILD-INFO.txt" <<EOF
构建时间: $(date '+%F %T')
内核版本: $KVER
源码树  : $KSRC
Image   : $KSRC/arch/arm64/boot/Image
i2c-stub: $KSRC/drivers/i2c/i2c-stub.ko
EOF
cat "$HOME/BUILD-INFO.txt"
echo "==> 内核构建完成。下一步（macOS 侧）：bash scripts/20-sync-artifacts.sh"
