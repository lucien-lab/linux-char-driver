#!/usr/bin/env bash
# ============================================================================
# 05-debug-gdb.sh - 用 GDB 调试 QEMU 里运行的 ARM64 内核 / 驱动模块
#
# 【在虚拟机内执行】limactl shell dev -> bash <项目路径>/scripts/05-debug-gdb.sh
#
# 原理：
#   qemu -s   : 打开 gdbstub，监听 localhost:1234
#   qemu -S   : 启动后暂停 CPU，等待调试器
#   gdb       : 通过 vmlinux（带 DEBUG_INFO）拿到所有内核符号和源码行
#
# 说明：内核模块是运行时加载的，符号需要 add-symbol-file 动态注册，
#       脚本最后会打印手动调试模块的完整步骤。
# ============================================================================
set -euo pipefail

KSRC=${KSRC:-$HOME/kernel-build/linux-6.6.156}
IMAGE="$KSRC/arch/arm64/boot/Image"
DTB=$HOME/virt-sensor.dtb
INITRD=$HOME/initramfs.cpio.gz

echo "==> 启动 QEMU（暂停状态，等待 GDB 连接）"
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1G -display none -serial null \
    -dtb "$DTB" -kernel "$IMAGE" -initrd "$INITRD" \
    -append "console=ttyAMA0 nokaslr" -S -s &
QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true' EXIT

sleep 2

echo "==> GDB 连接并下断点：i2c_get_adapter（驱动 probe 会调用它）"
# 批处理模式：连接 -> 断点 -> 继续 -> 命中后打印调用栈 -> 退出
timeout 60 gdb-multiarch -q "$KSRC/vmlinux" \
    -ex "target remote :1234" \
    -ex "break i2c_get_adapter" \
    -ex "break i2c_new_client_device" \
    -ex "continue" \
    -ex "echo \n==== 命中断点，当前调用栈 ====\n" \
    -ex "bt 6" \
    -ex "info registers pc" \
    -ex "detach" \
    -ex "quit" || true

echo
cat <<'EOF'
================================================================================
手动调试内核模块（完整步骤）
================================================================================
终端 1（虚拟机内）：带调试参数启动 QEMU，进入交互 shell 后手动加载驱动
    qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1G -display none -serial stdio \
      -dtb ~/virt-sensor.dtb -kernel ~/kernel-build/linux-6.6.156/arch/arm64/boot/Image \
      -initrd ~/initramfs.cpio.gz -append "console=ttyAMA0 nokaslr" -S -s

    (qemu) c                     # 继续启动
    # 在 guest shell 里：
    insmod /lib/modules/6.6.156/i2c-stub.ko chip_addr=0x48
    insmod /lib/modules/6.6.156/sensor_char.ko
    cat /sys/module/sensor_char/sections/.text     # 记下这个地址，例如 0xffff8000801a0000

终端 2（虚拟机内）：另开一个 limactl shell
    gdb-multiarch ~/kernel-build/linux-6.6.156/vmlinux
    (gdb) target remote :1234
    (gdb) add-symbol-file ~/lab/driver/sensor_char.ko 0xffff8000801a0000   # 上一步的地址
    (gdb) break sensor_irq_thread        # 在线程化中断处理里下断点
    (gdb) continue
    (gdb) bt                             # 确认中断下半部调用栈
    (gdb) p sd->latest                   # 直接看驱动内部数据结构
    (gdb) finish / next / step           # 单步
    (gdb) disconnect / quit

常见坑：
  * 不加 nokaslr 时 vmlinux 与运行时地址不一致，断点会打偏。
  * 忘记 add-symbol-file 会提示 "No symbol table is loaded"。
  * 模块卸载后符号失效，需要重新 insmod 并重新 add-symbol-file。
EOF
