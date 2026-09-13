#!/usr/bin/env bash
# ============================================================================
# 01-setup-macos.sh - 开发机（macOS / Apple Silicon）环境准备
#
# 作用：安装 QEMU、Lima(轻量 Linux 虚拟机)、新版 GNU Make、ARM 交叉工具链
# 注意：macOS 不能直接 insmod Linux 内核模块，所以我们需要：
#         - Lima 虚拟机  -> 编译内核/模块（Linux 环境）
#         - QEMU          -> 运行 ARM64 Linux 并加载我们的驱动
#         - musl 交叉工具链 -> 在 macOS 上交叉编译 ARM64 用户态程序
# ============================================================================
set -euo pipefail

echo "==> [1/4] 使用 Homebrew 国内镜像（可选，网络正常可跳过这几行）"
export HOMEBREW_BOTTLE_DOMAIN=https://mirrors.tuna.tsinghua.edu.cn/homebrew-bottles
export HOMEBREW_API_DOMAIN=https://mirrors.tuna.tsinghua.edu.cn/homebrew-bottles/api

echo "==> [2/4] 安装 QEMU（运行 ARM64 Linux）和 Lima（Linux 构建环境）"
brew install qemu lima

echo "==> [3/4] 安装新版 GNU Make"
# 关键：macOS 自带 make 3.81，内核要求 >= 3.82
brew install make
export PATH="/opt/homebrew/opt/make/libexec/gnubin:$PATH"
make --version | head -1

echo "==> [4/4] 安装 ARM64 交叉工具链（macOS -> Linux ARM64）"
brew tap filosottile/musl-cross
brew install filosottile/musl-cross/musl-cross

echo
echo "==> 验证安装："
for bin in qemu-system-aarch64 limactl aarch64-linux-musl-gcc; do
    printf '  %-26s %s\n' "$bin" "$(command -v $bin || echo '缺失!')"
done
echo
echo "下一步：bash scripts/02-setup-vm.sh"
