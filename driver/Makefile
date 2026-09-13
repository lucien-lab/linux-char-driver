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

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 同步编译产物到指定目录（例如 rootfs 或共享目录）
install: all
	install -d $(DESTDIR)
	install -m 644 sensor_char.ko $(DESTDIR)/

.PHONY: all clean install
