#!/usr/bin/env bash
# ============================================================================
# 02-setup-vm.sh - 创建并初始化 Linux 构建虚拟机（lima）
#
# 在 macOS 上执行。完成后会得到一个 arm64 Ubuntu 虚拟机，里面有：
#   gcc / make / flex / bison / bc / libssl-dev / libelf-dev / qemu / dtc / i2c-tools
#
# 重要：lima 默认把宿主 $HOME 以【只读】方式挂载进虚拟机。
#       所以内核编译放在虚拟机本地盘（~/kernel-build），源码从只读挂载拷进去。
# ============================================================================
set -euo pipefail

VM_NAME=${VM_NAME:-dev}

echo "==> [1/3] 创建虚拟机（Ubuntu LTS, arm64）"
if limactl list --format '{{.Name}}' | grep -qx "$VM_NAME"; then
    echo "  虚拟机 $VM_NAME 已存在，跳过创建"
else
    limactl start --name="$VM_NAME" template://ubuntu-lts
fi

echo "==> [2/3] 切换 apt 源为清华镜像（国内网络必需，否则 apt 可能 502）"
limactl shell "$VM_NAME" bash -c '
    sudo sed -i "s|http://archive.ubuntu.com/ubuntu|https://mirrors.tuna.tsinghua.edu.cn/ubuntu|g; \
                s|http://security.ubuntu.com/ubuntu|https://mirrors.tuna.tsinghua.edu.cn/ubuntu|g" \
        /etc/apt/sources.list.d/*.sources /etc/apt/sources.list 2>/dev/null || true
    sudo apt-get update -qq
'

echo "==> [3/3] 安装内核编译 + 运行 + 调试所需软件包"
limactl shell "$VM_NAME" bash -c '
    sudo apt-get install -y -qq \
        build-essential flex bison bc libssl-dev libelf-dev \
        kmod cpio device-tree-compiler i2c-tools \
        qemu-system-arm gdb-multiarch rsync
    echo "--- 版本 ---"
    gcc --version | head -1
    qemu-system-aarch64 --version | head -1
    gdb-multiarch --version | head -1
'

echo
echo "==> 完成。进入虚拟机："
echo "    limactl shell $VM_NAME"
echo "下一步：把内核源码放进虚拟机并编译 -> bash scripts/03-build-all-vm.sh（在虚拟机内执行）"
