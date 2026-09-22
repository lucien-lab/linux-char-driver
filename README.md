# Linux 字符设备驱动开发与 I2C 传感器数据采集

[![CI](https://github.com/lucien-lab/linux-char-driver/actions/workflows/ci.yml/badge.svg)](https://github.com/lucien-lab/linux-char-driver/actions/workflows/ci.yml)
![Platform](https://img.shields.io/badge/platform-ARM64%20%7C%20Linux%206.6.156-blue)
![Kernel](https://img.shields.io/badge/kernel%20module-C%20%2F%20GPL--2.0-informational)
[![License](https://img.shields.io/badge/license-GPL--2.0-green)](LICENSE)

> 个人项目 · 独立开发 · C / Linux Kernel / ARM64
> 2025.12 — 2026.02

在 **macOS (Apple Silicon) 开发机**上搭建一套完整的 Linux 驱动开发环境：
交叉编译 ARM64 内核 → QEMU 运行 ARM64 Linux → 内核字符设备驱动 + I2C 传感器采集 → 设备树解耦 → dmesg/ftrace/GDB 调试。

**整套流程已实测跑通**。构建产物（`artifacts/`）、内核源码（`kernel-src/`）、QEMU 串口日志（`logs/`）体积过大不入库，按「六、快速开始」可一键复现。

---

## 项目亮点

- **5 个内核模块、2769 行 C / 7194 行代码与脚本**：核心 `driver/sensor_char.c`（1676 行）实现字符设备四种 IO 模型、kfifo 环形缓冲 + `mmap` 零拷贝、sysfs/debugfs 参数与统计、Runtime PM；另有自研虚拟 I2C 控制器 `virt_i2c.c`、标准 IIO 驱动 `sensor_iio.c`、KUnit 单元测试模块。
- **七套自动化测试套件、197 项断言全绿**，零内核 WARNING / Oops。实测数据：`01-io-models 16` · `02-i2c-driver 19` · `03-ringbuffer-mmap 40` · `04-sysfs-debugfs 53` · `05-iio 22` · `06-runtime-pm-kunit 32` · `smoke 15`。
- **每个阶段都有独立验证报告 + 负控（变异）实验**：主动向驱动注入 20+ 处缺陷（删掉 `poll_wait()`、去掉 12 位符号扩展、互换 IIO 通道……），确认测试会 FAIL —— 专门用来拆穿「假绿」。见 `docs/verify/`。
- **并发正确性有实测证据**：kfifo + 自实现 seqlock，并用 **KCSAN** 内核数据竞争检测器跑通 `concurrency_test`（8 进程并发读写）。
- **完整链路端到端可复现**：设备树 `compatible` 匹配 → `probe` → 中断注册（自建 `irq_chip`/`irq_domain`）→ `/dev/sensor0` → 线程化中断内读 I2C → 用户态 `read`/`ioctl`；`ftrace` 抓到线程化中断执行轨迹，GDB 断点命中 `i2c_get_adapter`。
- **不依赖真实硬件**：用内核自带 `i2c-stub` + 寄存器级芯片模拟替代传感器，I2C/中断/字符设备代码路径与真机一致，移植到树莓派 + SHT30 只需改 3 处（见 `docs/04-真机移植指南.md`）。

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
linux-char-driver/
├── README.md                       本文件
├── LICENSE                         GPL-2.0（与 MODULE_LICENSE("GPL") 一致）
├── driver/                         内核模块（2769 行 C）
│   ├── sensor_char.c               字符设备 + kfifo/mmap + sysfs/debugfs + PM（1676 行）
│   ├── virt_i2c.c                  虚拟 I2C 控制器：irq_chip/irq_domain + 寄存器级芯片模拟
│   ├── sensor_iio.c                标准 IIO 驱动（trigger / buffer / channel）
│   ├── sensor_kunit.c              KUnit 单元测试（纯逻辑层 sensor_calc.h）
│   ├── sensor_calc.h               定点数转换等纯逻辑，可脱离内核单测
│   ├── sensor_ioctl.h              内核/用户态共享的 ioctl 接口定义
│   ├── modules.load                模块加载顺序
│   └── Makefile                    外部模块编译
├── user/                           用户态测试程序（交叉编译为 ARM64 静态 ELF）
│   ├── sensor_test.c               read / ioctl / write 基本接口
│   ├── io_models_test.c            阻塞 / 非阻塞 / poll·epoll / fasync·SIGIO
│   ├── ring_mmap_test.c            kfifo 环形缓冲 + mmap 零拷贝
│   ├── concurrency_test.c          8 进程并发压力（KCSAN 验证对象）
│   └── iio_read_test.c             IIO 字符设备读取
├── tests/                          自动化测试链路（阶段套件 + runner）
│   ├── phases/                     7 个测试套件：smoke / 01~06
│   ├── runner/                     结果解析库（把串口输出解析成 PASS/FAIL）
│   └── userspace/sensor_stat.c     sysfs/debugfs 状态校验工具
├── dts/
│   ├── sensor-char.dtsi            设备树节点说明（含真机写法参考）
│   └── sensor-node.dts.inc         脚本用于插入 QEMU dtb 的纯节点片段
├── scripts/                        环境搭建 / 构建 / 运行 / 调试 / 门禁脚本
│   ├── 01-setup-macos.sh           开发机环境（qemu / lima / aarch64-musl 工具链）
│   ├── 03-build-all-vm.sh          【虚拟机内】一键编内核 + 驱动 + 设备树 + rootfs
│   ├── 04-run-qemu-vm.sh           【虚拟机内】跑 QEMU（自动演示 / 交互模式）
│   ├── 05-debug-gdb.sh             【虚拟机内】GDB 调试内核与模块
│   ├── 13-vm-fast-cycle.sh         【虚拟机内】改代码→重编→跑测试 快速循环
│   └── 23-check-worktree-clean.sh  合并门禁：扫描交付树是否残留实验代码
└── docs/
    ├── 01-环境搭建与踩坑实录.md     真实踩过的 9 个坑 + 解决办法
    ├── 02-驱动设计笔记.md           字符设备 / I2C / 中断 / 设备树 四大模块讲解
    ├── 03-调试方法手册.md           dmesg / ftrace / GDB 实操 + 真实 WARNING 案例
    ├── 04-真机移植指南.md           换到树莓派 + SHT30 的改动清单
    ├── 10-开发与验证守则.md         工程纪律：什么算「验证通过」
    ├── 11-阶段任务书.md             6 个阶段的接口契约与验收标准
    ├── impl/                       各阶段实现记录
    ├── kb/                         各阶段知识点整理（含内核源码行号索引）
    └── verify/                     各阶段独立验证报告 + 负控实验结果
```

未入库（体积原因，脚本可重新生成）：`artifacts/`（内核 Image / dtb / initramfs / .ko）、`kernel-src/`（内核源码树）、`rootfs/`、`logs/`。

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
bash "$PWD/scripts/03-build-all-vm.sh"

# 3) 运行 + 观察
bash "$PWD/scripts/04-run-qemu-vm.sh"
# 手动练习（推荐）：INTERACTIVE=1 bash "$PWD/scripts/04-run-qemu-vm.sh"
```

---

## 七、本项目未使用真实硬件

- 实验环境用 `i2c-stub`（内核自带的虚拟 I2C 总线）代替真实传感器，
  驱动里的 I2C 调用路径、中断路径、字符设备路径**与真机完全一致**。
- 唯一区别：真机的 `i2c-1` 换成 `i2c-stub` 的 `i2c-0`，传感器寄存器解析换成 SHT30 公式。
- 移植到树莓派 + SHT30 只需改 3 处，见 `docs/04-真机移植指南.md`。

---

## 八、许可

GPL-2.0（见 [LICENSE](LICENSE)）。

内核模块以 GPL 兼容许可发布，才能合法调用内核内部导出符号 —— 各模块源码中的
`MODULE_LICENSE("GPL")` 与本仓库 LICENSE 保持一致。
