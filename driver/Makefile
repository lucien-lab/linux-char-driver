# SPDX-License-Identifier: GPL-2.0
#
# 外部内核模块 Makefile（kbuild 外部编译标准写法）
#
# 用法（在目标 Linux 构建环境里）：
#   make KDIR=~/kernel-build/linux-6.6.156
#   make KDIR=... clean
#
# KDIR 指向已配置好的内核源码树（需要先跑过 make modules_prepare 或完整编译）。

obj-m += sensor_char.o
obj-m += virt_i2c.o
# IIO 子系统版驱动（阶段 05）：与字符设备版并存，用另一颗同型号芯片（0x49）避免争抢。
obj-m += sensor_iio.o

# KUnit 单元测试模块（阶段 06）：只测纯逻辑（sensor_calc.h），
# 不参与正常功能，因此单独一个 .ko，由阶段 06 的测试脚本按需 insmod。
# 注意：KUnit 的 kunit_* 符号在内核里是 EXPORT_SYMBOL_GPL，本模块是 GPL，
# 所以可以直接编成外部模块，不需要把它编进内核。
obj-m += sensor_kunit.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 同步编译产物到指定目录（例如 rootfs 或共享目录）
install: all
	install -d $(DESTDIR)
	install -m 644 $(patsubst %.o,%.ko,$(obj-m)) $(DESTDIR)/

.PHONY: all clean install
