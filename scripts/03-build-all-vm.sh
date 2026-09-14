#!/usr/bin/env bash
# ============================================================================
# 03-build-all-vm.sh - 在 Linux 虚拟机内一键构建全部产物
#
# 【在虚拟机内执行】limactl shell dev  ->  bash <项目路径>/scripts/03-build-all-vm.sh
#
# 流程：
#   1. 把（只读挂载的）内核源码拷到虚拟机本地盘
#   2. 配置 + 编译 ARM64 内核 Image
#   3. 编译字符设备驱动模块 sensor_char.ko
#   4. 生成设备树：dump QEMU 的 virt dtb -> 加入自己的节点 -> 重新编译
#   5. 打包 initramfs（busybox + 驱动 + 测试程序）
# ============================================================================
set -euo pipefail

PROJ=${PROJ:-/Users/lucien/workspace/self-study/projects/linux-char-driver}
KVER=${KVER:-6.6.156}
KSRC_HOST="$PROJ/kernel-src/linux-$KVER"
KSRC="$HOME/kernel-build/linux-$KVER"
LAB="$HOME/lab"
IMG="$KSRC/arch/arm64/boot/Image"

echo "==================== [1/5] 准备内核源码 ===================="
mkdir -p "$HOME/kernel-build"
if [ ! -d "$KSRC" ]; then
    echo "  从只读挂载拷贝内核源码（约 2GB，40 秒左右）..."
    cp -a "$KSRC_HOST" "$HOME/kernel-build/"
else
    echo "  已存在：${KSRC}（如需重新开始请先删除）"
fi

echo "==================== [2/5] 配置并编译内核 ===================="
cd "$KSRC"
make defconfig >/dev/null
# 驱动开发必需的配置项：
#   DEVTMPFS*      -> /dev 自动创建（字符设备节点）
#   I2C_STUB       -> 虚拟 I2C 总线（没有真实传感器时使用）
#   I2C_CHARDEV    -> /dev/i2c-N（用户态 i2c-tools 访问）
#   FTRACE/...     -> 函数追踪，调试用
#   DEBUG_INFO     -> 符号信息，GDB 调试用
./scripts/config \
    -e DEVTMPFS -e DEVTMPFS_MOUNT \
    -e DEBUG_INFO -e DEBUG_KERNEL -e KALLSYMS -e KALLSYMS_ALL \
    -e FTRACE -e FUNCTION_TRACER -e FUNCTION_GRAPH_TRACER -e DYNAMIC_FTRACE \
    -e MAGIC_SYSRQ \
    -m I2C_STUB -e I2C_CHARDEV -e I2C \
    -e VIRTIO_BLK -e PCI_HOST_GENERIC
make olddefconfig >/dev/null
make -j"$(nproc)" Image modules
ls -lh "$IMG"

echo "==================== [3/5] 编译驱动模块 ===================="
mkdir -p "$LAB"
rm -rf "$LAB/driver"
cp -a "$PROJ/driver" "$LAB/"
cd "$LAB/driver"
make KDIR="$KSRC"
ls -lh sensor_char.ko

echo "==================== [4/5] 生成设备树（加入自己的节点）===================="
cd "$HOME"
# 让 QEMU 导出它自己生成的 virt 设备树
qemu-system-aarch64 -M virt,dumpdtb=virt.dtb -m 1G -display none -serial null -S >/dev/null 2>&1 || true
dtc -I dtb -O dts virt.dtb > virt.dts 2>/dev/null
# 把 dts/sensor-node.dts.inc 里的节点插到根节点属性之后（必须在子节点之前）
INC="$PROJ/dts/sensor-node.dts.inc"
awk -v inc="$(cat "$INC")" \
    'FNR==NR{next} /compatible = "linux,dummy-virt";/{print; print inc; next} {print}' \
    "$INC" virt.dts > virt-sensor.dts
dtc -I dts -O dtb -o virt-sensor.dtb virt-sensor.dts 2>/dev/null
echo "  设备树节点："
grep -A7 "sensor_char" virt-sensor.dts

echo "==================== [5/5] 打包 initramfs ===================="
cd "$HOME"
rm -rf rootfs
mkdir -p rootfs/{bin,proc,sys,dev,tmp,root,lib/modules/$KVER}
cp "$HOME/kernel-build/busybox" rootfs/bin/busybox || {
    echo "  缺少 busybox：请先构建静态 busybox（见 README 的构建说明）"; exit 1; }
for applet in sh mount insmod rmmod lsmod mknod dmesg ls cat head tail grep echo printf sleep poweroff uname; do
    ln -sf busybox "rootfs/bin/$applet"
done
cp "$KSRC/drivers/i2c/i2c-stub.ko" "rootfs/lib/modules/$KVER/"
cp "$LAB/driver/sensor_char.ko"     "rootfs/lib/modules/$KVER/"
# 用户态测试程序（若已在 macOS 上交叉编译过则直接拷贝）
if [ -x "$PROJ/user/sensor_test" ]; then
    cp "$PROJ/user/sensor_test" rootfs/bin/sensor_test
else
    echo "  未找到交叉编译好的 sensor_test，改为在虚拟机内编译（arm64 native）"
    gcc -static -O2 -o rootfs/bin/sensor_test "$PROJ/user/sensor_test.c" -I"$PROJ"
fi
cp "$PROJ/scripts/init-demo.sh" rootfs/init
chmod +x rootfs/init
(cd rootfs && find . | cpio -o -H newc 2>/dev/null | gzip > "$HOME/initramfs.cpio.gz")
ls -lh "$HOME/initramfs.cpio.gz"

echo
echo "==================== 构建完成 ===================="
echo "  内核镜像 : $IMG"
echo "  设备树   : $HOME/virt-sensor.dtb"
echo "  根文件系统: $HOME/initramfs.cpio.gz"
echo "  驱动模块 : $LAB/driver/sensor_char.ko"
echo
echo "下一步：bash scripts/04-run-qemu-vm.sh"
