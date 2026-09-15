# 阶段 05 知识点：IIO 子系统驱动（triggered buffer + hrtimer software trigger）

> 面向目标：嵌入式 / Linux 驱动岗面试。读完应当能**讲清对象模型、画出两条取数路径的调用链、
> 手算 scan 元素布局、答出"为什么不用自创字符设备"**，并能被追问到"为什么这么设计"。
>
> 本文所有内核源码引用都来自本项目的目标内核树 `~/kernel-build/linux-6.6.156`（Linux 6.6.156），
> 格式为 `文件:行号`（只用只读查询核对过，未修改内核源码树）。
> 本项目代码引用格式为 `文件:行`（相对仓库根 `/Users/lucien/workspace/self-study/projects/wt-05`）。
>
> 行为证据来自本阶段最终 clean 构建的串口日志：
> - `logs/20260914-134111-05-05-iio.log`（阶段测试 **22 PASS / 0 FAIL**，`[TEST:END]` 在 305 行）
> - `logs/20260914-134114-05-smoke.log`（回归 **15 PASS / 0 FAIL**，`[TEST:END]` 在 296 行）
> - `logs/20260914-134059-05-05-iio.log`（含 `rmmod` / `insmod` 卸载路径诊断）
> - `logs/20260914-133853-05-05-iio.log`（含逐通道 `offset` 的布局诊断输出）
>
> 运行环境：宿主 macOS（QEMU `-accel tcg,thread=multi -cpu cortex-a72 -smp 2 -m 1G`），
> 内核 6.6.156 aarch64，普通模式内核（`~/kernel-build/linux-6.6.156/.config` 中无 `CONFIG_KCSAN=y`；
> 最终产物 `artifacts/Image` 40 901 120 字节 = 39 MiB）。上述日志中 `grep -i KCSAN` 命中 0 次。

---

## 0. 一句话心智模型

**IIO 是"传感器类设备"的标准内核子系统：内核替你定义了"一个传感器应该长什么样、
怎么被读、怎么被流式采样、单位是什么"，驱动只负责把寄存器翻译成通道。**

具体说，驱动的全部工作就是三件事：

1. **声明通道**：`struct iio_chan_spec` 数组 —— "我这颗芯片有温度、有湿度，各 16 位，扫描序号 0/1"；
2. **实现取值**：`iio_info.read_raw()` —— "给定通道和要取的量（raw/scale/输入值），去读寄存器"；
3. **装配缓冲**：`devm_iio_triggered_buffer_setup()` —— "允许被触发采样，触发时调用我的 handler"。

其余全部是子系统的：

| 你本来要自己写的 | IIO 替你做了 |
|---|---|
| 设备节点与文件命名 | `/sys/bus/iio/devices/iio:deviceN/{name,in_temp_raw,in_temp_input,in_temp_scale}` |
| 结构体 ABI + ioctl 定义 | sysfs 文本 ABI（内核 `Documentation/ABI/testing/sysfs-bus-iio` 统一规定） |
| 采样缓冲（环形队列） | `iio_kfifo_allocate()` + `/dev/iio:deviceN` 的 read/poll |
| 采样节拍（定时器/中断） | trigger 框架 + `iio-trig-hrtimer`（可 configfs 实例化） |
| 多通道打包成样本（scan 布局 + padding） | `iio_compute_scan_bytes()` |
| 单位换算约定 | raw / scale / offset / processed 与"毫"单位体系 |

一句话对比本项目两个驱动：**`sensor_char.c` 自己定义了 ABI 与会话语义（四种 IO 模型、kfifo、mmap）；
`sensor_iio.c` 把"传感器"这一部分交给标准 ABI，只留下硬件访问。** 两者并存，互不干扰
（字符设备版挂 0x48，IIO 版挂 0x49，见 `dts/sensor-node.dts.inc`）。

---

## 1. 为什么内核要求传感器走 IIO，而不是自创 `/dev`

### 1.1 自创字符设备的三个真实代价

不要泛泛说"标准更好"，面试里要能落到具体代价。以本项目 `driver/sensor_char.c` 为反例：

| 代价 | 具体表现 | 后果 |
|---|---|---|
| **ABI 私有** | 读温度要 `read()`，改采样率要 `ioctl(SET_INTERVAL)`，结构体定义在 `driver/sensor_ioctl.h` | 换一个传感器就要重写用户态；发行版无法为你写工具 |
| **语义私有** | "读到的 `int` 是毫摄氏度还是 raw？字节序？负温怎么表示？"全靠驱动文档 | 同一颗芯片，两个驱动可以给出两套不一致的语义；用户态无法通用 |
| **子系统能力要自己造** | 缓冲要自己写（本项目阶段 03 写了 kfifo + seqlock + mmap）、定时采样要自己起 hrtimer 并造中断、多通道打包要自己定 | 每个驱动重复造一遍，且各有无穷的小 bug（本项目阶段 01 就踩过漏写 `poll_wait()`） |

例外情况是**允许的**，但要能说清边界：私有控制协议（设备有厂商专有命令）、
非传感器语义的字符设备、需要用户态直接控制硬件时序的设备（例如本项目要演示四种 IO 模型、
epoll 语义、fasync、mmap 共享内存），自写 fops 是合理选择。IIO 的目标是"通用传感器"，
不是"所有设备"。

### 1.2 子系统替你做的事（逐条对应代码）

| 子系统能力 | 内核实现位置 | 本项目体现 |
|---|---|---|
| 通道枚举与命名 | `iio_chan_info_postfix[]`：`RAW→"raw"`、`PROCESSED→"input"`、`SCALE→"scale"`（`drivers/iio/industrialio-core.c:149-153`） | 驱动只写 `IIO_TEMP` + `info_mask_separate`，就得到 `in_temp_raw` / `in_temp_input` / `in_temp_scale` |
| 单点读的 sysfs 通路 | `iio_read_channel_info()`（`drivers/iio/industrialio-core.c:740-764`）把 `read_raw()` 的返回码格式化成文本 | `in_temp_scale` 读到 `62.500000`（`iio_format_value()` 按 `IIO_VAL_INT_PLUS_MICRO` 格式化） |
| 缓冲区 | `iio_kfifo_allocate()`（`drivers/iio/buffer/kfifo_buf.c`），字符设备 fops 在 `drivers/iio/industrialio-core.c:1847-1858` | `/dev/iio:device0` 的 `read()/poll()` 开箱可用 |
| 触发框架 | `iio_trigger` + `iio_alloc_pollfunc()` + `request_threaded_irq()`（`drivers/iio/industrialio-trigger.c:371-398,307`） | 驱动不注册定时器/中断，只提供一个 handler |
| 多通道成帧（scan 布局） | `iio_compute_scan_bytes()`（`drivers/iio/industrialio-buffer.c:709-733`） | 用户态不用问驱动"温度在哪几个字节"，但**必须自己按同一算法算**（第 5 节） |
| 单位约定 | raw / scale / processed 三件套 + processed 一律"毫"单位 | `in_temp_input=24187`（毫℃）、`in_temp_scale=62.500000`（毫℃/LSB） |

### 1.3 统一 ABI 带来的用户态工具链

走 IIO 的直接收益是**别人写的工具能直接用**：

- 内核源码树自带 `tools/iio/iio_generic_buffer.c`（同时提供 `lsiio`、`iio_event_monitor`）。
  它的 `size_from_channelarray()`（`tools/iio/iio_generic_buffer.c:54-80`）就是用户态版的
  scan 布局算法（第 5 节会逐行对照）；它读通道列表用的也是 `scan_elements/*` 目录。
  **结论**：本项目 `user/iio_read_test.c` 做的事与内核自带工具是同一件事，
  ABI 统一意味着"我的解析逻辑可以换成官方工具再验一遍"（本项目**未做**这一步交叉验证，见第 8 节）。
- 用户空间库 **libiio**（`iio_info`、`iio_readdev` 等）只认这套 sysfs + `/dev/iio:deviceN` 布局，
  与具体驱动无关。
- ABI 的稳定性由内核维护（`Documentation/ABI/testing/sysfs-bus-iio`），
  私有 ioctl 则要你自己维护到永远。

### 1.4 与 hwmon / input / 字符设备的分工

面试常问"温度传感器为什么不写 hwmon"。边界如下（按**数据的消费者**选，而不是按芯片选）：

| 子系统 | 面向的消费者 | 典型场景 | 为什么不用它 |
|---|---|---|---|
| **hwmon** | 系统监控（`sensors`、`lm-sensors`、thermal 框架） | 板载温度/电压/风扇转速，量少、速率低、只需"当前值" | 不支持多通道成帧、触发采样、缓冲流；单位与命名固定为"监控"语义；无法表达"同步采 3 个通道" |
| **input** | 人机输入栈（`/dev/input/eventX`、evdev） | 加速度计/陀螺仪做屏幕旋转、lid 开关 | 语义是"事件流"，不表达物理量与单位；不适合高频采样与批量搬运 |
| **IIO** | 测量/采样应用 | 温度、湿度、压力、IMU、ADC/DAC（含高速同步采样） | —— 它是为"工业测量"设计的：通道 + 单位 + 触发 + 缓冲 |
| **自写字符设备** | 私有协议/教学/需要自定义 IO 语义 | 本项目 `sensor_char.c` | 见 1.1 的三个代价 |

一句话答法：**"hwmon 是给系统监控读一个数、input 是给人机输入送事件，它们都不表达
'物理量 + 单位 + 多通道同步 + 采样节拍'；IIO 才是测量语义的子系统。我的这颗温度/湿度传感器
正是 IIO 的目标设备。"**

边界上还有一个真实细节值得主动说：温度传感器的**数据**走 IIO，但如果它同时是
SoC 的 thermal zone，那么 thermal 框架可以通过 IIO consumer API 去读（
`iio_channel_get()` + `iio_read_channel_processed()`），两条路不冲突。

---

## 2. IIO 对象模型

### 2.1 `struct iio_dev`：一个设备

定义在 `include/linux/iio/iio.h:577`。驱动侧真正关心这些字段：

| 字段 | 位置 | 本项目赋值 | 作用 |
|---|---|---|---|
| `name` | `include/linux/iio/iio.h:595` | `"sensor_iio"` | 出现在 `/sys/bus/iio/devices/iio:deviceN/name` |
| `info` | `include/linux/iio/iio.h:597` | `&sensor_iio_info` | 取值回调集合（`.read_raw` 等） |
| `channels` / `num_channels` | `include/linux/iio/iio.h:592-593` | 3 个通道 | 通道枚举的唯一来源 |
| `modes` | `include/linux/iio/iio.h:578` | `INDIO_DIRECT_MODE`，随后被 setup 追加 `INDIO_BUFFER_TRIGGERED` | 决定"能不能开缓冲、要不要 trigger" |
| `masklength` | `include/linux/iio/iio.h:585` | 内核按通道数算 | `active_scan_mask` 的位宽 |
| `scan_bytes` | `include/linux/iio/iio.h:582` | 由 `iio_compute_scan_bytes()` 填 | 一个扫描元素的字节数 |
| `trig` | `include/linux/iio/iio.h:588` | 由 `current_trigger` 写入 | 当前绑定的 trigger |
| `scan_timestamp` | `include/linux/iio/iio.h:587` | 由 `scan_elements/in_timestamp_en` 写入 | 本次扫描是否带时间戳 |

（`struct iio_dev` 定义在 `include/linux/iio/iio.h:577-601`。）

**`iio_dev` 是"管理结构"，不是"数据"。** 驱动自己的状态用 `iio_priv(indio_dev)` 取：
`devm_iio_device_alloc(dev, sizeof(*st))`（`driver/sensor_iio.c:266`）一次性分配
"`iio_dev` 不透明区 + 私有区"，`iio_priv()`（`driver/sensor_iio.c:270`）返回私有区指针。
这是 IIO 的惯用法，好处是**私有数据与设备生命周期绑定、且不需要额外的指针解引用**。

### 2.2 `struct iio_chan_spec`：一个通道

定义在 `include/linux/iio/iio.h:241`。本项目通道（`driver/sensor_iio.c:174-209`）：

```c
static const struct iio_chan_spec sensor_iio_channels[] = {
	{
		.type = IIO_TEMP,
		.address = SENSOR_REG_TEMP,          /* 驱动自用：寄存器地址 */
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = 0,                     /* 扫描序号，不是字节偏移！ */
		.scan_type = {
			.sign = 's', .realbits = 16,
			.storagebits = 16, .endianness = IIO_CPU,
		},
	},
	/* ... IIO_HUMIDITYRELATIVE, scan_index = 1 ... */
	IIO_CHAN_SOFT_TIMESTAMP(2),              /* 时间戳通道，关键字：必须紧跟数据通道 */
};
```

三个字段族，面试要分清：

1. **`type` + `info_mask_*`** → **决定 sysfs 文件名**。`type=IIO_TEMP` 给出前缀 `in_temp_`；
   `BIT(IIO_CHAN_INFO_RAW)` 给出后缀 `_raw`（后缀表见 `drivers/iio/industrialio-core.c:149-153`）。
   所以 `in_temp_raw` 这个名字**不是驱动起的**，是子系统推导出来的。
   `_separate` 表示"按通道独立"，`_shared_by_type/_by_dir/_by_all` 表示"同类通道共享属性文件"。
2. **`address`** → **驱动自用**，内核只原样透传给你的 `read_raw()`（
   `drivers/iio/industrialio-core.c:751-756` 把 `this_attr->address` 传下去）。本项目放寄存器号。
3. **`scan_index` + `scan_type`** → **决定"打包进缓冲时的顺序与位宽"**。
   `scan_index` 是序号（`include/linux/iio/iio.h:246`），`scan_type` 描述元素位宽与符号
   （`include/linux/iio/iio.h:254`）。第 5 节专讲这块。

`IIO_CHAN_SOFT_TIMESTAMP(_si)` 展开为 `{ .type = IIO_TIMESTAMP, .channel = -1,
.scan_index = _si, .scan_type = { .sign='s', .realbits=64, .storagebits=64 } }`
（`include/linux/iio/iio.h:309-317`）——**注意它没有 `.endianness`，默认 0 = `IIO_CPU`**。

### 2.3 `iio_info.read_raw()`：取值回调

`include/linux/iio/iio.h:457` 声明。签名与返回码是面试必问：

```c
static int sensor_iio_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask)   /* driver/sensor_iio.c:131 */
```

- `mask` 是"要哪个量"：`IIO_CHAN_INFO_RAW` / `PROCESSED` / `SCALE`（还有 `OFFSET`、
  `SAMP_FREQ`、`CALIBBIAS`…）。
- `*val` / `*val2` 输出数值，**返回码决定怎么解释这两个数**：

| 返回码 | 语义 | 本项目使用 |
|---|---|---|
| `IIO_VAL_INT` | 数值 = `*val` | `in_temp_raw=385`、`in_temp_input` 走 `IIO_VAL_INT` 返回 24187 |
| `IIO_VAL_INT_PLUS_MICRO` | 数值 = `*val + *val2/1000000` | `in_temp_scale`：`*val=62, *val2=500000` → 打印 `62.500000` |
| `IIO_VAL_INT_PLUS_NANO` / `_MICRO_DB` / `IIO_VAL_FRACTIONAL` … | 其它精度/比率 | 本项目未用 |

- **三个量的关系（必须能推）**：
  `processed = (raw + offset) * scale`，其中 `scale` 的单位是"物理量 / LSB"。
  本项目温度：LSB = 1/16 ℃ = 62.5 毫℃ → `scale = 62.5 毫℃`，
  `processed(毫℃) = raw * 1000 / 16`（`driver/sensor_iio.c:107-110`，先转 `s16` 再乘，
  否则负温会算错）。

  > 面试追问 "为什么 `processed` 用毫摄氏度而不是摄氏度"：
  > 整数 sysfs 不能带小数，IIO 约定用"毫"级整数（`_input` 类属性）。
  > 我用 `IIO_VAL_INT` 直接给毫摄氏度，用户态零精度损失；
  > 也有的驱动让 `_input` 返回 `IIO_VAL_INT_PLUS_MICRO`，两者都合法，但**必须与 `scale` 自洽**。

- **内核内消费者**（另一个驱动想读你的传感器）走的是另一条路：
  `iio_channel_get()` + `iio_read_channel_processed()`，内部会调
  `iio_convert_raw_to_processed()`（`drivers/iio/inkern.c:711-730`）替你算 `raw*scale+offset`。
  也就是说：**`scale` 不是给人看的装饰，它是内核换算的输入**。

### 2.4 `iio_trigger`：谁在"打拍子"

`struct iio_trigger`（`include/linux/iio/trigger.h:60`）代表一个**触发源**，与设备解耦：

- 触发源可以是硬件的 DRDY 引脚中断，也可以是软件定时器（hrtimer）、sysfs 手动触发、其它设备。
- 设备侧只需要"被通知"：触发发生时，IIO 会把该 trigger 上的所有 consumer（设备）的
  poll func 拉起来（`iio_trigger_poll()`，`drivers/iio/industrialio-trigger.c:201-216`）。
- 一个 trigger 能挂几个 consumer 由 `trig->subirqs[CONFIG_IIO_CONSUMERS_PER_TRIGGER]`
  数组大小决定（`include/linux/iio/trigger.h:74`）。本项目内核
  `CONFIG_IIO_CONSUMERS_PER_TRIGGER=2`，即**最多 2 个设备共享同一个 trigger**，
  超过会打印 `Could not find an available irq for trigger ...`
  （`drivers/iio/industrialio-trigger.c:301-302`）。

### 2.5 `iio_buffer` 与 triggered buffer 的关系

这是最容易讲混的地方。**缓冲（buffer）负责"存样本"，触发（trigger）负责"什么时候采"，
两者独立，通过 poll func 连接。**

```text
struct iio_buffer（include/linux/iio/buffer_impl.h:81）
  ├─ bytes_per_datum   ← 由 iio_compute_scan_bytes() 算出的 stride（buffer_impl.h:89）
  ├─ length            ← 样本条数，sysfs buffer/length（buffer_impl.h:83）
  ├─ scan_mask         ← 哪些通道要进缓冲，sysfs scan_elements/*_en（buffer_impl.h:101）
  ├─ scan_timestamp    ← 是否带时间戳，sysfs scan_elements/in_timestamp_en（buffer_impl.h:114）
  ├─ access            ← 后端函数表：本项目是 kfifo（kfifo_buf.c:194-201）
  └─ pollq             ← 供 read()/poll() 等待（buffer_impl.h:107）
```

装配 triggered buffer 的那一行（`driver/sensor_iio.c:312-313`）：

```c
ret = devm_iio_triggered_buffer_setup(dev, indio_dev, NULL,
				      sensor_iio_trigger_handler, NULL);
```

它内部做三件事（`drivers/iio/buffer/industrialio-triggered-buffer.c:39-98`）：

1. `iio_kfifo_allocate()` —— 分配 kfifo 缓冲（`industrialio-triggered-buffer.c:58`）；
2. `iio_alloc_pollfunc(h, thread, IRQF_ONESHOT, ...)` —— 申请 poll func
   （`industrialio-triggered-buffer.c:65-72`），把 `h` 当上半部、`thread` 当下半部；
3. `indio_dev->modes |= INDIO_BUFFER_TRIGGERED`（`industrialio-triggered-buffer.c:81`）
   —— **这一位决定了 `/dev/iio:deviceN` 与 `buffer/` 目录会不会出现**。

`modes` 没这一位会怎样：验证报告的负控 C 把这一行注释掉后，`buffer/enable`、
`scan_elements/*_en`、`current_trigger`、用户态读样本一共 10 项全红
（`logs/20260914-133754-05-05-iio.log`，`[TEST:END]` 312 行 `pass=12 fail=10`）。
换句话说，**`devm_iio_triggered_buffer_setup()` 是"从单点读设备升级成流式设备"的开关**。

`devm_` 版本只是把清理动作注册成 devres（`industrialio-triggered-buffer.c:117-135`），
所以本驱动 `remove` 里什么都不用拆（`driver/sensor_iio.c:336-339` 只打一条日志）。
实测卸载路径正常：`sensor_iio 0-0049: removed` → `lsmod` 计数归 0 → 重新 `insmod` 后
再次 `IIO device registered`（`logs/20260914-134059-05-05-iio.log:303-314`）。

### 2.6 `scan_elements` 与 `current_trigger`：两个 sysfs 目录

它们都挂在 `/sys/bus/iio/devices/iio:deviceN/` 下：

```text
/sys/bus/iio/devices/iio:device0/
├── name                    "sensor_iio"
├── in_temp_raw             385        ← read_raw(RAW)      [单点路径 1]
├── in_temp_input           24187      ← read_raw(PROCESSED)[单点路径 2]
├── in_temp_scale           62.500000  ← read_raw(SCALE)    [单点路径 3]
├── in_humidityrelative_raw 4002
├── trigger/
│   └── current_trigger     "inst0"    ← 写即绑定 trigger（顺序有约束，见 4.3）
├── scan_elements/
│   ├── in_temp_en / in_temp_index / in_temp_type
│   ├── in_humidityrelative_en / _index / _type
│   └── in_timestamp_en / in_timestamp_index / in_timestamp_type
└── buffer/
    ├── enable   0/1    ← 开关流式采样（必须最后写）
    ├── length   64     ← kfifo 能存多少条样本
    └── watermark
```

`scan_elements/*_en` 的写入路径是 `iio_scan_el_store()`（`drivers/iio/industrialio-buffer.c:507-544`）：
它**在 buffer 已激活时直接返回 `-EBUSY`**（`industrialio-buffer.c:523-526`），
所以"先配 scan_elements、最后 enable"是内核强制的顺序。
`in_timestamp_en` 走的是另一个函数 `iio_scan_el_ts_store()`（`industrialio-buffer.c:555-580`），
它改的是 `buffer->scan_timestamp`。

`*_index` / `*_type` 是只读属性，分别由 `iio_show_scan_index()`
（`industrialio-buffer.c:358-362`）和 `iio_show_fixed_type()`（`industrialio-buffer.c:365-394`）生成。
本项目实测（**来源是用户程序的解析结果，不是单独的 `cat` 留证**）：`user/iio_read_test.c`
的 `parse_type()` 按格式串 `%3[^:]:%c%d/%d>>%d` 成功解析出 `le:s16/16>>0`，
随后的日志回显了同一串（`logs/20260914-133853-05-05-iio.log:300-302`）：

```text
scan_elements/in_temp_type   = le:s16/16>>0     （= 格式 %s:%c%d/%d>>%d：端序/符号/realbits/storagebits/shift）
scan_elements/in_temp_index  = 0                （= scan_index，不是字节偏移）
scan_elements/in_humidityrelative_type = le:u16/16>>0
scan_elements/in_timestamp_type        = le:s64/64>>0
```

### 2.7 本项目的完整对象图

```text
设备树 sensor@49 (compatible="lucien,sensor-iio")   dts/sensor-node.dts.inc
   │  i2c 核心枚举 ──▶ i2c_client
   ▼
sensor_iio_probe()                                  driver/sensor_iio.c:255
   ├─ devm_iio_device_alloc(dev, sizeof(struct sensor_iio))   :266  → iio_dev + 私有区
   │     └─ devm_regmap_init_i2c()                  :273  寄存器访问（reg_bits=8, val_bits=16, LITTLE）
   ├─ 填充 indio_dev->{name,info,modes,channels,num_channels} :298-302
   ├─ devm_iio_triggered_buffer_setup(dev, indio_dev, NULL, handler, NULL)  :312
   │     └─ kfifo 缓冲 + pollfunc(thread=handler) + modes |= INDIO_BUFFER_TRIGGERED
   └─ devm_iio_device_register(dev, indio_dev)      :319
         └─ 生成 /sys/bus/iio/devices/iio:device0/* 与 /dev/iio:device0

独立存在的触发源（不在驱动的 probe 里）：
configfs mkdir /sys/kernel/config/iio/triggers/hrtimer/inst0
   → iio_sw_trigger_create("hrtimer", "inst0")      drivers/iio/industrialio-sw-trigger.c:100
      → iio_trig_hrtimer_probe("inst0")              drivers/iio/trigger/iio-trig-hrtimer.c:129
         → iio_trigger_alloc(NULL, "%s", name)       iio-trig-hrtimer.c:138   ← 名字 = 实例名 inst0
         → iio_trigger_register()                    iio-trig-hrtimer.c:154
运行时绑定： echo inst0 > /sys/.../iio:device0/trigger/current_trigger
            echo 1     > /sys/.../iio:device0/buffer/enable
```

---

## 3. 两条取数路径

### 3.1 路径 A：sysfs 单点读（`read_raw`）

适用场景：**低频、按需、只想要"当前一个值"**。比如用户态要显示温度、或 thermal 框架读一次。

```text
用户态   cat /sys/bus/iio/devices/iio:device0/in_temp_raw
  │
VFS      sysfs 属性读：kernfs → dev_attr_show()                     fs/kernfs/file.c（属性 show 回调）
  │
IIO core iio_read_channel_info()                                   drivers/iio/industrialio-core.c:740
  │        indio_dev->info->read_raw(indio_dev, chan, &vals[0], &vals[1], mask)   :751-756
  │        iio_format_value(buf, ret, val_len, vals)                :763
  │        （返回码 → 文本：IIO_VAL_INT / IIO_VAL_INT_PLUS_MICRO …）
  │
driver   sensor_iio_read_raw()                                     driver/sensor_iio.c:131
  │        case IIO_CHAN_INFO_RAW:                                  :139
  │
driver   sensor_iio_read_reg()  →  mutex_lock + regmap_read + mutex_unlock   driver/sensor_iio.c:113-125
  │
regmap   regmap_read() → regmap_smbus_word_read() → i2c_smbus_read_word_data()
  │
i2c core i2c_smbus_xfer() → … → virt_i2c.smbus_xfer()
  │
virt_i2c  bank[0x49].regs[0x00]（每次读温度寄存器都重算一次"转换"）
  ▼
用户态看到 "385\n"
```

**上下文与睡眠**：整条链在**进程上下文**（sysfs 文件读写由发起系统调用的进程执行），
`read_raw()` **允许睡眠** —— `regmap_read()` 会走 I2C，可能睡眠。
项目驱动在 `sensor_iio_read_reg()` 里用 `mutex_lock()` 保护 regmap
（`driver/sensor_iio.c:118-120`），这是安全的；**如果谁在中断上下文里调它，就是 `BUG`**。

### 3.2 路径 B：`/dev/iio:deviceN` 流式读（triggered buffer）

适用场景：**高频、连续、要样本序列与时间戳**。比如同时采温度+湿度再算相关性、
上报告警、做 FFT。

```text
[产生侧]  hrtimer 到点（HRTIMER_MODE_REL_HARD）
  iio_hrtimer_trig_handler()                                        drivers/iio/trigger/iio-trig-hrtimer.c:98-108
  │        hrtimer_forward_now(timer, period)                       :104
  │        iio_trigger_poll(info->swt.trigger)                      :105
  │
  iio_trigger_poll()（硬中断上下文）                                 drivers/iio/industrialio-trigger.c:201-216
  │        对 triggers 上每个 enabled 的 subirq：generic_handle_irq()  :210
  │
  request_threaded_irq 的 primary handler（本项目 h=NULL → 默认）     industrialio-trigger.c:307
  │        → 唤醒内核线程执行 threaded handler
  │
  sensor_iio_trigger_handler()   ← ★线程上下文，可睡眠★              driver/sensor_iio.c:220
  │        test_bit(0, indio_dev->active_scan_mask)                  :238
  │        sensor_iio_read_reg(TEMP)      → I2C（会睡眠）             :239
  │        sensor_iio_read_reg(HUMIDITY)                              :243
  │        iio_push_to_buffers_with_timestamp(indio_dev, &scan, ns)  :247
  │        iio_trigger_notify_done(indio_dev->trig)                  :251
  │
  iio_push_to_buffers_with_timestamp()（inline）                     include/linux/iio/buffer.h:37-48
  │        if (indio_dev->scan_timestamp)                            :40
  │            ((int64_t *)data)[scan_bytes/8 - 1] = timestamp;      :41   ← 时间戳写在元素末尾
  │        iio_push_to_buffers() → 遍历 buffer_list → iio_push_to_buffer()  industrialio-buffer.c:1867
  │            → kfifo_in(&kf->kf, data, 1)                          kfifo_buf.c:90-99
  │        → wake_up_interruptible_poll(&buffer->pollq, EPOLLIN)    :1858（iio_push_to_buffer() 内）
  ▼
[消费侧]
用户态   read(fd, buf, stride)
  │
VFS      iio_buffer_fileops.read = iio_buffer_read_outer_addr       industrialio-core.c:1847-1858
  │        （CONFIG_IIO_BUFFER 下 = iio_buffer_read_wrapper）        drivers/iio/iio_core.h:78
  │
IIO core iio_buffer_read_wrapper()  →  iio_buffer_read()            industrialio-buffer.c:106-170
  │        datum_size = rb->bytes_per_datum;                        :126   ← 就是 stride
  │        iio_buffer_ready() / wait_woken() 等数据                  :147-155
  │        rb->access->read(rb, n, buf)                             :158
  │
kfifo    iio_read_kfifo()                                          drivers/iio/buffer/kfifo_buf.c:100-119
  │        if (!kfifo_initialized || n < kfifo_esize) return -EINVAL;  :110 ← 本阶段踩坑点（第 5.4 节）
  │        kfifo_to_user()                                          :112
  ▼
用户态拿到 stride 字节的 1 个（或多个）扫描元素
```

两个必须记住的细节：

1. **`/dev/iio:deviceN` 里的 `deviceN` 与 sysfs 里的编号一致**，两者由同一个 `iio_dev` 实例产生；
   本项目实测 `IIO 设备目录: /sys/bus/iio/devices/iio:device0`、`字符设备: /dev/iio:device0`
   （`logs/20260914-134111-05-05-iio.log:267-268`）。但**不要写死编号**，要按 `name` 找
   （测试脚本就是这么做的：`tests/phases/05-iio.sh:17-27`）。
2. **`open()` 是独占的**：`iio_chrdev_open()` 用 `test_and_set_bit(IIO_BUSY_BIT_POS, ...)`
   拒绝第二次打开（`drivers/iio/industrialio-core.c:1753-1761`）。所以"两个进程同时读同一个
   iio:device"是做不到的，第二个 open 得到 `-EBUSY`。
   > 对比：本项目字符设备版 `sensor_char.c` 允许 per-open 并发（阶段 03 的 8 进程测试）。
   > 这是两套接口的设计差异，面试时可以主动说。

### 3.3 为什么流式读"必须有 trigger"

**因为缓冲区里没有"发动机"。** kfifo 只是一块内存，`read()` 只会从里面取；
样本从哪来？只有 trigger 到点 → poll func 被调用 → 你的 handler → `iio_push_to_buffers*()`。
没有 trigger，kfifo 永远空，`read()` 会一直等到 `-ERESTARTSYS` 或超时。

内核在代码里就是这么约束的（`drivers/iio/industrialio-buffer.c:872-892`）：

```c
	/* Definitely possible for devices to support both of these. */
	if ((modes & INDIO_BUFFER_TRIGGERED) && indio_dev->trig) {
		config->mode = INDIO_BUFFER_TRIGGERED;
	} else if (modes & INDIO_BUFFER_HARDWARE) {
		...
	} else if (modes & INDIO_BUFFER_SOFTWARE) {
		config->mode = INDIO_BUFFER_SOFTWARE;
	} else {
		/* Can only occur on first buffer */
		if (indio_dev->modes & INDIO_BUFFER_TRIGGERED)
			dev_dbg(&indio_dev->dev, "Buffer not started: no trigger\n");
		return -EINVAL;
	}
```

本驱动的 `modes` 只有 `INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED`，
而 kfifo 后端声明自己支持 `INDIO_BUFFER_SOFTWARE | INDIO_BUFFER_TRIGGERED`
（`drivers/iio/buffer/kfifo_buf.c:199`），按位与之后只剩 `INDIO_BUFFER_TRIGGERED`。
所以**不绑 trigger 就 `echo 1 > buffer/enable` 会走 `else` 分支返回 `-EINVAL`**
（`industrialio-buffer.c:887-892`）。
> 这条是**读代码核对**的结论；本项目的测试脚本总是先绑 trigger 再开 buffer，
> 因此**未在 QEMU 里实测**这个 `-EINVAL`（第 8 节列为未验证项）。

### 3.4 `trigger_handler` 的执行上下文：能不能睡眠

**能。但前提是它被注册成"线程化"处理函数 —— 本项目正是这么做的。**

`devm_iio_triggered_buffer_setup(dev, indio_dev, NULL, handler, NULL)` 的第 3 个参数是
"上半部"，第 4 个是"线程化下半部"（`include/linux/iio/triggered_buffer.h:33` 的宏展开）；
内核把它注册为 `iio_alloc_pollfunc(h, thread, IRQF_ONESHOT, ...)`
（`industrialio-triggered-buffer.c:65-72`），最终用
`request_threaded_irq(pf->irq, pf->h, pf->thread, pf->type, pf->name, pf)`
（`drivers/iio/industrialio-trigger.c:307`）注册。于是：

| 部分 | 参数 | 上下文 | 能否睡眠 | 本项目 |
|---|---|---|---|---|
| 上半部（primary） | 第 3 个入参 `h` | 硬中断上下文 | ❌ 不能 | 传 `NULL`（hrtimer 自己就在硬中断上下文里触发的，没有额外上半部工作） |
| 下半部（threaded） | 第 4 个入参 `thread` | **内核线程（进程上下文）** | ✅ 能 | `sensor_iio_trigger_handler`，里面直接发 I2C |

这就是**"IIO 缓冲路径 + 真实硬件访问"的标准写法**：把 I2C/SPI 读寄存器放在 threaded handler。
如果错误地把 I2C 访问放进上半部，会在硬中断上下文里试图睡眠，触发
`BUG: scheduling while atomic`（本项目未实测，属已知内核事实）。

三个补充事实：

1. **触发源本身在"硬"上下文**：hrtimer 用 `HRTIMER_MODE_REL_HARD` 启动
   （`drivers/iio/trigger/iio-trig-hrtimer.c:117-118`），回调 `iio_hrtimer_trig_handler()`
   在硬中断上下文跑；`iio_trigger_poll()` 的文档也明确 "should only be called from a hard
   IRQ context"（`drivers/iio/industrialio-trigger.c:197-201`）。所以**上半部仍然不能睡眠**，
   只是本项目恰好不需要上半部。
2. **handler 的参数是 `struct iio_poll_func *` 而不是 `iio_dev *`**：
   `struct iio_poll_func *pf = p; struct iio_dev *indio_dev = pf->indio_dev;`
   （`driver/sensor_iio.c:222-223`）。写错会空指针。上游同版本写法见
   `drivers/iio/humidity/hdc100x.c:314-315`。
3. **handler 结束必须 `iio_trigger_notify_done(indio_dev->trig)`**
   （`driver/sensor_iio.c:251`）。它递减 refcount，归零后才会调 trigger 的 `reenable`
   回调（`drivers/iio/industrialio-trigger.c:249-255`）。漏掉它，trigger 只投递一次
   （本项目字符设备版阶段 01 踩过同构的坑：漏写 `poll_wait()` 导致 epoll 首唤醒 2503ms）。

### 3.5 两条路径对照

| 维度 | sysfs 单点读 | `/dev/iio:deviceN` 流式读 |
|---|---|---|
| 触发方式 | 用户态发起（`cat`） | trigger 周期驱动 |
| 数据量 | 每次 1 个值 | 每条样本 = stride 字节（本项目 16 B） |
| 时间戳 | 无 | 有（`in_timestamp_en=1` 时） |
| 上下文 | 进程上下文 | 产生侧：线程化 IRQ；消费侧：进程上下文 |
| 并发 | 多进程可同时 `cat`（只读） | `open()` 独占 |
| 延迟 | = I2C 一次传输 | = trigger 周期（本项目可设 50 Hz）+ 线程调度 |
| 适用 | 显示当前值、thermal 框架、调试 | 连续采样、多通道同步、批量搬运 |
| 内核路径 | `iio_read_channel_info()` → `read_raw()` | kfifo → `iio_buffer_read()` → `kfifo_to_user()` |
| 本项目证据 | `in_temp_raw=385`、`in_temp_input=24187` | `dev_reads=5 ... OVERALL=PASS` |

---

## 4. hrtimer software trigger

### 4.1 software trigger 框架与 configfs 实例化

IIO 把"软件触发的种类"注册成一个框架（`drivers/iio/industrialio-sw-trigger.c`）：

```text
内核初始化（built-in 时随内核启动，模块时随 insmod）
  iio_sw_trigger_init()                                   industrialio-sw-trigger.c:165-172
    └─ configfs_register_default_group(iio_configfs_subsys.su_group, "triggers", ...)   :168
         → 目录 /sys/kernel/config/iio/triggers/
  iio_trig_hrtimer 注册为一种 type（名字 "hrtimer"）
    module_iio_sw_trigger_driver(iio_trig_hrtimer)        iio-trig-hrtimer.c:195
         → 目录 /sys/kernel/config/iio/triggers/hrtimer/

用户态 mkdir 一个实例
  mkdir /sys/kernel/config/iio/triggers/hrtimer/inst0
    └─ trigger_make_group(group="hrtimer", name="inst0")  industrialio-sw-trigger.c:132-146
         └─ iio_sw_trigger_create("hrtimer", "inst0")      industrialio-sw-trigger.c:100-121
              └─ iio_trig_hrtimer_probe("inst0")           iio-trig-hrtimer.c:129-166
                   ├─ iio_trigger_alloc(NULL, "%s", "inst0")     iio-trig-hrtimer.c:138
                   ├─ trigger->ops = &iio_hrtimer_trigger_ops     :145（只有 set_trigger_state）
                   ├─ hrtimer_init(CLOCK_MONOTONIC, REL_HARD)     :148
                   ├─ 默认采样率 100 Hz                            :151-152
                   └─ iio_trigger_register()                      :154
         → 出现 /sys/bus/iio/devices/triggerN（name = 实例名 "inst0"）

绑定与使能
  echo inst0 > /sys/.../iio:device0/trigger/current_trigger   ← 只登记 indio_dev->trig
  echo 1     > /sys/.../iio:device0/buffer/enable             ← 这一步才真正启动 hrtimer
       → iio_enable_buffers() 里 currentmode==INDIO_BUFFER_TRIGGERED
         → iio_trigger_attach_poll_func()                    industrialio-trigger.c:287-336
              ├─ iio_trigger_get_irq()（占一个 subirq 槽）       :299（定义在 :258-268）
              ├─ request_threaded_irq()                        :307
              └─ trig->ops->set_trigger_state(trig, true)      :314-315
                   └─ iio_trig_hrtimer_set_state()  hrtimer_start(period, REL_HARD)
                                                             iio-trig-hrtimer.c:110-122
```

**为什么 `set_trigger_state` 只在真正需要时才启动定时器**：`iio_trigger_attach_poll_func()`
里用 `notinuse` 判断"我是第一个使用者"，只有第一个 consumer 绑定时才启动；
`iio_trigger_detach_poll_func()` 里用 `no_other_users` 判断"我是最后一个"，才停止
（`drivers/iio/industrialio-trigger.c:342-348`）。这就是"一个 trigger 被多个设备共享时
定时器不会互相干扰"的实现方式。

### 4.2 `sampling_frequency` 在哪：一个真实踩坑

**它在 trigger 设备上，不在 configfs 实例目录里。**

```c
static DEVICE_ATTR(sampling_frequency, S_IRUGO | S_IWUSR,
		   iio_hrtimer_show_sampling_frequency,
		   iio_hrtimer_store_sampling_frequency);
```
（`drivers/iio/trigger/iio-trig-hrtimer.c:80-82`，属性组在 `:84-96`，
通过 `trigger->dev.groups = iio_hrtimer_attr_groups` 挂到 trigger 设备，`:145`）

而 configfs 实例目录的类型只填了 `.ct_owner`：

```c
static const struct config_item_type iio_hrtimer_type = {
	.ct_owner = THIS_MODULE,
};
```
（`iio-trig-hrtimer.c:32-34`）

所以：

| 路径 | 结果 |
|---|---|
| `/sys/kernel/config/iio/triggers/hrtimer/inst0/sampling_frequency` | ❌ 写失败，`Permission denied`（对 configfs 目录里不存在的名字以 `O_CREAT` 打开得到 `EACCES`，不是 `ENOENT`，所以报错很有迷惑性） |
| `/sys/bus/iio/devices/trigger0/sampling_frequency` | ✅ 实测写 `50` 回读 `50.000000` |

本项目实测（`logs/20260914-134111-05-05-iio.log:286-287`）：

```text
      ·   sampling_frequency 写入 50 后回读: 50.000000
[CHECK:PASS] 可通过 trigger 设备设置采样频率（实测 50.000000）
```

解析与存储：`iio_hrtimer_store_sampling_frequency()` 用 `iio_str_to_fixpoint(buf, 100, ...)`
解析成 Hz + 小数（`iio-trig-hrtimer.c:50-78`），换算成 `period = PSEC_PER_SEC / (mHz)`
存 nS 给 `hrtimer_start()` 用（`:66-75`）；读回时用 `iio_format_value(IIO_VAL_INT_PLUS_MICRO)`
（`:37-47`），所以打印成 `50.000000` 而不是 `50`。默认值是 100 Hz（`:151-152`）。

> `docs/11-阶段任务书.md` 阶段 05 节的示例写的是往 configfs 路径写 `sampling_frequency`，
> 与 6.6 的实际行为不符（验证报告第六节已记录，属契约文档错误，实现与测试用的是正确路径）。

### 4.3 trigger 与设备的绑定规则

四条规则，都要能说：

1. **顺序不可颠倒：先绑 trigger，再开 buffer。**
   `current_trigger_store()` 在设备已经进入 `INDIO_BUFFER_TRIGGERED` 时直接返回 `-EBUSY`：
   ```c
   	mutex_lock(&iio_dev_opaque->mlock);
   	if (iio_dev_opaque->currentmode == INDIO_BUFFER_TRIGGERED) {
   		mutex_unlock(&iio_dev_opaque->mlock);
   		return -EBUSY;
   	}
   ```
   （`drivers/iio/industrialio-trigger.c:444,455-458`）
   `currentmode` 只有在 buffer 真正 enable 之后才是 `INDIO_BUFFER_TRIGGERED`
   （`industrialio-buffer.c:1064-1074`）。所以运行时改 trigger 必须先 `buffer/enable=0`。
   同理 `scan_elements/*_en` 在 buffer 激活时写也是 `-EBUSY`
   （`industrialio-buffer.c:523-526`）。
   **正确顺序：`trigger/current_trigger` → `scan_elements/*_en` → `buffer/length` → `buffer/enable`。**

2. **绑定靠"名字"**：`current_trigger_store()` 用 `iio_trigger_acquire_by_name(buf)` 查全局
   trigger 列表（`industrialio-trigger.c:465`），名字就是 configfs 实例名。
   实测 `/sys/bus/iio/devices/trigger0/name` = `inst0`
   （`logs/20260914-134111-05-05-iio.log:284-285`）。
   > 契约文档原来写"`trigger*/name` 出现 `hrtimer`"，实际是实例名 `inst0` —— 因为
   > `iio_trigger_alloc(NULL, "%s", name)` 用的是 configfs 实例名（`iio-trig-hrtimer.c:138`）。

3. **可以双向校验**：绑定时内核依次调用
   `indio_dev->info->validate_trigger()`（设备自校验，本项目未实现）和
   `trig->ops->validate_device()`（trigger 自校验）（`industrialio-trigger.c:471-481`）。
   IIO 提供了现成的"只能绑自己的设备"的校验函数 `iio_validate_own_trigger()`
   （`industrialio-trigger.c:743-749`，比较 `idev->dev.parent != trig->dev.parent`），
   用于"片上内置 trigger"这类不能跨设备共享的场景。本项目用的是通用 hrtimer trigger，
   两个都不设，因此**任何设备都能绑它** —— 这正是软件 trigger 的价值。
4. **共享上限 = `CONFIG_IIO_CONSUMERS_PER_TRIGGER`**（本项目内核值为 **2**）：
   每次 attach 从 `trig->pool` 位图里取一个 subirq（`industrialio-trigger.c:261-274`），
   取不到就报错并返回失败（`:299-303`）。也就是说这个 trigger 最多同时服务 2 个 IIO 设备。
   驱动侧不需要做任何协调 —— **参考实现是"设备绑定自己的 trigger"，共享是可选能力**。

---

## 5. scan 元素布局（本阶段踩坑重点）

这一节是全文最该背下来的部分，因为它决定"用户态能不能正确解析缓冲"。

### 5.1 `scan_elements/*_index` 不是字节偏移

内核里那个属性的实现只有一行：

```c
static ssize_t iio_show_scan_index(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", to_iio_dev_attr(attr)->c->scan_index);
}
```
（`drivers/iio/industrialio-buffer.c:358-362`）

**它返回的是 `chan->scan_index`，即"这个通道在扫描序列里排第几"（0、1、2…）。**
内核用它来：
- 匹配 `iio_find_channel_from_si()`（`drivers/iio/industrialio-core.c:238-246`）；
- 索引 `active_scan_mask` 的第 N 位（本项目 `driver/sensor_iio.c:238,242` 的
  `test_bit(0/1, indio_dev->active_scan_mask)`）；
- 排序（用户态按它排序，保证解析顺序与驱动填充顺序一致）。

它**从来不是**"这个通道的数据在元素里的第几个字节"。字节偏移必须按下面的算法算。

### 5.2 `iio_compute_scan_bytes()`：布局的权威算法

```c
static int iio_compute_scan_bytes(struct iio_dev *indio_dev,
				  const unsigned long *mask, bool timestamp)
{
	unsigned int bytes = 0;
	int length, i, largest = 0;

	/* How much space will the demuxed element take? */
	for_each_set_bit(i, mask, indio_dev->masklength) {
		length = iio_storage_bytes_for_si(indio_dev, i);
		bytes = ALIGN(bytes, length);
		bytes += length;
		largest = max(largest, length);
	}

	if (timestamp) {
		length = iio_storage_bytes_for_timestamp(indio_dev);
		bytes = ALIGN(bytes, length);
		bytes += length;
		largest = max(largest, length);
	}

	bytes = ALIGN(bytes, largest);
	return bytes;
}
```
（`drivers/iio/industrialio-buffer.c:709-733`）

逐行解释：

- **`for_each_set_bit(i, mask, masklength)`**：按 `scan_index` **升序**遍历"已启用"的通道
  （位图天然从低位到高位）。所以**驱动填充样本的顺序必须与 scan_index 升序一致**。
- **`iio_storage_bytes_for_si()`**：取该通道的存储字节数
  （`industrialio-buffer.c:688-697`）：
  ```c
  	ch = iio_find_channel_from_si(indio_dev, scan_index);
  	bytes = ch->scan_type.storagebits / 8;
  	if (ch->scan_type.repeat > 1)
  		bytes *= ch->scan_type.repeat;
  ```
  本项目全部 `repeat = 1`（未设置），所以就是 `storagebits/8`：温度 2、湿度 2、时间戳 8。
  **注意：用的是 `storagebits`，不是 `realbits`。** 例如一个 12 位 ADC 用 16 位存储，
  占位是 2 字节，`realbits=12` 只影响 `shift`/符号扩展。
- **`bytes = ALIGN(bytes, length); bytes += length;`**：**该通道的起始偏移 = 把当前累计长度
  向上对齐到它自己的存储宽度**。这一步就是 padding 的第一个来源。
- **`largest = max(largest, length)`**：记录最大元素宽度，用于最后整体对齐。
- **时间戳分支**：`iio_storage_bytes_for_timestamp()` 取时间戳通道的存储字节数
  （`industrialio-buffer.c:701-706`，本项目 = 8），做 8 字节对齐后追加。**时间戳永远在最后。**
- **`bytes = ALIGN(bytes, largest)`**：**整个元素再对齐到最大元素宽度**，
  目的是"让下一个样本的开头也保持对齐"。这是 padding 的第二个来源。
  注意 `iio_push_to_buffers_with_timestamp()` 正是靠"时间戳在末尾"这一不变量取值的：
  ```c
  		size_t ts_offset = indio_dev->scan_bytes / sizeof(int64_t) - 1;
  ```
  （`include/linux/iio/buffer.h:40-42`）—— 它不查驱动结构体的成员偏移，而是**用
  `scan_bytes - 8` 算时间戳位置**。

### 5.3 本项目的布局与 padding 的三个来源

通道：温度 `storagebits=16`（`scan_index=0`）、湿度 `storagebits=16`（`scan_index=1`）、
时间戳 `storagebits=64`（`scan_index=2`，`IIO_CHAN_SOFT_TIMESTAMP(2)`）。

手算：

```text
bytes = 0, largest = 0
[scan_index 0] temp     len = 16/8 = 2
    bytes = ALIGN(0, 2) = 0         → offset(temp) = 0
    bytes = 0 + 2 = 2 ;  largest = 2
[scan_index 1] humidity len = 16/8 = 2
    bytes = ALIGN(2, 2) = 2         → offset(humidity) = 2
    bytes = 2 + 2 = 4 ;  largest = 2
[时间戳]                len = 64/8 = 8
    bytes = ALIGN(4, 8) = 8         → offset(timestamp) = 8      ← padding 2 个字节（offset 4~7）
    bytes = 8 + 8 = 16 ; largest = 8
stride = ALIGN(16, 8) = 16
```

**最终布局（16 字节，本阶段实测 stride=16）：**

| 字节 | 内容 |
|---|---|
| 0–1 | 温度，`le:s16` |
| 2–3 | 湿度，`le:u16` |
| 4–7 | **padding（未初始化/无意义）** |
| 8–15 | 时间戳，`le:s64` |

与驱动侧结构体逐一对应（`driver/sensor_iio.c:226-234`）：

```c
	struct {
		s16 temp;
		s16 humidity;
		s64 ts __aligned(8);
	} scan;
```
`sizeof(scan) = 16`，成员偏移 0 / 2 / 8（4~7 是编译器插入的填充）。
**`__aligned(8)` 不是装饰：没有它，`s64 ts` 在奇偶不确定的偏移上会变成 12 或 4，
与内核算出的布局（固定 8）不一致，`iio_push_to_buffers_with_timestamp()` 会把时间戳
写到 `offset 8`，而驱动结构体的 `ts` 在别处 —— 时间戳错位、且可能踩到 padding。**

**padding 的三个来源，逐条对应：**

| 来源 | 内核代码 | 本项目是否出现 | 若出现会是多少 |
|---|---|---|---|
| ① 通道自身对齐：`ALIGN(bytes, len)` | `industrialio-buffer.c:718` | ✅ 出现（4→8，给时间戳让位） | 2 字节（offset 4~7） |
| ② 时间戳前的 8 字节对齐 | `industrialio-buffer.c:725` | 同上（与 ① 是同一处生效） | —— |
| ③ 末尾整体对齐：`ALIGN(bytes, largest)` | `industrialio-buffer.c:730` | ❌ 本项目 16 已是 8 的倍数 | 0 字节 |

**再举两个例子（同一算法不同启用组合）**，用来证明"布局随启用集合变化，不能写死"（均为手工推导，本项目**未实测**）：

| 启用集合 | 计算 | stride | 备注 |
|---|---|---|---|
| temp + humidity + timestamp（本项目实测） | 0,2,(4→8),8..15 | **16** | ✅ 日志实测 `stride=16` |
| 只 temp + timestamp | 0,(2→8),8..15 | 16 | `largest` 被时间戳抬到 8，stride 仍是 16 |
| temp + humidity（不启用时间戳） | 0,2 | **4** | stride 变成 4 |
| 只 humidity + timestamp | 0,(2→8),8..15 | 16 | 湿度跑到 offset 0 |

### 5.4 为什么用户态必须镜像内核布局：本项目初版真实案例

**初版 `user/iio_read_test.c` 直接拿 `*_index` 当字节偏移：**

```text
错误算法（把 *_index 直接当字节偏移）：
  offset(temp)      = scan_index = 0
  offset(humidity)  = scan_index = 1
  offset(timestamp) = scan_index = 2
  stride            = timestamp_offset + 8 = 10
  → 得到 stride=10 / temp_offset=0 / humidity_offset=1 / timestamp_offset=2
```

这个结果**自相矛盾却很容易被忽略**：16 位通道之间不可能只隔 1 字节。
按它去 `read(fd, buf, 10)`，内核直接返回 `-EINVAL`：

```c
static int iio_read_kfifo(struct iio_buffer *r, size_t n, char __user *buf)
{
	...
	if (!kfifo_initialized(&kf->kf) || n < kfifo_esize(&kf->kf))
		ret = -EINVAL;
	else
		ret = kfifo_to_user(&kf->kf, buf, n, &copied);
```
（`drivers/iio/buffer/kfifo_buf.c:100-112`；`kfifo_esize()` 就是 `bytes_per_datum` = 16）

**故障链完整表述**：`*_index` 当偏移 → 算出 `stride = 10` → `10 < kfifo_esize(16)`
→ `read()` 返回 `-EINVAL` → 用户程序读到 0 个样本 → `OVERALL=FAIL`。

这条因果链在本阶段被**负控实验 D 复现并抓住了**：把 `compute_layout()` 改回
"把 `_index` 当字节偏移"，测试脚本断言 `stride=16` 实测到 **10**，共 4 项失败
（`logs/20260914-133820-05-05-iio.log:296,298,299,300`，`[TEST:END]` 305 行 `pass=18 fail=4`）。
这正是 `tests/phases/05-iio.sh:150-153` 那条断言存在的意义：

```busybox
# 布局自检：temp(16bit)+humidity(16bit)+timestamp(64bit, 8 字节对齐) → stride 应为 16
# 这条能挡住"用户态按 =_index 当偏移"这类解析错误（那种错会算出 stride=10 或读到错位数据）
check_eq "扫描元素跨度与内核布局算法一致（stride=16 字节）" "16" "${STRIDE:-}"
```

**顺带说清"read 长度与 stride 的关系"**（面试常问）：
`iio_read_kfifo()` 只挡 `n < esize`；当 `n > esize` 时进入 `kfifo_to_user()`，
底层把字节数换算成元素个数时会**向下取整**：

```c
int __kfifo_to_user(struct __kfifo *fifo, void __user *to,
		unsigned long len, unsigned int *copied)
{
	unsigned int esize = fifo->esize;
	...
	if (esize != 1)
		len /= esize;          /* ← 只搬整数个元素 */
	l = fifo->in - fifo->out;
	if (len > l)
		len = l;
	...
```
（`lib/kfifo.c:270-279`）

所以"读的长度不是元素跨度的整数倍"会以两种方式暴露：
- `n < stride`：直接 `-EINVAL`（本项目踩到的就是这种）；
- `n > stride` 且不是整数倍：返回的字节数**少于请求**，用户程序必须检查
  `n == stride`（`user/iio_read_test.c` 里 `if (n != stride) → bad++`）。

**用户态的正确做法（镜像内核算法）**：本项目 `compute_layout()`（`user/iio_read_test.c`）
逐行实现 5.2 的算法，字段来源全部是 sysfs：

| 算法输入 | 来源 |
|---|---|
| 通道列表与启用状态 | 枚举 `scan_elements/` 下所有 `*_en`，读值 |
| 排序键 | `scan_elements/<chan>_index` |
| 元素宽度 `storagebits`、符号、端序 | `scan_elements/<chan>_type`（格式 `le:s16/16>>0`） |
| 时间戳特判 | `storagebits == 64 && realbits == 64` → 最后处理并做 8 字节对齐 |

> **交叉证据（重要）**：内核源码树自带的用户态工具也是这么做的 ——
> `tools/iio/iio_generic_buffer.c:54-80` 的 `size_from_channelarray()` 是同一个算法
> （用 `max` 表示 `largest`，末尾 `misalignment = bytes % max` 补齐），
> 通道序号来自 `tools/iio/iio_utils.c:437-460` 读的 `*_index`，
> 随后用 `bsort_channel_array_by_index()`（`tools/iio/iio_utils.c:292-306`）**排序**。
> "读 `_index` 只用来排序，不用来当偏移"是上游的既定用法。

### 5.5 用户态解析 checklist（可直接背）

1. `opendir(<iio:deviceN>/scan_elements)`，收集所有 `*_en` 通道；
2. 读 `*_en`（0/1，判断是否参与布局）、`*_index`（排序键）、`*_type`（宽度/符号/端序）；
3. 按 `_index` 升序排序；
4. 跑一遍 `iio_compute_scan_bytes()` 的算法得到每个通道的 `byte_offset` 与 `stride`；
5. `open("/dev/iio:deviceN")`，`poll()` 等可读，`read(fd, buf, stride)`，
   **检查返回值等于 stride**；
6. 按 `byte_offset` + `storagebits` + 端序 + 符号取出整数，再做符号扩展与 `>> shift`；
7. 边界情况：`buffer/enable` 关闭时 `read()` 会阻塞（无数据）；元素里 padding 字节是
   未初始化的垃圾，**不要读**。

> 本项目的 `user/iio_read_test.c` 在打印布局时有个**已知缺陷**（验证报告 S2）：
> `compute_layout()` 的 `have_ts` 分支只累加 `bytes`、没有回填 `ch[i].byte_offset`，
> 于是日志里 `in_timestamp ... offset=0`（实际应为 8）。
> 这只影响日志可读性，**不参与解析**（时间戳不参与温度取值），
> `stride=16` 与温度/湿度解析都正确 —— 但看日志时要知道这一条是打印缺陷，
> 以 `logs/20260914-133853-05-05-iio.log:302` 的实测输出为准。

---

## 6. 面试问答

### Q1. IIO 驱动怎么写？最小骨架有哪些必需对象？

答：五步。

1. **总线驱动**：`i2c_driver`（或 `spi_driver`/`platform_driver`），
   `.of_match_table` 匹配设备树 `compatible`。本项目 `driver/sensor_iio.c:342-352`
   （`id_table` 匹配 `"sensor_iio"`、`of_match_table` 匹配 `"lucien,sensor-iio"`），
   注册在 `:354-363`（`module_i2c_driver`）。
2. **probe 里分配 `iio_dev`**：`devm_iio_device_alloc(dev, sizeof(*priv))` + `iio_priv()`
   （`driver/sensor_iio.c:266,270`）。
3. **写通道表** `iio_chan_spec[]`：`type` + `info_mask_*`（决定 sysfs 名字）+
   `address`（驱动自用）+ `scan_index`/`scan_type`（进缓冲用）。本项目 3 个通道
   （`driver/sensor_iio.c:174-209`）。
4. **写 `iio_info.read_raw()`**：处理 `RAW`/`PROCESSED`/`SCALE`（`driver/sensor_iio.c:131-168`）。
5. **装配 buffer + 注册**：`devm_iio_triggered_buffer_setup()` 然后
   `devm_iio_device_register()`（`driver/sensor_iio.c:312,319`）。
   全程用 `devm_*`，`remove()` 里只打日志 —— 卸载顺序由 devres 逆序保证
   （实测 `rmmod`/`insmod` 干净，`logs/20260914-134059-05-05-iio.log:303-314`）。

追问"为什么要 `num_channels` 而不是靠 `channels` 结尾的空元素"：
`iio_chan_spec` 数组**必须显式给长度**，因为通道很多时逐个探测 `scan_index < 0` 很脆弱；
上游惯用 `ARRAY_SIZE()`（`driver/sensor_iio.c:302`）。

### Q2. trigger 与 buffer 是什么关系？为什么不 enable buffer 就 read 不到？

答：**buffer 是容器，trigger 是节拍器，poll func 是连接件。**
`read()` 只从 kfifo 里取，kfifo 的填充只能由 `iio_push_to_buffers*()` 完成，
而它只会在你的 poll func（本项目 `sensor_iio_trigger_handler`）里被调用，
poll func 只有在 buffer enable 时才会被 attach 到 trigger 上
（`industrialio-buffer.c:1112-1114`）。所以没 enable 就没有填充源。
另外不绑 trigger：`modes` 里只剩 `INDIO_BUFFER_TRIGGERED` 而 `indio_dev->trig == NULL`
→ `iio_verify_update()` 返回 `-EINVAL`（`industrialio-buffer.c:872-892`）。
**顺序：trigger → scan_elements → buffer/enable。**

### Q3. `read_raw` 能不能睡眠？`trigger_handler` 能不能睡眠？

答：**`read_raw` 能睡**（进程上下文，sysfs 读路径，本项目在里面 `mutex_lock` + I2C）。
**`trigger_handler` 能睡 —— 前提是它被注册为 threaded handler**（本项目第 4 个参数，
`driver/sensor_iio.c:312-313`，内核用 `request_threaded_irq` 注册，
`industrialio-trigger.c:307`）。但**上半部不能睡**：hrtimer 以 `HRTIMER_MODE_REL_HARD`
运行在硬中断上下文（`iio-trig-hrtimer.c:117`），`iio_trigger_poll()` 也要求硬中断上下文
（`industrialio-trigger.c:197-201`）。本项目把上半部传 `NULL`，所以不存在"上半部做重活"的风险。
追问"handler 里能睡，那为什么还要分上下半部"：因为**触发侧（硬中断）必须尽快返回**，
睡眠的动作被推迟到内核线程；这也让 `CONFIG_PREEMPT` 下的响应抖动可控。

### Q4. `scan_elements` 的 padding 是怎么来的？

答：三个来源，全部在 `iio_compute_scan_bytes()`（`industrialio-buffer.c:709-733`）里产生：
①每通道按自身存储宽度对齐 `ALIGN(bytes, length)`；②时间戳按 8 字节对齐；
③末尾按"最大元素宽度"整体对齐 `ALIGN(bytes, largest)`。
本项目 `[temp u16][humidity u16][pad 2B][ts s64]`，stride 16
（`logs/20260914-134111-05-05-iio.log:297-298` 实测 `stride=16`）。
追问"padding 内容是什么"：驱动结构体的空洞，**未初始化**；
本项目 `scan` 未 `memset`，所以 padding 是栈上的残留值（验证报告 S5 建议清零）。
用户态不得解释 padding。

### Q5. `scan_elements/<chan>_index` 是字节偏移吗？用户态怎么知道每个通道在哪？

答：**不是。** 它是 `chan->scan_index`，是扫描序号，内核的 index 属性就一行
`sysfs_emit(buf, "%u\n", ...->c->scan_index)`（`industrialio-buffer.c:358-362`）。
字节偏移必须由用户态**镜像内核的 `iio_compute_scan_bytes()`** 算出来（第 5.2/5.4 节）。
本项目初版把 index 当偏移 → `stride=10` → `read()` 返回 `-EINVAL`
（`kfifo_buf.c:110` 的 `n < kfifo_esize`），0 个样本；负控 D 复现了该失败
（`logs/20260914-133820-05-05-iio.log`，`stride` 实测 10）。
上游工具同样是"用 index 排序、不用它当偏移"（`tools/iio/iio_generic_buffer.c:54-80`、
`tools/iio/iio_utils.c:292-306`）。

### Q6. 为什么不用字符设备暴露传感器？hwmon/input 呢？

答：三点：**统一 ABI**（sysfs 命名 + `/dev/iio:deviceN` 布局由内核规定，
libiio / `iio_generic_buffer` 直接可用）；**子系统替你实现**（通道枚举、缓冲、触发、
scan 布局、单位约定）；**分工正确**（hwmon 面向系统监控只读当前值、input 面向人机事件，
它们都不表达"物理量 + 多通道同步 + 采样节拍"）。
反过来说：**当设备语义不是"传感器测量"时，自写字符设备是对的**
——本项目 `sensor_char.c` 就用来演示四种 IO 模型、epoll、fasync、mmap 这些
IIO 不提供的会话语义。**"选子系统"是选语义，不是选"更高级"。**

### Q7. raw / scale / processed 三者的关系？为什么 processed 用"毫"单位？

答：`processed = (raw + offset) * scale`，`scale` 的单位是"物理量 / LSB"。
本项目温度 LSB = 1/16 ℃ = 62.5 毫℃，`scale` 用 `IIO_VAL_INT_PLUS_MICRO` 返回
`62 + 500000/1e6`（打印 `62.500000`），`processed` 用 `IIO_VAL_INT` 返回毫摄氏度
（`385 * 1000/16 = 24062`，`driver/sensor_iio.c:107-110,146-154`）。
`set` 里三者自洽，用户态乘 `scale`、内核用 `iio_convert_raw_to_processed()`
（`drivers/iio/inkern.c:711-730`）都能还原物理量。
**用"毫"是因为 sysfs 是文本整数**，IIO 约定 `*_input` 类属性用毫级整数；
用 `IIO_VAL_INT_PLUS_MICRO` 也可以，但要保证与 `scale` 一致。
追问"负温怎么办"：`raw` 按 `s16` 解释再换算，本项目
`sensor_iio_raw_to_milli()` 先 `(s16)raw` 再乘（`driver/sensor_iio.c:107-110`），
这是字符设备版踩过字节序坑后的既定做法。

### Q8. 为什么必须先绑 trigger 再开 buffer？反过来会怎样？

答：`current_trigger_store()` 在 `currentmode == INDIO_BUFFER_TRIGGERED` 时返回 `-EBUSY`
（`industrialio-trigger.c:444,455-458`），`currentmode` 由 enable 设置
（`industrialio-buffer.c:1064-1074`）。同理 `scan_elements/*_en` 在 buffer 激活时也是 `-EBUSY`
（`industrialio-buffer.c:523-526`）、`buffer/length` 也不能改。
所以运行中要换 trigger 必须 `echo 0 > buffer/enable`。
**这解释了为什么"写完 trigger 立刻 echo 1 > buffer/enable"是对的，
而"先开 buffer 再绑 trigger"会失败。**

### Q9. 一个 trigger 能绑几个设备？hrtimer 的采样率在哪设？为什么不在 configfs 目录？

答：最多 `CONFIG_IIO_CONSUMERS_PER_TRIGGER` 个（本项目内核 = **2**），
由 `trig->subirqs[]`/`trig->pool` 位图限制（`include/linux/iio/trigger.h:74-75`，
取不到槽位会报错返回失败，`industrialio-trigger.c:301-302`）。
采样率在 **trigger 设备**上：`/sys/bus/iio/devices/triggerN/sampling_frequency`，
因为它是 `DEVICE_ATTR(sampling_frequency, ...)` 挂在 `trigger->dev.groups`
（`iio-trig-hrtimer.c:80-96,145`），而 configfs 实例目录的
`config_item_type` 只填了 `.ct_owner`（`iio-trig-hrtimer.c:32-34`）——
往 configfs 目录写会得到 `EACCES`。configfs 只负责"实例化/销毁 trigger"。
实测写 50 回读 `50.000000`（`logs/20260914-134111-05-05-iio.log:286-287`），
默认 100 Hz（`iio-trig-hrtimer.c:151-152`）。

### Q10. `iio_push_to_buffers_with_timestamp()` 的时间戳放在哪？驱动结构体为什么 `__aligned(8)`？

答：时间戳放在**扫描元素的最后 8 字节**，位置由 `scan_bytes/8 - 1` 算
（`include/linux/iio/buffer.h:40-42`）——**内核不看驱动结构体的成员偏移**。
所以驱动结构体必须与内核算出的布局逐字节一致：`s16 temp; s16 humidity; s64 ts __aligned(8);`
→ 偏移 0/2/8（4~7 是填充），而内核算的也是 0/2/8、stride 16
（`industrialio-buffer.c:709-733`）。`__aligned(8)` 保证 `ts` 落在 8；
否则时间戳会被写到内核认为的 offset 8、而结构体里的 `ts` 在别处，
出现"时间戳错位/padding 被覆盖"。

### Q11. `buffer/length` 默认是多少？为什么要先关 buffer 才能改？

答：本项目实测默认很小（**2** 条，`tests/phases/05-iio.sh:137` 的注释记录了这一点），
测试里改成 **64** 并回读确认（`logs/20260914-134111-05-05-iio.log:293`）。
`length` 走 `iio_set_length_kfifo()`，最小值被抬到 2（`kfifo_buf.c:80-88`），
改长度会置 `update_needed`，下次 enable 时重建 kfifo（`kfifo_buf.c:42-60`）。
**必须在 enable=0 时改**，因为 buffer 激活时属性写入路径整体被锁住/被拒
（同 Q8 的 `-EBUSY` 机制，`iio_scan_el_store()` 那一类检查）。
默认 2 条的问题很实际：**流式读来不及取就丢样本**（kfifo 满时
`iio_store_to_kfifo()` 返回 `-EBUSY`，`kfifo_buf.c:90-99`）。

### Q12. 为什么你的 IIO 驱动用 `i2c_driver` + `regmap`？

答："IIO"是**功能子系统**，"i2c/spi/platform"是**总线** —— 两者是正交的，
IIO 驱动通常就是"某个总线驱动 + 在上面注册 iio_dev"。本项目 `probe` 参数是
`struct i2c_client *`（`driver/sensor_iio.c:255`），注册用 `module_i2c_driver()`
（`:363`）。`regmap` 让"寄存器读写"与"总线传输"解耦
（`reg_bits=8, val_bits=16, max_register=0x02`，`driver/sensor_iio.c:99-103`），
还顺带解决字节序问题：**`val_format_endian` 不写就默认 BIG，SMBus word 会被
`swab16()` 弄反**（本项目字符设备版踩过：25 ℃ 读成 2000+ ℃）。
本项目用 `REGMAP_ENDIAN_LITTLE`。证据：`in_temp_raw=385`（= 24.06 ℃ 量级）在合理区间
（`logs/20260914-134111-05-05-iio.log:275-276`）。

### Q13. `/dev/iio:deviceN` 能同时被两个进程读吗？

答：**不能。** `iio_chrdev_open()` 用 `test_and_set_bit(IIO_BUSY_BIT_POS, ...)`
拒绝第二次 open，返回 `-EBUSY`（`drivers/iio/industrialio-core.c:1753-1761`）。
四舍五入等于"一个消费者"。要在多进程间共享，标准做法是单进程读再分发（IPC），
或者用 IIO 的 `dmabuf`/`IIO_BUFFER_CB` 等新接口（**本项目未涉及，未验证**）。
对比：本项目字符设备版 `sensor_char.c` 支持 per-open 并发（阶段 03 用 8 进程 200 轮验证过）
—— 这是"自写 fops"的自由度之一。

---

## 7. 亲手验证

### 7.1 一键复现（与 `docs/10-开发与验证守则.md` 一致）

```bash
WT=/Users/lucien/workspace/self-study/projects/wt-05

# ① 设备树（含 virt-i2c 下的 sensor@48 / sensor@49）
bash $WT/scripts/20-macos-gen-dtb.sh

# ② 构建模块 + 用户态程序 + initramfs（VM 内，约 10 秒；本工作树固定 WORKTREE=05 → ~/lab-05）
limactl shell dev bash -c "WORKTREE=05 TEST=05-iio bash $WT/scripts/13-vm-fast-cycle.sh"

# ③ 产物拷回宿主
WORKTREE=05 bash $WT/scripts/21-macos-sync-artifacts.sh

# ④ 阶段测试 + 回归（QEMU，TCG / cortex-a72 / -smp 2）
WORKTREE=05 bash $WT/scripts/22-macos-run-test.sh 05-iio 300
WORKTREE=05 bash $WT/scripts/22-macos-run-test.sh smoke  180
```

预期结果（本阶段最终 clean 构建的实测）：

```text
[TEST:END] 05-iio pass=22 fail=0        # logs/20260914-134111-05-05-iio.log:305
[TEST:END] smoke  pass=15 fail=0        # logs/20260914-134114-05-smoke.log:296
```

### 7.2 QEMU 内可复现的命令序列（逐条 + 预期输出）

默认 initramfs **没有交互式 shell**（`tests/runner/init.sh` 挂好
proc/sys/dev/debugfs/configfs → 按 `/etc/modules/<阶段>.load` insmod →
source `/tests/phases/<阶段>.sh` → 打印 `[TEST:END]` → `poweroff -f`）。
所以要"手敲"这些命令，做法是把它们写进一个阶段脚本（例如
`tests/phases/05-manual.sh`）再用 `TEST=05-manual` 构建/运行；
下面这些命令与 `tests/phases/05-iio.sh` 里已执行的部分逐条等价。

前置（由 `tests/runner/init.sh` 完成，人工跑时要自己补）：

```busybox
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev
mkdir -p /sys/kernel/debug /sys/kernel/config
mount -t debugfs  none /sys/kernel/debug
mount -t configfs none /sys/kernel/config
insmod /lib/modules/$(uname -r)/virt_i2c.ko
insmod /lib/modules/$(uname -r)/sensor_iio.ko
```

（模块清单来源：`tests/phases/05-iio.modules` = `virt_i2c.ko` + `sensor_iio.ko`，
日志打印 `--- 模块清单: /etc/modules/05-iio.load ---`，`logs/20260914-134111-05-05-iio.log:258`。
**不能加载 `sensor_char.ko`**，它会去抢 0x48 那颗从设备，且会污染 dmesg 判断。）

| 步骤 | 命令 | 预期输出（实测值） | 证据 |
|---|---|---|---|
| 1 找设备 | `cat /sys/bus/iio/devices/iio:device0/name` | `sensor_iio` | log:266-268 |
| 2 看绑定 | `readlink -f /sys/bus/i2c/devices/0-0049/driver` | `/sys/bus/i2c/drivers/sensor_iio` | log:271-272 |
| 3 dmesg | `dmesg \| grep sensor_iio` | `chip detected: config=0x0001` / `IIO device registered: name=sensor_iio channels=3` | log:263-264 |
| 4 单点读 raw | `cat /sys/bus/iio/devices/iio:device0/in_temp_raw` | `385`（24.06 ℃ 量级） | log:275-276 |
| 5 单点读 processed | `cat /sys/bus/iio/devices/iio:device0/in_temp_input` | `24187`（毫℃） | log:277-278 |
| 6 读 scale | `cat /sys/bus/iio/devices/iio:device0/in_temp_scale` | `62.500000` | log:279 |
| 7 湿度 raw | `cat /sys/bus/iio/devices/iio:device0/in_humidityrelative_raw` | `4002`（40.02 %RH） | log:280-281 |
| 8 建 trigger | `mkdir -p /sys/kernel/config/iio/triggers/hrtimer/inst0` | 目录存在；`/sys/bus/iio/devices/trigger0/name` = `inst0` | log:283-285 |
| 9 设采样率 | `echo 50 > /sys/bus/iio/devices/trigger0/sampling_frequency` 然后 `cat` 回读 | `50.000000` | log:286-287 |
| 10 绑 trigger | `echo inst0 > /sys/bus/iio/devices/iio:device0/trigger/current_trigger` 后 `cat` | `inst0` | log:289 |
| 11 选通道 | `for en in in_temp_en in_humidityrelative_en in_timestamp_en; do echo 1 > .../scan_elements/$en; done` 后逐个 `cat` | 都是 `1` | log:290-292 |
| 12 缓冲长度 | `echo 64 > .../buffer/length` 后 `cat` | `64` | log:293 |
| 13 开缓冲 | `echo 1 > .../buffer/enable` 后 `cat` | `1` | log:294 |
| 14 流式读 | `/bin/iio_read_test /dev/iio:device0 /sys/bus/iio/devices/iio:device0` | 见下 | log:296-300 |
| 15 收尾 | `echo 0 > .../buffer/enable`；`dmesg \| grep -cE 'WARNING:\|Call trace:'` | `0` | log:302 |

第 14 步的完整实测输出（`logs/20260914-133853-05-05-iio.log:299-305`，
该次日志额外回显了逐通道 offset；默认脚本只打印 `layout stride=` 一行）：

```text
[IIO] layout stride=16 channels=3
[IIO]   chan in_temp                  scan_index=0 en=1 type=le:s16/16>>0 offset=0
[IIO]   chan in_humidityrelative      scan_index=1 en=1 type=le:u16/16>>0 offset=2
[IIO]   chan in_timestamp             scan_index=2 en=1 type=le:s64/64>>0 offset=0
[IIO] dev_path=/dev/iio:device0
[IIO] samples_ok=5 samples_bad=0 timeouts=0
[IIO] dev_reads=5 temp_milli=24250 humidity_milli=40030 OVERALL=PASS
```

（`in_timestamp ... offset=0` 是打印缺陷，实际偏移 8，见 5.5 结尾的说明。）

### 7.3 理解那些"看起来不对"的数字

- **`in_temp_raw=385` 而 `in_temp_input=24187` 不是同一个数**：
  385 * 1000/16 = 24062，与 24187 差一个漂移台阶。
  原因不是 bug —— `virt_i2c` 的模拟芯片在**每次读温度寄存器时**自增样本计数并重算一次
  "转换"（`driver/virt_i2c.c:114-126`，触发点在 `:180`/`:194`），
  所以两次 sysfs 读取（raw 一次、processed 一次）会看到相邻台阶
  （锯齿波 24.0~26.0 ℃，步进 0.1 ℃，周期 21 次，`driver/virt_i2c.c:79-82`）。
  387 * 1000/16 = 24187 恰好对齐 —— **内部自洽**。
- **湿度 `4002` → `40020` 毫%RH**：`virt_i2c` 湿度 = `4000 + samples % 200`
  （0.01 %RH/LSB，`driver/virt_i2c.c:124`），驱动换算 `raw * 10`
  （`driver/sensor_iio.c:153`）。两次读取同理可能差一个台阶（实测 40020 / 40030）。
- **`stride=16`**：见第 5.3 节手算，与内核 `iio_compute_scan_bytes()` 及驱动结构体三方一致。

### 7.4 一个值得自己动手的负控（验证者已做过，可复现）

把 `user/iio_read_test.c` 的 `compute_layout()` 改成"把 `_index` 当字节偏移"重建，
观察 `stride` 实测变成 **10**、`iio_read_test` 得到 `-EINVAL` 且读到 0 个样本
（对应 `logs/20260914-133820-05-05-iio.log`：`pass=18 fail=4`）。
**这个实验的价值**：证明 `stride=16` 这条断言不是恒真，它真的能抓住布局解析错误。

---

## 8. 已知简化与未验证项（面试时主动说，避免被"你有没有想过…"问住）

| # | 项 | 状态 | 说明 |
|---|---|---|---|
| 1 | 未绑 trigger 时 `buffer/enable` 返回 `-EINVAL` | **读代码核对，未实测** | 依据 `industrialio-buffer.c:872-892`；本项目测试总是先绑 trigger |
| 2 | 通道子集（只开温度 / 只开湿度）的布局 | **未实测** | 手算见 5.3 表格；驱动未设 `available_scan_masks`，内核允许任意组合 |
| 3 | libiio / `iio_generic_buffer` 交叉验证 | **未做** | 湿度"毫%RH"约定与上游工具的换算未交叉确认 |
| 4 | 大端主机 | **未验证** | 用户程序有 le/be 分支，实测只在 aarch64 小端（`endianness = IIO_CPU` 在此即 LE） |
| 5 | `handle`/`repeat>1` 通道（`in_temp_type` 里的 `X<n>`） | **未涉及** | 用户程序 `parse_type()` 未解析 `X<n>`（`user/iio_read_test.c` 注释已说明本项目用不到） |
| 6 | 内核内消费者路径（`iio_channel_get()` + `iio_read_channel_processed()`） | **未实现** | 只讲了机制，本项目没有第二个驱动去消费它 |
| 7 | `active_scan_mask` 与 `buffer->scan_mask` 不一致时的 demux | **未触发** | 本项目两套掩码相同，`iio_update_demux()` 不产生拷贝（`industrialio-buffer.c:1044`） |
| 8 | `read_errors` 的观测通道 | **缺口** | 驱动累加了 `read_errors`（`driver/sensor_iio.c:89`）但没有 sysfs/debugfs 导出（验证报告 S4），且自增在锁外（S3） |
| 9 | 高采样率下的丢样本行为 | **未实测** | `buffer/length=2` 时理论上极易丢样本（`kfifo_buf.c:90-99` 返回 `-EBUSY`），本项目只在 64 条下验证 |
| 10 | `rmmod`/`insmod` 循环 | **已实测**（非断言） | `logs/20260914-134059-05-05-iio.log:303-314`，卸载/重装/无告警；但**阶段脚本未把它做成检查项** |

---

## 附：本文引用的内核源码索引

| 引用 | 内容 |
|---|---|
| `drivers/iio/industrialio-core.c:149-153` | `iio_chan_info_postfix[]`：`raw` / `input` / `scale` 后缀表 |
| `drivers/iio/industrialio-core.c:238-246` | `iio_find_channel_from_si()` |
| `drivers/iio/industrialio-core.c:740-764` | `iio_read_channel_info()`：sysfs 单点读入口 |
| `drivers/iio/industrialio-core.c:1847-1858` | `/dev/iio:deviceN` 的 `iio_buffer_fileops` |
| `drivers/iio/industrialio-core.c:1753-1761` | `iio_chrdev_open()` 独占打开（`-EBUSY`） |
| `drivers/iio/industrialio-core.c:1965-1966` | 无 setup_ops 时用 `noop_ring_setup_ops` |
| `drivers/iio/industrialio-buffer.c:106-170` | `iio_buffer_read()` |
| `drivers/iio/industrialio-buffer.c:240-262` | `iio_buffer_poll()` |
| `drivers/iio/industrialio-buffer.c:358-362` | `iio_show_scan_index()`：**index = scan_index，不是偏移** |
| `drivers/iio/industrialio-buffer.c:365-394` | `iio_show_fixed_type()`：`le:s16/16>>0` 的格式 |
| `drivers/iio/industrialio-buffer.c:507-544` | `iio_scan_el_store()`：buffer 激活时 `-EBUSY` |
| `drivers/iio/industrialio-buffer.c:555-580` | `iio_scan_el_ts_store()`：`scan_timestamp` |
| `drivers/iio/industrialio-buffer.c:688-697` | `iio_storage_bytes_for_si()` |
| `drivers/iio/industrialio-buffer.c:701-706` | `iio_storage_bytes_for_timestamp()` |
| `drivers/iio/industrialio-buffer.c:709-733` | **`iio_compute_scan_bytes()`：布局权威算法** |
| `drivers/iio/industrialio-buffer.c:777-789` | `iio_buffer_update_bytes_per_datum()` |
| `drivers/iio/industrialio-buffer.c:872-892` | 模式选择：无 trigger → `-EINVAL` |
| `drivers/iio/industrialio-buffer.c:1044` | `iio_update_demux()`（掩码不一致时的重排） |
| `drivers/iio/industrialio-buffer.c:1064-1074` | `iio_enable_buffers()` 设置 `currentmode` |
| `drivers/iio/industrialio-buffer.c:1112-1114` | enable 时 attach poll func 到 trigger |
| `drivers/iio/industrialio-buffer.c:1867-1881` | `iio_push_to_buffers()` |
| `include/linux/iio/buffer.h:37-48` | `iio_push_to_buffers_with_timestamp()`（`ts_offset = scan_bytes/8 - 1`） |
| `drivers/iio/buffer/kfifo_buf.c:69-77` / `:80-88` | `set_bytes_per_datum` / `set_length`（最小 2） |
| `drivers/iio/buffer/kfifo_buf.c:90-99` | `iio_store_to_kfifo()`（满则 `-EBUSY`） |
| `drivers/iio/buffer/kfifo_buf.c:100-119` | `iio_read_kfifo()`（**`n < esize` → `-EINVAL`，`:110`**） |
| `drivers/iio/buffer/kfifo_buf.c:194-201` | `kfifo_access_funcs.modes = SOFTWARE \| TRIGGERED` |
| `drivers/iio/buffer/industrialio-triggered-buffer.c:39-98` | `iio_triggered_buffer_setup_ext()`（`:65` pollfunc、`:81` 加 `INDIO_BUFFER_TRIGGERED`） |
| `include/linux/iio/triggered_buffer.h:33` | `devm_iio_triggered_buffer_setup()` 宏 |
| `drivers/iio/industrialio-trigger.c:197-216` | `iio_trigger_poll()`（硬中断上下文） |
| `drivers/iio/industrialio-trigger.c:258-274` | subirq 槽位分配（`CONFIG_IIO_CONSUMERS_PER_TRIGGER`） |
| `drivers/iio/industrialio-trigger.c:287-336` | `iio_trigger_attach_poll_func()`（`:307` `request_threaded_irq`） |
| `drivers/iio/industrialio-trigger.c:342-348` | detach 判断"最后一个使用者" |
| `drivers/iio/industrialio-trigger.c:371-398` | `iio_alloc_pollfunc()` |
| `drivers/iio/industrialio-trigger.c:420-505` | `current_trigger` show/store（`:455-458` `-EBUSY`） |
| `drivers/iio/industrialio-trigger.c:743-749` | `iio_validate_own_trigger()` |
| `drivers/iio/industrialio-sw-trigger.c:100-121` / `:132-146` / `:165-172` | `iio_sw_trigger_create()` / configfs `make_group` / 注册 `triggers` 组 |
| `drivers/iio/industrialio-configfs.c:21-30` | `iio_configfs_subsys`（configfs 子系统 `iio`） |
| `drivers/iio/trigger/iio-trig-hrtimer.c:32-34` | `iio_hrtimer_type` 只填 `.ct_owner`（**configfs 目录没有 sampling_frequency**） |
| `drivers/iio/trigger/iio-trig-hrtimer.c:37-47` / `:50-78` | `sampling_frequency` 的 show / store |
| `drivers/iio/trigger/iio-trig-hrtimer.c:80-96` | `DEVICE_ATTR(sampling_frequency, ...)` + 属性组 |
| `drivers/iio/trigger/iio-trig-hrtimer.c:98-108` | `iio_hrtimer_trig_handler()`（`hrtimer_forward_now` + `iio_trigger_poll`） |
| `drivers/iio/trigger/iio-trig-hrtimer.c:110-122` | `set_trigger_state`（`HRTIMER_MODE_REL_HARD`） |
| `drivers/iio/trigger/iio-trig-hrtimer.c:129-166` | `iio_trig_hrtimer_probe()`（`:138` 名字 = 实例名，`:145` 挂属性组，`:151-152` 默认 100 Hz） |
| `include/linux/iio/iio.h:241` / `:246` / `:254-255` | `iio_chan_spec` / `scan_index` / `scan_type` / `info_mask_separate` |
| `include/linux/iio/iio.h:309-317` | `IIO_CHAN_SOFT_TIMESTAMP()` |
| `include/linux/iio/iio.h:457` | `iio_info.read_raw` |
| `include/linux/iio/iio.h:577-601` | `struct iio_dev` |
| `include/linux/iio/buffer_impl.h:81-114` | `struct iio_buffer`（`bytes_per_datum` / `scan_mask` / `scan_timestamp` / `pollq`） |
| `include/linux/iio/trigger.h:60` / `:74-75` | `struct iio_trigger` / `subirqs[CONFIG_IIO_CONSUMERS_PER_TRIGGER]` |
| `drivers/iio/inkern.c:711-730` | `iio_convert_raw_to_processed()` |
| `lib/kfifo.c:270-292` | `__kfifo_to_user()`（`:279` `len /= esize`，非整数倍会短读） |
| `tools/iio/iio_generic_buffer.c:54-80` | **用户态权威实现** `size_from_channelarray()` |
| `tools/iio/iio_utils.c:292-306` / `:437-460` | `bsort_channel_array_by_index()` / 读 `*_index` |
| `.config`（内核） | `CONFIG_IIO=y`、`CONFIG_IIO_BUFFER=y`、`CONFIG_IIO_KFIFO_BUF=y`、`CONFIG_IIO_TRIGGERED_BUFFER=y`、`CONFIG_IIO_CONFIGFS=y`、`CONFIG_IIO_TRIGGER=y`、`CONFIG_IIO_SW_TRIGGER=y`、`CONFIG_IIO_HRTIMER_TRIGGER=y`、`CONFIG_IIO_CONSUMERS_PER_TRIGGER=2`、`CONFIG_REGMAP=y`、`CONFIG_REGMAP_I2C=y` |

## 附：本项目文件引路

| 文件 | 看什么 |
|---|---|
| `driver/sensor_iio.c:131-168` | `read_raw()`：raw / processed / scale 三个分支 |
| `driver/sensor_iio.c:174-209` | 通道表（scan_index / scan_type / timestamp） |
| `driver/sensor_iio.c:220-253` | `trigger_handler`：active_scan_mask、push、notify_done |
| `driver/sensor_iio.c:255-329` | `probe()`：regmap → iio_dev → triggered buffer → register（全 devm） |
| `user/iio_read_test.c` | 用户态镜像内核布局算法（`compute_layout()`、`extract()`） |
| `tests/phases/05-iio.sh` | 22 项检查：raw/scale、trigger、bind、scan_elements、buffer、流式读、dmesg |
| `tests/phases/05-iio.modules` | 本阶段只加载 `virt_i2c.ko` + `sensor_iio.ko` |
| `dts/sensor-node.dts.inc` | `sensor@48`（字符设备版）/ `sensor@49`（IIO 版） |
| `driver/virt_i2c.c:79-126` | 模拟芯片：确定性锯齿波、`0x48~0x4b` 各一套 bank |
| `docs/impl/05-iio-实现记录.md` | 实现过程与踩坑（P1 布局、P2 sampling_frequency、P3 模块清单） |
| `docs/verify/05-iio-验证报告.md` | 独立验证：22 项检查表、4 个负控、代码审查 S1~S6 |
