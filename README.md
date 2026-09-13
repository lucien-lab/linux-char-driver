# Linux 字符设备驱动开发与 I2C 传感器数据采集

> 个人项目 · 独立开发 · C / Linux Kernel / ARM64
> 2025.12 — 2026.02

在 **macOS (Apple Silicon) 开发机**上搭建一套完整的 Linux 驱动开发环境：
交叉编译 ARM64 内核 → QEMU 运行 ARM64 Linux → 内核字符设备驱动 + I2C 传感器采集 → 设备树解耦 → dmesg/ftrace/GDB 调试。

**整套流程已实测跑通**，产物在 `artifacts/` 目录，可一键复现。

---

## 一、项目做了什么（对应需求）

| 需求 | 实现 | 关键文件 |
|------|------|----------|
| 字符设备驱动（file_operations / 主次设备号 / open·read·ioctl） | `fops` 全实现：阻塞 read、文本/结构体双通道、4 个 ioctl 命令、write 下发命令 | `driver/sensor_char.c` |
| 驱动内调用 I2C 子系统读传感器 | `i2c_get_adapter()` + `i2c_new_client_device()` + `i2c_smbus_read/write_word_data()` | `driver/sensor_char.c` |
| 中断处理 | 自建虚拟中断控制器（`irq_chip`+`irq_domain`）+ `request_threaded_irq`，上半部/线程化下半部分离 | `driver/sensor_char.c` |
| 打通内核态→用户态数据通路 | 线程化中断读传感器 → 内核缓存 + waitqueue → `read()`/`ioctl()` 唤醒返回 | `driver/sensor_char.c` `user/sensor_test.c` |
| 设备树解耦匹配 | `of_match_table` 匹配 `compatible`，总线号/地址/周期全部来自 dts | `dts/` `driver/sensor_char.c` |
| 交叉编译工具链 + ARM 平台编译模块 | macOS 上 `aarch64-linux-musl-gcc` 交叉编译用户态程序；Linux VM 内编译内核与 .ko | `scripts/01/03` |
| insmod / rmmod 验证 | 自动演示脚本含加载、probe 日志、节点检查、卸载全流程 | `scripts/init-demo.sh` |
| dmesg / ftrace / GDB 定位问题 | ftrace 抓到线程化中断执行轨迹；GDB 断点命中 `i2c_get_adapter`；真实 WARNING 案例的定位与修复 | `docs/03-调试方法手册.md` |

---

## 二、实测结果

```
=== [1] BOOT: 6.6.156 / aarch64 ===
=== [2] 加载驱动（i2c-stub 提供虚拟 I2C 总线，slave 地址 0x48）===
sensor_char sensor-char: DT: i2c-bus=0 sensor-addr=0x48 interval=500ms   <- 设备树参数
sensor_char sensor-char: i2c client on bus 0 addr 0x48                   <- I2C 子系统
sensor_char sensor-char: irq registered: virq=21                         <- 中断注册
sensor_char sensor-char: probe done: major=511 minor=0 /dev/sensor0      <- 字符设备
crw------- 1 0 0 511, 0 /dev/sensor0
=== [3] ftrace 追踪 sensor_irq_thread ===
 irq/21-sensor_c-59  [000] .....  1.782342: sensor_irq_thread <-irq_thread_fn
 irq/21-sensor_c-59  [000] .....  2.276037: sensor_irq_thread <-irq_thread_fn
 irq/21-sensor_c-59  [000] .....  2.777669: sensor_irq_thread <-irq_thread_fn
=== [4] 用户态测试程序 sensor_test ===
  [read 1] seq=6 temp=25.412C irq=6 t=3751238784 ns
  [read 2] seq=7 temp=25.475C irq=7 t=4254244304 ns
  [read 3] seq=8 temp=25.537C irq=8 t=4754762640 ns
  == 2) ioctl(SENSOR_IOC_SET_INTERVAL, 200) ==
  [read 1] seq=9  temp=25.400C irq=9  t=4959992096 ns   <- 周期 500ms -> 200ms 生效
  == 3) ioctl(SENSOR_IOC_GET_SAMPLE) ==        seq=11 temp=25.150C
  == 5) ioctl(SENSOR_IOC_GET_STATS) ==         open=1 read=6 irq=12 i2c_err=0 interval=200ms
=== [5] 内核告警数量: 0 ===
=== [6] rmmod OK ===
```

---

## 三、环境架构（为什么用虚拟机 + QEMU）

```
macOS (Apple Silicon)                     ← 你敲代码的地方
├── 交叉编译工具链 aarch64-linux-musl-gcc → 编译 ARM64 用户态程序（真·交叉编译）
├── Lima 虚拟机 (arm64 Ubuntu)             ← Linux 构建环境（编内核/模块）
│     └── QEMU (qemu-system-aarch64)       ← 模拟 ARM64 开发板
│           ├── 自编 Linux 6.6.156 内核 Image
│           ├── 自定义设备树 virt-sensor.dtb
│           └── initramfs（busybox + 我们的 .ko）
└── 项目源码 / 文档 / artifacts
```

**为什么不能在 macOS 直接 insmod？** 内核模块必须加载到 Linux 内核里，macOS 不是 Linux。
**为什么用 QEMU 而不是真板子？** 零成本、秒级重启、GDB 可直连 CPU、没有硬件损坏风险；
真机移植只需改设备树和传感器解析函数（见 `docs/04-真机移植指南.md`）。

---

## 四、目录结构

```
linux-char-driver/
├── README.md                    本文件
├── driver/
│   ├── sensor_char.c            字符设备 + I2C + 中断 + 设备树匹配（核心，~450 行）
│   ├── sensor_ioctl.h           内核/用户态共享的 ioctl 接口定义
│   └── Makefile                 外部模块编译
├── user/
│   ├── sensor_test.c            用户态测试程序（read/ioctl/write）
│   └── sensor_test              已交叉编译的 ARM64 静态可执行文件
├── dts/
│   ├── sensor-char.dtsi         设备树节点说明（含真机写法参考）
│   └── sensor-node.dts.inc      脚本用于插入 QEMU dtb 的纯节点片段
├── scripts/
│   ├── 01-setup-macos.sh        开发机环境（qemu/lima/make/musl 工具链）
│   ├── 02-setup-vm.sh           创建 Linux 虚拟机 + 装依赖
│   ├── 03-build-all-vm.sh       【虚拟机内】一键编内核+驱动+设备树+rootfs
│   ├── 04-run-qemu-vm.sh        【虚拟机内】跑 QEMU（自动演示 / 交互模式）
│   ├── 05-debug-gdb.sh          【虚拟机内】GDB 调试内核与模块演示
│   └── init-demo.sh             initramfs 的 init（开机自动演示全流程）
├── docs/
│   ├── 01-环境搭建与踩坑实录.md   真实踩过的 9 个坑 + 解决办法
│   ├── 02-驱动设计笔记.md         字符设备/I2C/中断/设备树四大模块讲解
│   ├── 03-调试方法手册.md         dmesg/ftrace/GDB 实操 + 真实 WARNING 案例
│   └── 04-真机移植指南.md         换到树莓派 + SHT30 的改动清单
├── artifacts/                  构建产物（内核/设备树/rootfs/驱动模块）
└── kernel-src/linux-6.6.156/   内核源码
```

---

## 五、一天上手路线（8 小时）

| 时段 | 做什么 | 验证标准 |
|------|--------|----------|
| 0–1h | 跑 `scripts/01` + `02`，建好虚拟机；`03` 编出内核 | `artifacts/Image` 生成 |
| 1–2h | 跑 `04` 自动演示，看清 insmod → probe → /dev/sensor0 全链路 | 看到温度数据流 |
| 2–3h | 读 `driver/sensor_char.c` 的字符设备部分，改一个 ioctl 命令试试 | 重编后新命令生效 |
| 3–4h | 读 I2C 部分 + `i2c-tools`（`i2cdetect -y 0`、`i2cget`）手动收发 | 用户态直接读到 0x48 |
| 4–5h | 改 `dts/sensor-node.dts.inc`（如把周期改 1000ms、地址改 0x44），重编 dtb | dmesg 显示新参数 |
| 5–6h | 按 `docs/03` 用 ftrace 观察中断下半部；`echo function_graph` 看调用链 | trace 里看到自己的函数 |
| 6–7h | 按 `docs/05` 用 GDB 断点、单步、查看 `sd->latest` | 断点命中，能打印结构体 |
| 7–8h | 故意制造故障（错地址 / 错 compatible / 少 of_match_table）并自己定位 | 能说出错误现象→原因→修法 |

---

## 六、快速开始（3 条命令）

```bash
# 1) 开发机准备
bash scripts/01-setup-macos.sh && bash scripts/02-setup-vm.sh

# 2) 进虚拟机，一键构建（编内核约 15-30 分钟）
limactl shell dev
bash /Users/$USER/workspace/self-study/projects/linux-char-driver/scripts/03-build-all-vm.sh

# 3) 运行 + 观察
bash /Users/$USER/workspace/self-study/projects/linux-char-driver/scripts/04-run-qemu-vm.sh
# 手动练习（推荐）：INTERACTIVE=1 bash .../04-run-qemu-vm.sh
```

---

## 七、本项目未使用真实硬件

- 实验环境用 `i2c-stub`（内核自带的虚拟 I2C 总线）代替真实传感器，
  驱动里的 I2C 调用路径、中断路径、字符设备路径**与真机完全一致**。
- 唯一区别：真机的 `i2c-1` 换成 `i2c-stub` 的 `i2c-0`，传感器寄存器解析换成 SHT30 公式。
- 移植到树莓派 + SHT30 只需改 3 处，见 `docs/04-真机移植指南.md`。
