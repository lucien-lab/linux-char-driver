#!/usr/bin/env bash
# ============================================================================
# 04-run-qemu-vm.sh - 启动 QEMU 运行 ARM64 Linux + 我们的驱动
#
# 【在虚拟机内执行】limactl shell dev  ->  bash <项目路径>/scripts/04-run-qemu-vm.sh
#
# 两种模式：
#   自动演示（默认）：开机自动跑完 insmod/ftrace/用户态测试并打印，然后关机
#   交互模式(INTERACTIVE=1)：进入 shell，可手动敲命令（推荐动手练习时用）
# ============================================================================
set -euo pipefail

KSRC=${KSRC:-$HOME/kernel-build/linux-6.6.156}
IMAGE="$KSRC/arch/arm64/boot/Image"
DTB=$HOME/virt-sensor.dtb
INITRD=$HOME/initramfs.cpio.gz

for f in "$IMAGE" "$DTB" "$INITRD"; do
    [ -e "$f" ] || { echo "缺少产物：${f}，请先运行 scripts/03-build-all-vm.sh"; exit 1; }
done

# 说明：
#   -M virt        通用虚拟平台（ARM64）
#   -cpu cortex-a72 模拟 CPU（贴近真实开发板）
#   -dtb           传入我们改过的设备树（多了 sensor 节点，驱动靠它匹配）
#   -serial stdio  串口接到当前终端
#   -append        内核命令行；交互模式加 nokaslr 方便 GDB 调试
ARGS=(-M virt -cpu cortex-a72 -m 1G -display none -serial stdio
      -dtb "$DTB" -kernel "$IMAGE" -initrd "$INITRD")

if [ "${INTERACTIVE:-0}" = "1" ]; then
    echo "== 交互模式：进入 shell 后可手动执行 insmod / cat /dev/sensor0 等 =="
    echo "== 提示：insmod /lib/modules/6.6.156/i2c-stub.ko chip_addr=0x48  ;  insmod /lib/modules/6.6.156/sensor_char.ko"
    exec qemu-system-aarch64 "${ARGS[@]}" -append "console=ttyAMA0 nokaslr"
else
    exec qemu-system-aarch64 "${ARGS[@]}" -append "console=ttyAMA0"
fi
