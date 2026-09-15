# 阶段 05 实现记录：IIO 子系统驱动（triggered buffer + hrtimer trigger）

> 工作树：`/Users/lucien/workspace/self-study/projects/wt-05`（git worktree，分支 `phase05-iio`，基线 `730d0ff`）
> 最终结果：**05-iio 22/22 通过**、**smoke 回归 15/15 通过**（同一份 initramfs，`-smp 2` 双核）

---

## 一、交付物清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `driver/sensor_iio.c` | 新增（约 380 行） | IIO 驱动：`i2c_driver` + `iio_dev` 两通道 + triggered buffer |
| `driver/Makefile` | 修改 | 增加 `obj-m += sensor_iio.o` |
| `dts/sensor-node.dts.inc` | 修改 | `virt-i2c` 下新增 `sensor1: sensor@49`（`compatible = "lucien,sensor-iio"`） |
| `tests/phases/05-iio.sh` | 新增（22 项检查） | 阶段测试脚本 |
| `tests/phases/05-iio.modules` | 新增 | 本阶段模块清单：`virt_i2c.ko` + `sensor_iio.ko` |
| `user/iio_read_test.c` | 新增 | 用户态：按内核布局算法解析 `/dev/iio:deviceN` 的扫描元素 |
| `scripts/13-vm-fast-cycle.sh` | 修改 | 模块清单改为"运行时按阶段选择"（见第四节 P3） |
| `tests/runner/init.sh` | 修改 | 按 `/etc/modules/<阶段>.load` 选择模块清单 |

**未改动**：`driver/sensor_char.c`（字符设备版保持原样作为对照）、`driver/virt_i2c.c`
（它已经为 0x48~0x4b 各维护独立 bank，0x49 本来就让给 IIO 用）。

---

## 二、IIO 子系统对象模型（本驱动的结构）

```
i2c_driver(sensor_iio)  ──of_match "lucien,sensor-iio"──▶ probe(client)  [从机 0x49]
   │
   ├── devm_regmap_init_i2c()           寄存器访问抽象（reg_bits=8, val_bits=16, LITTLE）
   │
   ├── devm_iio_device_alloc()          → struct iio_dev
   │     ├── .name        = "sensor_iio"          → /sys/bus/iio/devices/iio:device0/name
   │     ├── .channels[]  = { IIO_TEMP, IIO_HUMIDITYRELATIVE, IIO_CHAN_SOFT_TIMESTAMP(2) }
   │     ├── .info.read_raw                        → 单点读取路径
   │     └── .modes       = INDIO_DIRECT_MODE
   │
   ├── devm_iio_triggered_buffer_setup(dev, indio_dev, NULL, handler, NULL)
   │     └── 内部：iio_kfifo_allocate() + iio_alloc_pollfunc() + modes |= INDIO_BUFFER_TRIGGERED
   │        （内核源码 drivers/iio/buffer/industrialio-triggered-buffer.c:81）
   │
   └── devm_iio_device_register()       → 生成 /sys/.../iio:device0/* 与 /dev/iio:device0
```

**触发源**（本驱动不自己产生触发，用内核标准软件触发）：

```
configfs:  mkdir /sys/kernel/config/iio/triggers/hrtimer/inst0
              └─ iio_sw_trigger_create() → iio_trig_hrtimer_probe(name="inst0")
                   └─ iio_trigger_alloc(NULL, "%s", name)   ← 触发器的名字 = configfs 实例名
                        （内核源码 drivers/iio/trigger/iio-trig-hrtimer.c:135）
sysfs:     echo inst0 > /sys/bus/iio/devices/iio:device0/trigger/current_trigger
           echo 1     > /sys/bus/iio/devices/iio:device0/buffer/enable
              └─ 绑定成功后 hrtimer 才会按 sampling_frequency 周期性回调
                 → 驱动的 sensor_iio_trigger_handler()
```

**为什么绑定顺序不能颠倒**：`current_trigger_store()` 在设备已经进入
`INDIO_BUFFER_TRIGGERED` 模式时直接返回 `-EBUSY`
（内核源码 `drivers/iio/industrialio-trigger.c:455`）。
所以顺序必须是：**先绑 trigger → 再配 scan_elements → 最后 `buffer/enable=1`**。
本项目的测试脚本就是按这个顺序写的（中途踩过这个坑，见第四节 P2 相关记录）。

---

## 三、两条取数路径（本阶段的核心对比）

### 3.1 sysfs 单点读取（`read_raw`）

```
用户态: cat /sys/bus/iio/devices/iio:device0/in_temp_raw
  → VFS → iio_read_channel_info() → sensor_iio_read_raw()
  → regmap_read(0x00) → i2c_smbus_read_word_data → virt_i2c.smbus_xfer()
  → data->word = 芯片寄存器值（0x0181 = 385）
```
上下文：进程上下文，**允许睡眠**（I2C 访问会睡眠）。

实测（日志 `logs/20260914-130828-05-05-iio.log`）：

| 属性 | 实测值 | 语义 |
|---|---|---|
| `in_temp_raw` | `385` | 寄存器原始值（LSB = 1/16 ℃ → 385/16 = 24.06 ℃） |
| `in_temp_input` | `24187` | processed，单位毫摄氏度 |
| `in_temp_scale` | `62.500000` | 62.5（毫℃/LSB）= 1/16 ℃，`IIO_VAL_INT_PLUS_MICRO` |
| `in_humidityrelative_raw` | `4002` | 湿度原始值（LSB = 0.01 %RH） |

换算一致性：`raw × scale = 385 × 62.5 = 24062.5 ≈ 24187`（读数时刻不同，芯片值在漂移），
`processed = raw × 1000 / 16` 与 `scale = 62.5` 严格自洽 —— 这正是 IIO 的
raw/scale/processed 三件套约定：用户态自己乘 scale 也能还原物理量。

### 3.2 缓冲区流式读取（triggered buffer）

```
hrtimer(10ms, 100Hz) → iio_trigger_poll() → 线程化 poll func
  → sensor_iio_trigger_handler()          ← 线程上下文，可以发 I2C
      ├─ regmap_read(0x00) → scan.temp
      ├─ regmap_read(0x01) → scan.humidity
      ├─ iio_push_to_buffers_with_timestamp(indio_dev, &scan, iio_get_time_ns())
      └─ iio_trigger_notify_done(indio_dev->trig)
用户态: open("/dev/iio:device0") + read(fd, buf, stride)
```

**为什么 handler 里可以睡眠**：`devm_iio_triggered_buffer_setup(dev, indio_dev, top_half,
thread, ops)` 的第四个参数被注册成 **threaded** 处理函数（`iio_alloc_pollfunc(h, thread, ...)`，
内核源码 `drivers/iio/buffer/industrialio-triggered-buffer.c`）。本项目把 `top_half` 传 `NULL`
（hrtimer 本身已在软中断上下文，没有硬中断上半部要做的事），所有 I2C 访问都放在线程化部分。
这也是"IIO 缓冲路径不能用硬中断上下文"的原因。

**handler 参数是 `iio_poll_func *` 而不是 `iio_dev *`**：6.6 上游驱动的写法是
`struct iio_poll_func *pf = p; struct iio_dev *indio_dev = pf->indio_dev;`
（参考同版本内核 `drivers/iio/humidity/hdc100x.c:315`）。写错会导致空指针解引用。

**实测结果**（同一条日志）：

```
[IIO] layout stride=16 channels=3
[IIO]   chan in_temp               scan_index=0 en=1 type=le:s16/16>>0 offset=0
[IIO]   chan in_humidityrelative   scan_index=1 en=1 type=le:u16/16>>0 offset=2
[IIO]   chan in_timestamp          scan_index=2 en=1 type=le:s64/64>>0 offset=8
[IIO] samples_ok=5 samples_bad=0 timeouts=0
[IIO] dev_reads=5 temp_milli=24187 humidity_milli=40020 OVERALL=PASS
```

即元素布局 = `[temp u16][humidity u16][2B padding][timestamp s64]`，共 16 字节。

---

## 四、踩到的问题与解决（含两个真实缺陷）

### P0：第一次构建编译的是主树代码（工程配置问题）
`scripts/13-vm-fast-cycle.sh` 里 `PROJ=${PROJ:-<主树路径>}` 默认指向主项目目录。
在 worktree 里直接调用脚本，会把**主树**的 `driver/`、`user/`、`tests/` 拷进虚拟机构建
——表现为"新写的 `sensor_iio.c` 没被编译、`05-iio.sh` 找不到、模块清单用的是主树的"。
**修法**：显式传 `PROJ=<worktree>`（`PROJ=$WT limactl shell dev bash -c '... 13-vm-fast-cycle.sh'`）。
**教训**：脚本支持环境变量覆盖时，worktree 场景必须显式传，否则会静默构建另一棵树的代码。

### P1：`scan_elements/*_index` 不是字节偏移（真实解析缺陷）
初版 `user/iio_read_test.c` 直接拿 `in_temp_index / in_humidityrelative_index /
in_timestamp_index` 当字节偏移来算 stride，得到 `stride=10 temp_offset=0 humidity_offset=1
timestamp_offset=2`。这明显不对（16 位通道之间不可能只隔 1 字节），继续按它 `read()` 直接
`-EINVAL`（read 长度必须是扫描元素跨度的整数倍），最终读到 0 个样本。

**根因**：内核 `industrialio-buffer.c:362` 的 index 属性实现是
`sysfs_emit(buf, "%u\n", to_iio_dev_attr(attr)->c->scan_index)` ——
它返回的是**通道的扫描序号**（0/1/2…），不是字节偏移。

**修法**：用户态必须镜像内核的权威布局算法
`iio_compute_scan_bytes()`（`drivers/iio/industrialio-buffer.c:709`）：

```
bytes = 0; largest = 0
for 已启用通道（按 scan_index 升序）:
    len = storagebits / 8
    bytes = ALIGN(bytes, len); offset(chan) = bytes; bytes += len
    largest = max(largest, len)
if 时间戳启用:                       # iio_storage_bytes_for_timestamp() = 时间戳通道存储字节数
    len = 8; bytes = ALIGN(bytes, 8); bytes += len; largest = max(largest, 8)
stride = ALIGN(bytes, largest)
```
`sensor_iio.c` 的通道 `storagebits` 是 16/16/64，于是 offset = 0/2/8、stride = 16，
与驱动侧 `struct { s16 temp; s16 humidity; s64 ts __aligned(8); }` 的内存布局逐一对应。

**顺带产出的强检查**：测试脚本新增断言 `stride == 16`
（`镜像内核布局算法得到的扫描元素跨度`）。这条能直接挡住"把 index 当偏移"这类解析错误
（那种错会算出 stride=10）。

### P2：`sampling_frequency` 不在 configfs 实例目录里（契约文档写错了）
契约（`docs/11-阶段任务书.md` 阶段 05 节）写的是：

```sh
mkdir /sys/kernel/config/iio/triggers/hrtimer/inst0
echo 100 > /sys/kernel/config/iio/triggers/hrtimer/inst0/sampling_frequency   # ← 对 6.6 不成立
```

实测报错：`can't create .../inst0/sampling_frequency: Permission denied`。
**根因**：6.6 里 `sampling_frequency` 是挂在 **trigger 设备** 上的普通 device 属性
（`iio-trig-hrtimer.c:80`：`static DEVICE_ATTR(sampling_frequency, S_IRUGO | S_IWUSR, ...)`），
而 configfs 实例目录的 `config_item_type` 只填了 `.ct_owner`（同文件 `iio_hrtimer_type`），
根本没有该属性 —— 对 configfs 目录里不存在的名字以 `O_CREAT` 打开会得到 `EACCES`（不是 ENOENT），
所以报"Permission denied"具有迷惑性。

**修法**：写到 `/sys/bus/iio/devices/trigger*/sampling_frequency`。
实测：写入 50 后回读 `50.000000`（`IIO_VAL_INT_PLUS_MICRO` 格式）。
**需要集成负责人处理**：`docs/11` 契约里那两行示例命令应改成 trigger 设备路径，
并说明"configfs 只负责实例化 trigger，采样率通过 trigger 设备属性控制"。

### P3：模块清单是"构建期烤进去"的 → smoke 回归假失败（基础设施缺陷，影响所有工作树）
现象：本工作树用 `TEST=05-iio` 构建后，拿同一份 initramfs 跑 `smoke` 得到 **4 PASS / 11 FAIL**
（`/dev/sensor0` 不存在、probe 日志缺失、ftrace 抓不到……），看起来像"阶段 02 的字符设备驱动被改坏了"。

**根因**：阶段脚本的**选择**已经做成运行时可切换（内核命令行 `test=<阶段>`），
但**模块清单**仍是构建期写死的 `/etc/modules.load`：
`05-iio.modules` 只加载 `virt_i2c.ko + sensor_iio.ko`，所以 smoke 需要的 `sensor_char.ko`
根本没被 `insmod`。

**修法**（本工作树内改了两处基础设施，集成负责人合并时需要留意）：

1. `scripts/13-vm-fast-cycle.sh`：把默认清单与**每个阶段的专属清单**都打包到
   `/etc/modules/{default,<阶段>}.load`，并把 `/etc/modules.load` 恒定重置为 `default.load`
   （否则构建时的阶段清单会被当成默认值，问题依旧）；
2. `tests/runner/init.sh`：按 `test=<阶段>` 优先加载 `/etc/modules/<阶段>.load`，
   没有则回退 `/etc/modules.load`；日志里打印实际使用的清单路径，便于自查。

**验证**：同一份 initramfs 依次跑 smoke → 05-iio 全部通过：

```
logs/20260914-130813-05-smoke.log   --- 模块清单: /etc/modules.load        → 15 PASS / 0 FAIL
logs/20260914-130828-05-05-iio.log  --- 模块清单: /etc/modules/05-iio.load → 22 PASS / 0 FAIL
```
这条修复同时消除了主工作树后续做"跨阶段回归"时会踩的同一个坑。

### P4：注释里的 `/*` 触发 `-Wcomment` 警告
`scan_elements/*_index` 写在块注释里会被 gcc 视为嵌套注释起点，编译报
`warning: '/*' within comment [-Wcomment]`。改为"各通道的 index 属性"描述后干净通过。
**DoD 要求"无 warning"，这条属于必须清零的。**

---

## 五、与字符设备版的对比（本阶段要讲清楚的取舍）

| 维度 | `sensor_char.c`（字符设备） | `sensor_iio.c`（IIO 子系统） |
|---|---|---|
| 用户态接口 | 自创 `/dev/sensor0` + 自定义 ioctl | 标准 `/sys/bus/iio/devices/iio:deviceN/*` + `/dev/iio:deviceN` |
| ABI 归属 | 驱动自己定（换驱动就要改用户态） | 内核统一（libiio 等工具直接可用） |
| 单位 | 自己定（本项目用毫摄氏度） | 内核约定 raw/scale/processed + milli 单位体系 |
| 缓冲 | 驱动自己实现（阶段 03 用 kfifo + mmap） | 子系统提供 kfifo 缓冲 + scan 布局 + padding 规则 |
| 触发 | 驱动自己起 hrtimer 并造中断 | 触发与设备解耦：任何 trigger 都能绑到任何设备 |
| 多设备 | 每个驱动各写一套 | 一套框架管所有传感器 |
| 适合场景 | 专用设备、需要私有控制协议（如本项目要演示四种 IO 模型） | 通用传感器（本项目的温度/湿度就是典型） |
| 代价 | 实现自由，但要自己维护 ABI 与工具链 | 要理解 channel/scan 布局/trigger 三种概念，学习曲线更陡 |

**结论**：两类接口各有位置。这个项目的价值在于**两条都实现过**：
字符设备版用来演示内核驱动的底层机制（fops、waitqueue、poll/fasync、kfifo、mmap、中断上下半部），
IIO 版用来证明"我知道对于传感器这类器件，内核期望的形态是什么"。

---

## 六、验证证据（可复现）

### 6.1 复现命令

```bash
WT=/Users/lucien/workspace/self-study/projects/wt-05
# 构建（约 10 秒；注意必须显式传 PROJ=<worktree>，见 P0）
limactl shell dev bash -c "WORKTREE=05 PROJ=$WT TEST=05-iio bash $WT/scripts/13-vm-fast-cycle.sh"
WORKTREE=05 bash $WT/scripts/21-macos-sync-artifacts.sh
bash $WT/scripts/20-macos-gen-dtb.sh                 # 设备树（含 sensor@49）
# 测试（同一份产物可跑任意阶段）
WORKTREE=05 bash $WT/scripts/22-macos-run-test.sh 05-iio 240
WORKTREE=05 bash $WT/scripts/22-macos-run-test.sh smoke 180
```

### 6.2 检查项结论

**05-iio：22 PASS / 0 FAIL**（`logs/20260914-130828-05-05-iio.log`）
关键项：

| 检查项 | 证据 |
|---|---|
| IIO 设备已注册（name=sensor_iio） | `/sys/bus/iio/devices/iio:device0/name` |
| i2c 从设备 0x49 由设备树枚举出来 | `/sys/bus/i2c/devices/0-0049/name = sensor-iio` |
| 0x49 已绑定到 sensor_iio 驱动 | `readlink /sys/bus/i2c/devices/0-0049/driver → /sys/bus/i2c/drivers/sensor_iio` |
| in_temp_raw 落在模拟区间 | 实测 385（24.06 ℃） |
| in_temp_input 在 15000~45000 | 实测 24187 毫℃ |
| in_temp_scale = 62.500000 | 与 1/16 ℃ 一致 |
| configfs 实例化 hrtimer trigger | `/sys/kernel/config/iio/triggers/hrtimer/inst0` |
| trigger 已注册到 IIO（name=inst0） | `/sys/bus/iio/devices/trigger0/name` |
| 可设置采样频率 | 写 50 → 回读 `50.000000` |
| current_trigger 绑定为 inst0 | 回读一致 |
| scan_elements 三项使能 / buffer 开启 | `*_en=1`、`buffer/enable=1` |
| 扫描元素跨度与内核算法一致 | stride=16 |
| 用户态读到缓冲样本 | `samples_ok=5 samples_bad=0`，`OVERALL=PASS` |
| 无内核告警 | `WARNING:/Call trace:` 计数 0（测试体执行后取样） |

**smoke 回归：15 PASS / 0 FAIL**（`logs/20260914-130813-05-smoke.log`）——
阶段 02 的字符设备驱动在本工作树内未被破坏。

驱动侧 probe 链路（日志实测）：

```
virt_i2c virt-i2c: registered i2c adapter i2c-0, adapter.of_node=/virt-i2c (sub-devices enumerated from DT)
sensor_iio 0-0049: chip detected: config=0x0001 (continuous conversion already enabled)
sensor_iio 0-0049: IIO device registered: name=sensor_iio channels=3 (temp + humidity + timestamp)
```

### 6.3 运行环境说明（重要）

本工作树构建时共享内核树正处于另一工作树的 **KCSAN 实验**状态
（`~/kernel-build/linux-6.6.156/.config` 中 `CONFIG_KCSAN=y`，Image 51 MB）。
本工作树的 `Image` 与模块均由该树产出，二者**互相匹配**（故能正常加载运行）。
若集成负责人之后 `restore` 内核配置并重建，需要按 `scripts/13 → 21` 重新生成产物，
否则会出现 `invalid module format`。

---

## 七、遗留风险与未覆盖项

1. **rmmod/insmod 循环未在阶段脚本中断言**：本阶段检查覆盖注册与数据路径，
   没有验证卸载路径（`devm` 资源释放顺序、缓冲/trigger 解绑）。代码使用全 `devm_*`，
   理论上安全，但**缺少实测证据**。建议由验证工作树补一条 `rmmod sensor_iio && insmod` 的检查。
2. **`available_scan_masks` 未设置**：内核允许任意通道组合；本项目只在"三通道全开"下验证过
   布局（stride=16）。只开温度通道时布局会变成 `[temp u16][pad][ts s64]`，用户态程序
   按同一算法仍能算对，但**未实测**。
3. **`scan_type.endianness = IIO_CPU`**：用户态解析里实现了 le/be 分支，但本项目只在
   小端主机上验证过 le 分支。
4. **湿度单位约定**：IIO 惯例 `in_humidityrelative_input` 用毫 %RH，本驱动按
   `scale = 10`（毫%RH/LSB）实现，实测 `humidity_milli=40020`（40.02 %RH）自洽；
   但未与 libiio 工具（`iio_generic_buffer`）交叉验证。
5. **契约文档需更新**：`docs/11` 阶段 05 节的两处示例命令
   （`configfs/.../sampling_frequency` 路径、`driver/modules.load` 的说法）与 6.6 实际行为不符，
   已在第四节 P2 说明，需集成负责人修订契约。
6. **基础设施改动需合并**：本工作树修改了 `scripts/13-vm-fast-cycle.sh` 与
   `tests/runner/init.sh`（P3 的修复）。合并到主树时应确认与主树正在进行的阶段 03/04 改动不冲突。
