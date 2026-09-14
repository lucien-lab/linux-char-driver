#!/usr/bin/env bash
# ============================================================================
# 13-vm-fast-cycle.sh - 【在 Lima Linux 虚拟机内执行】快速迭代构建
#
# 这是整个开发循环的核心：把"改驱动 → 编译 → 打包 initramfs"压缩到 20~40 秒，
# 不重新编译内核。
#
# 用法（macOS 侧，指定要跑哪个阶段测试）：
#   limactl shell dev bash -c 'cd ~ && TEST=01-io-models \
#       bash /Users/.../linux-char-driver/scripts/13-vm-fast-cycle.sh'
#
# 产物统一放到 $HOME/lab/out/ ：
#   initramfs.cpio.gz  根文件系统（含 busybox + 内核模块 + 用户态测试程序 + 测试脚本）
#   Image              内核镜像（从内核树拷来，方便统一同步）
#   i2c-stub.ko        虚拟 I2C 总线模块
#   virt-sensor.dtb    设备树（由 macOS 侧 20-macos-gen-dtb.sh 生成，这里只做校验）
# ============================================================================
set -uo pipefail

# PROJ 默认取"脚本自身所在的项目目录"：这样在 git worktree 里执行时会自动指向该 worktree，
# 而不是硬编码的主树 —— 否则并行 lane 会拿别人的源码构建，产生完全错误的结论（阶段 05 踩过）。
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJ=${PROJ:-$(cd "$SCRIPT_DIR/.." && pwd)}
KVER=${KVER:-6.6.156}
KSRC="$HOME/kernel-build/linux-$KVER"
LAB="$HOME/lab-${LANE:-main}"
OUT="$LAB/out"
TEST=${TEST:-smoke}

command -v gcc-13 >/dev/null 2>&1 && export CC=gcc-13 HOSTCC=gcc-13

echo "==================== [1/5] 同步源码到虚拟机本地盘 ===================="
mkdir -p "$LAB" "$OUT"
for d in driver user tests; do
    mkdir -p "$LAB/$d"
    if command -v rsync >/dev/null 2>&1; then
        rsync -a --delete "$PROJ/$d/" "$LAB/$d/"
    else
        rm -rf "$LAB/$d"; cp -a "$PROJ/$d" "$LAB/$d"
    fi
done
# dts 也同步一份（便于在虚拟机里核对节点）
mkdir -p "$LAB/dts" && cp -a "$PROJ/dts/." "$LAB/dts/" 2>/dev/null || true
echo "  driver/ user/ tests/ 已同步"

echo "==================== [2/5] 编译内核模块 ===================="
cd "$LAB/driver"
# 增量编译：kbuild 只重编改动的文件
make KDIR="$KSRC" -j"$(nproc)" 2>&1 | grep -E "CC \[M\]|LD \[M\]|error|warning|Error" | head -40
KO_LIST=$(ls "$LAB"/driver/*.ko 2>/dev/null)
if [ -z "$KO_LIST" ]; then
    echo "!! 没有生成 .ko，编译失败"; exit 1
fi
for ko in $KO_LIST; do echo "  生成: $(basename "$ko") ($(du -h "$ko" | cut -f1))"; done

echo "==================== [3/5] 编译用户态测试程序（静态链接 aarch64）===================="
mkdir -p "$LAB/bin"
rm -f "$LAB/bin"/*
build_ok=1
for src in "$LAB"/user/*.c "$LAB"/tests/userspace/*.c; do
    [ -f "$src" ] || continue
    bin="$LAB/bin/$(basename "${src%.c}")"
    # 阶段测试程序可能依赖其他 .c（如公共库），用 -I 暴露驱动程序目录
    if ! gcc -static -O2 -Wall -I"$LAB/driver" -o "$bin" "$src" 2> "/tmp/cc-$(basename "$src").err"; then
        echo "!! 编译失败: $(basename "$src")"
        sed -n '1,15p' "/tmp/cc-$(basename "$src").err"
        build_ok=0
    else
        echo "  生成: $(basename "$bin")"
    fi
done
[ "$build_ok" = "1" ] || { echo "!! 用户态程序编译失败"; exit 1; }

echo "==================== [4/5] 打包 initramfs ===================="
ROOTFS="$LAB/rootfs"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{bin,proc,sys,dev,tmp,root,tests,etc,lib/modules/$KVER,sys/kernel/debug,sys/kernel/config}

# busybox（Ubuntu 的 busybox-static 提供静态 aarch64 版本）
cp /bin/busybox "$ROOTFS/bin/busybox"
for applet in sh ash mount umount insmod rmmod lsmod modprobe mknod dmesg ls cat head tail \
              grep echo printf sleep poweroff reboot uname sync mkdir rmdir rm cp mv ln chmod \
              dd od hexdump awk sed cut tr wc tee find sort uniq expr test kill ps date \
              du df id env more less vi; do
    ln -sf busybox "$ROOTFS/bin/$applet" 2>/dev/null
done
# [ 是 busybox 的 applet（用于 [ -f file ] 判断）
ln -sf busybox "$ROOTFS/bin/[" 2>/dev/null
ln -sf busybox "$ROOTFS/bin/[[" 2>/dev/null

# 模块加载顺序（项目内 driver/modules.load 定义，便于版本管理）
# 阶段可以覆盖：tests/phases/<阶段名>.modules 存在时优先使用（例如 IIO 阶段
# 只需要 virt_i2c + sensor_iio，不能同时加载占用同一 i2c 从设备的字符设备驱动）
# 注意：先把清单算出来再拷模块，才能只打包真正被引用的内核树模块（如 i2c-stub）。
MODSRC="$LAB/driver/modules.load"
[ -f "$LAB/tests/phases/$TEST.modules" ] && MODSRC="$LAB/tests/phases/$TEST.modules"
if [ -f "$MODSRC" ]; then
    grep -v '^[[:space:]]*#' "$MODSRC" | grep -v '^[[:space:]]*$' > "$ROOTFS/etc/modules.load"
else
    printf 'i2c-stub.ko chip_addr=0x48\n' > "$ROOTFS/etc/modules.load"
    for ko in "$LAB"/driver/*.ko; do echo "$(basename "$ko")" >> "$ROOTFS/etc/modules.load"; done
fi
echo "  /etc/modules.load (来源: $(basename "$MODSRC")):"; sed 's/^/    /' "$ROOTFS/etc/modules.load"

# 内核模块：驱动自己编译的产物
cp "$LAB"/driver/*.ko "$ROOTFS/lib/modules/$KVER/"
# 内核树里的模块：只在模块清单确实引用时才打包。
# 阶段 02 之后已经不需要 i2c-stub（virt_i2c 提供带设备树节点的适配器），
# 无条件的把桩模块打进 initramfs 只会让产物里多一个用不到的东西。
if grep -q '^i2c-stub\.ko' "$ROOTFS/etc/modules.load"; then
    cp "$KSRC/drivers/i2c/i2c-stub.ko" "$ROOTFS/lib/modules/$KVER/" 2>/dev/null
fi
for dep in regmap-i2c industrialio industrialio-triggered-buffer kfifo-buf iio-trig-hrtimer; do
    for p in "$KSRC/drivers/base/regmap/$dep.ko" "$KSRC/drivers/iio/$dep.ko" \
             "$KSRC/drivers/iio/buffer/$dep.ko" "$KSRC/drivers/iio/trigger/$dep.ko" \
             "$KSRC/lib/$dep.ko"; do
        [ -f "$p" ] && cp "$p" "$ROOTFS/lib/modules/$KVER/" 2>/dev/null
    done
done

# 用户态程序
cp "$LAB"/bin/* "$ROOTFS/bin/" 2>/dev/null

# 测试框架
cp "$LAB/tests/runner/lib.sh"  "$ROOTFS/tests/lib.sh"
cp "$LAB/tests/runner/init.sh" "$ROOTFS/init"
chmod +x "$ROOTFS/init"

# 注入本次要跑的阶段脚本（作为默认值；实际执行哪个可以由内核命令行 test= 覆盖）
if [ -f "$LAB/tests/phases/$TEST.sh" ]; then
    cp "$LAB/tests/phases/$TEST.sh" "$ROOTFS/tests/phase.sh"
    sed -i "s/^TEST_NAME=.*/TEST_NAME=$TEST/" "$ROOTFS/init"
    echo "  默认阶段测试: $TEST"
else
    echo "  !! 找不到阶段脚本 tests/phases/$TEST.sh"
    echo "TEST_NAME=$TEST" >> "$ROOTFS/init"
fi

# 把所有阶段脚本都打进 initramfs：这样一次构建可以跑任意阶段测试
# （否则跑 smoke 回归时会重复执行上一次注入的脚本，得出假结论）
mkdir -p "$ROOTFS/tests/phases"
cp "$LAB"/tests/phases/*.sh "$ROOTFS/tests/phases/" 2>/dev/null
# 阶段级模块清单也一并带上（如 IIO 阶段只加载 virt_i2c + sensor_iio）
cp "$LAB"/tests/phases/*.modules "$ROOTFS/tests/phases/" 2>/dev/null

# ---------------------------------------------------------------------------
# 模块清单同样要"运行时可选"，否则会出现一类很难查的假结论：
#   initramfs 是构建期把 /etc/modules.load 烤进去的。
#   如果构建时用的是某个阶段的专属清单（例如 IIO 阶段只加载 virt_i2c+sensor_iio），
#   那么同一份产物去跑 smoke 回归时 sensor_char.ko 根本没被 insmod，
#   smoke 会大面积 FAIL —— 看起来像"回归被改坏了"，实际是清单不匹配。
#   做法：把默认清单与每个阶段的专属清单都放进 /etc/modules/，
#         由 init.sh 按内核命令行 test=<阶段> 选择同名清单。
# ---------------------------------------------------------------------------
mkdir -p "$ROOTFS/etc/modules"
if [ -f "$LAB/driver/modules.load" ]; then
    grep -v '^[[:space:]]*#' "$LAB/driver/modules.load" | grep -v '^[[:space:]]*$' \
        > "$ROOTFS/etc/modules/default.load"
else
    printf 'i2c-stub.ko chip_addr=0x48\n' > "$ROOTFS/etc/modules/default.load"
    for ko in "$LAB"/driver/*.ko; do echo "$(basename "$ko")" >> "$ROOTFS/etc/modules/default.load"; done
fi
for f in "$LAB"/tests/phases/*.modules; do
    [ -f "$f" ] || continue
    grep -v '^[[:space:]]*#' "$f" | grep -v '^[[:space:]]*$' \
        > "$ROOTFS/etc/modules/$(basename "${f%.modules}").load"
done
echo "  可用模块清单（/etc/modules/）："
ls "$ROOTFS/etc/modules/" | sed 's/^/    /'

# /etc/modules.load 必须始终是"默认清单"：阶段专属清单只在 init.sh 里按 test= 选择。
# 否则构建时用的阶段清单会被当成默认值，导致同一份产物跑 smoke 回归时缺驱动
# （sensor_char.ko 没被 insmod → smoke 大面积 FAIL，看起来像回归被改坏）。
cp "$ROOTFS/etc/modules/default.load" "$ROOTFS/etc/modules.load"
echo "  默认模块清单已重置为 default.load"

# 内核树里的模块（如 i2c-stub）只要被任一清单引用就要打包
if grep -lq '^i2c-stub\.ko' "$ROOTFS"/etc/modules/*.load "$ROOTFS/etc/modules.load" 2>/dev/null; then
    cp "$KSRC/drivers/i2c/i2c-stub.ko" "$ROOTFS/lib/modules/$KVER/" 2>/dev/null
fi
for f in "$ROOTFS"/tests/phases/*.sh; do
    [ -f "$f" ] || continue
    # .sh 结尾的阶段脚本在 initramfs 里不需要可执行位（用 . 加载），
    # 但 /tests/phase.sh 这份默认副本会被其他脚本复用，保持权限一致即可
    chmod 644 "$f"
done

# 打包
( cd "$ROOTFS" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "$OUT/initramfs.cpio.gz" )
echo "  initramfs: $(du -h "$OUT/initramfs.cpio.gz" | cut -f1)"

echo "==================== [5/5] 汇总产物 ===================="
cp "$KSRC/arch/arm64/boot/Image" "$OUT/Image"
cp "$KSRC/drivers/i2c/i2c-stub.ko" "$OUT/i2c-stub.ko"
ls -lh "$OUT"
echo
echo "==> 下一步（macOS 侧）：bash scripts/21-macos-sync-artifacts.sh && bash scripts/22-macos-run-test.sh $TEST"
