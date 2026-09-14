#!/usr/bin/env bash
# ============================================================================
# 15-vm-build-kcsan.sh - 【在 Lima Linux 虚拟机内执行】构建/还原 KCSAN 内核
#
# KCSAN（Kernel Concurrency Sanitizer）是内核自带的动态数据竞争检测器：
# 它会在运行时给内存访问插桩，发现"两个 CPU 无同步地访问同一地址"就打印
#   BUG: KCSAN: data-race in ...
# 这是证明"驱动无数据竞争"最直接的证据（概率性并发测试只能提高置信度）。
#
# 用法（在虚拟机内）：
#   bash <项目>/scripts/15-vm-build-kcsan.sh          # 打开 KCSAN 并重建内核
#   bash <项目>/scripts/15-vm-build-kcsan.sh restore  # 还原普通配置并重建
#
# 注意：
#   * 会修改内核 .config（本工程唯一允许的内核源码树改动例外），原始配置备份为
#     $KSRC/.config.no-kcsan，用完必须 restore，否则后续阶段都在 KCSAN 内核上跑。
#   * 配置变化会触发全量重编（约 8~15 分钟）。
#   * arm64 需要 CONFIG_EXPERT 才能选中 HAVE_ARCH_KCSAN（arch/arm64/Kconfig:170）。
#   * 必须在 restore 之后重新构建驱动模块与 initramfs，产物才与内核匹配。
# ============================================================================
set -uo pipefail

KVER=${KVER:-6.6.156}
KSRC="$HOME/kernel-build/linux-$KVER"
MODE=${1:-enable}

command -v gcc-13 >/dev/null 2>&1 && export CC=gcc-13 HOSTCC=gcc-13
cd "$KSRC" || exit 1

if [ "$MODE" = "restore" ]; then
    if [ ! -f .config.no-kcsan ]; then
        echo "缺少备份 .config.no-kcsan，无法还原"; exit 1
    fi
    cp .config.no-kcsan .config
    echo "==> 已还原原始 .config，重建内核（$(date '+%T')）"
else
    [ -f .config.no-kcsan ] || cp .config .config.no-kcsan
    # KCSAN 需要：EXPERT（arm64 的 HAVE_ARCH_KCSAN 依赖它）、DEBUG_KERNEL、非 KASAN
    ./scripts/config -e EXPERT -e KCSAN -e KCSAN_VERBOSE
    echo "==> 已打开 KCSAN，重建内核（$(date '+%T')）"
fi

make olddefconfig >/dev/null 2>&1
grep -E "^CONFIG_KCSAN=|^CONFIG_EXPERT=|^# CONFIG_KCSAN is not set" .config | sed 's/^/    /'

make -j"$(nproc)" Image modules
rc=$?
echo "==> make 退出码=${rc}（$(date '+%T')）"
ls -lh arch/arm64/boot/Image 2>/dev/null
exit $rc
