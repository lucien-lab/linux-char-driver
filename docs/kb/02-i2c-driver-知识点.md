# 阶段 02 知识点：虚拟 I2C 控制器 + 标准 i2c_driver + regmap + 设备树子节点匹配

> 面向目标：嵌入式 / Linux 驱动岗面试。读完应当能**讲清四层对象的关系、画出
> "设备树 → client → probe" 的完整调用链、说清 regmap 的选择逻辑与字节序陷阱，
> 并能被追问到"不这么写会怎样"**。
>
> **引用约定**
> - 内核源码引用来自本项目目标内核树 `~/kernel-build/linux-6.6.156`（Linux 6.6.156），
>   格式 `文件:行号`；行号在该内核树上可 `grep -n` 直接复核。
> - 本阶段改动前的实现（platform_driver 手工建 client）用
>   `文件@<commit>:行号` 引用，例：`driver/sensor_char.c@53f1498:466`。
> - 所有"实测"结论都标了日志文件名，日志在 `logs/`。
>
> **本文使用的日志**
> | 日志 | 内容 | 结论 |
> |---|---|---|
> | `logs/20260914-040921-02-i2c-driver.log` | 阶段 02 测试（当前 HEAD 重跑） | 16 PASS / 0 FAIL |
> | `logs/20260914-040819-zz-kb-probe.log` | 临时探针：总线拓扑、regmap debugfs、sysfs 手工实例化、故障注入 | 4 PASS（探针脚本已删，见 §7.6） |
> | `logs/20260914-040851-zz-kb-probe.log` | 临时探针：**字节序负控实验** + 故障注入打到驱动错误路径 | 1 PASS |
> | `logs/20260914-041825-smoke.log` | 回归（本文档写完前最后一次，与当前 HEAD 一致） | 15 PASS |
> | `logs/20260914-040014-smoke.log` | 回归（早前一次） | 15 PASS |
> | `logs/20260914-025014-01-io-models.log` | 阶段 02 **改造前**的基线（platform_driver + i2c-stub） | 对照用 |
> | `logs/20260914-040716-02-i2c-driver.log` | 独立验证者的**负控 A**（删掉 `.val_format_endian`） | 15 PASS / **1 FAIL**，§3.3.3 引用其结果 |
>
> 说明：`zz-kb-probe.sh` 是本文档为取证临时加的脚本，取证完已从仓库删除；
> 本文 §7 把它的全部命令内容原样列出，任何人可以用同样的方式重放。

---

## 0. 一句话心智模型

阶段 01 的心智模型是"**驱动只回答状态 + 登记唤醒源**"。
阶段 02 的心智模型是：

> **驱动不创造设备，驱动只是"认领"内核已经造好的设备。**

具体地说，设备树描述"板子上有什么"，**I2C 核心（i2c-core）负责把设备树节点变成
`i2c_client` 这个内核对象**，**驱动模型（driver core）负责把 `i2c_client` 和
`i2c_driver` 配对并调用 `probe(client)`**。驱动要做的事只有三件：

1. 注册一个 `i2c_driver`，声明"我用哪张表匹配"（`compatible` 字符串）；
2. 在 `probe(struct i2c_client *client)` 里从 `client` 取地址、取 `of_node`、取 IRQ，
   而不是自己去解析"总线号 + 从机地址"；
3. 用 `regmap` 访问寄存器，把"位宽 / 字节序 / 缓存"这类总线细节交出去。

阶段 02 之前，本项目的 `sensor_char.c` 是 `platform_driver`：它在自己 `probe(platform_device)`
里 `of_property_read_u32(np, "i2c-bus", &bus)` + `i2c_get_adapter(bus)` +
`i2c_new_client_device()` 手工造一个 client（`driver/sensor_char.c@53f1498:437,454,466`）。
那是 board file 时代的"手工实例化"路子。改成 `i2c_driver` 后，这 4 行代码全部删除，
换成了内核自动枚举 —— 这是本阶段最本质的变化。

---

## 1. Linux I2C 子系统分层

### 1.1 四个对象：谁是谁，谁持有谁

```
                 ┌──────────────────────────────┐
                 │   i2c_driver（从设备驱动）     │  "我支持 compatible=X 的芯片"
                 │   probe(struct i2c_client *)  │
                 └──────────────┬───────────────┘
                                │ 1 : N（一个驱动可服务多个同型号 client）
                                ▼
   ┌──────────────────┐   ┌──────────────────────┐
   │ i2c_algorithm    │◄──│   i2c_adapter        │  一条总线（控制器实例）
   │ master_xfer?     │1:1│   struct device dev  │  ← 挂在 platform 总线下
   │ smbus_xfer?      │   │   of_node 挂 dev 上  │
   │ functionality()  │   └──────────┬───────────┘
   └──────────────────┘              │ 1 : N（一条总线上多个从设备）
                                     ▼
                          ┌──────────────────────┐
                          │   i2c_client         │  从设备实例
                          │   addr / name        │  ← 由 i2c 核心创建（DT 枚举或手工）
                          │   struct device dev  │
                          └──────────────────────┘
```

| 对象 | 语义 | 内核定义 | 本项目中的实例 |
|---|---|---|---|
| `i2c_adapter` | 一条 I2C 总线（控制器）。它本身也是一个 `struct device`，注册到**平台总线**或别的总线下 | `include/linux/i2c.h:719` | `i2c-0`，由 `virt_i2c.ko` 注册（`driver/virt_i2c.c:339`） |
| `i2c_algorithm` | 这条总线**怎么搬字节**：`master_xfer`（发 I2C 消息）/`smbus_xfer`（发 SMBus 事务）/`functionality`（能力位图） | `include/linux/i2c.h:541`（`master_xfer` :551，`smbus_xfer` :555，`functionality` :563） | `virt_i2c_algo`（`driver/virt_i2c.c:222`） |
| `i2c_client` | 挂在某条总线上的**从设备实例**：`addr`（7 位地址）、`name`、`adapter`、`dev` | `include/linux/i2c.h:330` | `0-0048`（`sensor-char @0x48`） |
| `i2c_driver` | **从设备驱动**：`probe/remove(client)` + `id_table` + `driver.of_match_table` | `include/linux/i2c.h:271` | `sensor_i2c_driver`（`driver/sensor_char.c:718`） |

三句话记住关系：

- `adapter` 是**总线**，`client` 是**总线上的一块芯片**；一个 adapter 挂 N 个 client；
- `algorithm` 是 adapter 的**能力实现**（adapter 必有 algo，没有 algo 注册直接失败：
  `drivers/i2c/i2c-core-base.c:1524` 打印 `no algo supplied!`）；
- `driver` 是**写代码的人提供的那一半**，它不认识 adapter，只认识 `client`。

为什么 `i2c_client` 里要内嵌一个 `struct device`（`include/linux/i2c.h:346`）？
因为 driver core 的所有机制（`device_register`、匹配、`probe`、sysfs、`devm_*`、
电源管理、`devres`）都建立在 `struct device` 上（`include/linux/device.h:724`）。
`i2c_client` 只有内嵌 `device`，才能被 `device_add()` 交给总线去匹配 —— 这是
"总线模型"能复用的根本原因。

### 1.2 `probe()` 里的两个"取数据"接口

```c
/* driver/sensor_char.c:587 */
static int sensor_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;      /* DT 节点，来自 client->dev.of_node */
	u32 interval = 500;
	of_property_read_u32(np, "poll-interval-ms", &interval);   /* :597 */
	...
	sd->sensor_addr = client->addr;             /* :608，从机地址由 reg 属性填进来 */
	i2c_set_clientdata(client, sd);             /* :610，等价于 dev_set_drvdata */
```

| 想拿的东西 | 正确写法 | 内核位置 |
|---|---|---|
| 从机地址 | `client->addr` | `include/linux/i2c.h:341` |
| 设备树节点 | `client->dev.of_node`（或 `dev_of_node(dev)`） | `include/linux/device.h:795` / `:1119` |
| 私有数据挂载 | `i2c_set_clientdata()` / `i2c_get_clientdata()` | `include/linux/i2c.h:374` / `:369` |
| adapter 私有数据 | `i2c_set_adapdata()` / `i2c_get_adapdata()` | `include/linux/i2c.h:762` / `:757` |

**追问点：为什么 `remove()` 里不用管 `client` 的释放？**
因为 client 是 i2c 核心创建的，生命周期由核心管理。`driver/sensor_char.c:694` 的注释
写的就是这件事：驱动只释放自己申请的资源。对比旧实现，
`driver/sensor_char.c@53f1498:527` 必须自己 `i2c_put_adapter()` + 释放手工创建的 client，
少一处就是引用计数泄漏。

### 1.3 I2C 与 SMBus：不是同一套协议

| 维度 | I2C | SMBus |
|---|---|---|
| 来源 | Philips/NXP 的**电气 + 时序**标准 | Intel 在 I2C 之上定义的**协议子集**（时序更严：`tLOW`/`tHIGH` 有下限、超时 35ms、必须支持 PEC） |
| 消息模型 | 任意长度、任意数量的 `struct i2c_msg`，START/ReSTART/STOP 由驱动自由组合 | 固定的几种**事务类型**（Quick / Byte / Byte Data / Word Data / Block …），带 "命令字节（command）" 概念 |
| 内核抽象 | `i2c_algorithm.master_xfer(adap, msgs, num)` | `i2c_algorithm.smbus_xfer(adap, addr, flags, rw, command, size, data)` |
| 内核 API | `i2c_transfer()` / `i2c_master_send/recv()` | `i2c_smbus_read_word_data()` 等一组 |
| 文档 | `Documentation/i2c/i2c-protocol.rst` | `Documentation/i2c/smbus-protocol.rst` |

关键点：**SMBus 事务可以在"只实现 I2C 消息"的控制器上被模拟出来**，反过来不行。
内核的模拟器是 `i2c_smbus_xfer_emulated()`（`drivers/i2c/i2c-core-smbus.c:322`），
它把 SMBus 事务翻译成一两个 `i2c_msg` 后调用 `master_xfer`。

### 1.4 `smbus_xfer` 与 `master_xfer`：到底何时用哪个

调用方**不需要**选择，选择权在 i2c 核心：

```c
/* drivers/i2c/i2c-core-smbus.c:578 */
xfer_func = adapter->algo->smbus_xfer;
...
if (xfer_func) {
	for (res = 0, try = 0; try <= adapter->retries; try++) {
		res = xfer_func(adapter, addr, flags, read_write,
				command, protocol, data);
		...
	}
	if (res != -EOPNOTSUPP || !adapter->algo->master_xfer)
		goto trace;
	/*
	 * Fall back to i2c_smbus_xfer_emulated if the adapter doesn't
	 * implement native support for the SMBus operation.
	 */
}
res = i2c_smbus_xfer_emulated(adapter, addr, flags, read_write,
			      command, protocol, data);      /* drivers/i2c/i2c-core-smbus.c:607 */
```

即 **SMBus 入口（`i2c_smbus_*` / `i2c_smbus_xfer`）的三级选择**：

1. adapter 有 `smbus_xfer` → **直接调 `smbus_xfer`**（优先，语义最准、开销最小）；
2. 有 `smbus_xfer` 但它返回 `-EOPNOTSUPP` 且 adapter 有 `master_xfer` → **用 `i2c_smbus_xfer_emulated()` 翻译**；
3. 没有 `smbus_xfer` 但有 `master_xfer` → 同样走模拟（分支 2 与 3 合流）。

**I2C 入口（`i2c_transfer`）只认 `master_xfer`**：

```c
/* drivers/i2c/i2c-core-base.c:2259 */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	...
	if (!adap->algo->master_xfer) {
		dev_dbg(&adap->dev, "I2C level transfers not supported\n");
		return -EOPNOTSUPP;                    /* :2264 */
	}
	...
	ret = adap->algo->master_xfer(adap, msgs, num);   /* :2299 */
```

这两段源码直接回答了"何时用哪个"：

> **谁写在 `i2c_algorithm` 里，是控制器驱动作者决定的**（能原生讲 SMBus 就写 `smbus_xfer`，
> 只能搬 I2C 消息就只写 `master_xfer`，两者都能写就先走 `smbus_xfer`）；
> **谁被调用，是 i2c 核心按上面的规则决定的**；**上层驱动（如 `regmap_i2c`）只调用
> `i2c_smbus_*` 或 `i2c_transfer`，不需要知道背后是哪一条路**。

**本项目的选择**：`virt_i2c.c` 只实现 `smbus_xfer`（`driver/virt_i2c.c:137,223`），
不实现 `master_xfer`。原因是我们的"芯片"按寄存器语义工作，SMBus "Byte Data / Word Data"
就是寄存器读写的天然映射；而 `master_xfer` 要自己拆解 `i2c_msg` 里的寄存器地址字节、
处理 `I2C_M_RD` 与 ReSTART，对教学目标只增加噪声。

**代价必须知道**：这样做的 adapter **不支持 I2C 消息层**。实测证据：
用 `i2c_smbus_read_word_data` 走 `smbus_xfer` 一切正常（`logs/20260914-040921-02-i2c-driver.log:279`
的 `registers 前几行：0: 0181 1: 0fa1 2: 0000`）；调 `i2c_transfer()` 会返回
`-EOPNOTSUPP`（源码路径 `drivers/i2c/i2c-core-base.c:2264`，本项目未单独构造该调用，
标注**未验证**）。

### 1.5 `functionality`：能力声明，声明错就换错路

```c
/* driver/virt_i2c.c:212 */
static u32 virt_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	       I2C_FUNC_SMBUS_BYTE;
}
```

调用方用 `i2c_check_functionality()` 查询（`include/linux/i2c.h:911`）：

```c
static inline int i2c_check_functionality(struct i2c_adapter *adap, u32 func)
{
	return (func & i2c_get_functionality(adap)) == func;
}
```

**这是本项目最需要记住的一条"声明即契约"**：`regmap_i2c` 的后端选择就是查这个位图
（见 §3.2）。如果 `virt_i2c` 顺手多声明一个 `I2C_FUNC_I2C`，regmap 会走 `regmap_i2c`
后端（`drivers/base/regmap/regmap-i2c.c:314-315`），进而调 `i2c_transfer()` → `master_xfer`，
而 `master_xfer` 是 NULL → `-EOPNOTSUPP`（`drivers/i2c/i2c-core-base.c:2264`），
所有寄存器访问全挂。`driver/virt_i2c.c:212` 上方的注释写的就是这个坑。

同类现实坑：真机 SoC 的 I2C 控制器常常只声明 `I2C_FUNC_SMBUS_*` 的一个子集，
传感器驱动照着数据手册写 `i2c_smbus_read_i2c_block_data()`，
到了那颗 SoC 上直接被 `i2c_check_functionality()` 拒掉 —— 这是"换板子就废"的经典原因。

---

## 2. 从设备树到 `probe(client)` 的完整链路

### 2.1 全链路（带源码行号，本项目实测走过）

前提：`dts/sensor-node.dts.inc` 把控制器节点 `virt-i2c` 与子节点 `sensor@48`
放进 QEMU `virt` 机器设备树的根节点下，重新编译成 `artifacts/virt-sensor.dtb`，
由 `qemu-system-aarch64 -dtb` 传入。

```text
【A. 控制器：platform driver 阶段】
  ① platform 总线匹配 virt_i2c_driver                          driver/virt_i2c.c:376
       .of_match_table = { .compatible = "lucien,virt-i2c" }   driver/virt_i2c.c:370
     → virt_i2c_probe(pdev)                                    driver/virt_i2c.c:307
  ② chip->adap.dev.of_node = pdev->dev.of_node;   ★★★ 关键一步  driver/virt_i2c.c:337
  ③ i2c_add_adapter(&chip->adap)                               driver/virt_i2c.c:339
       → i2c_register_adapter(adap)                            drivers/i2c/i2c-core-base.c:1509
           → dev_set_name(&adap->dev, "i2c-%d", adap->nr)      drivers/i2c/i2c-core-base.c:1549
           → device_add(&adap->dev)      ← adapter 作为设备注册 drivers/i2c/i2c-core-base.c:1563
           → of_i2c_register_devices(adap)                     drivers/i2c/i2c-core-base.c:1593

【B. 枚举：设备树 → i2c_client】
  ④ of_i2c_register_devices(adap)                              drivers/i2c/i2c-core-of.c:85
       if (!adap->dev.of_node) return;      ★★★ 没有 of_node 直接返回  drivers/i2c/i2c-core-of.c:91
       for_each_available_child_of_node(bus, node)             drivers/i2c/i2c-core-of.c:100
  ⑤ of_i2c_register_device(adap, node)                         drivers/i2c/i2c-core-of.c:65
       → of_i2c_get_board_info(&adap->dev, node, &info)        drivers/i2c/i2c-core-of.c:22
           name  ← of_alias_from_compatible(node, info->type)  drivers/i2c/i2c-core-of.c:30
                   → 取 compatible 逗号后的部分                  drivers/of/base.c:1041
                   （"lucien,sensor-char" → "sensor-char"）
           addr  ← of_property_read_u32(node, "reg", &addr)    drivers/i2c/i2c-core-of.c:35
           info.of_node = node                                 drivers/i2c/i2c-core-of.c:52
       → i2c_new_client_device(adap, &info)                    drivers/i2c/i2c-core-of.c:78

【C. client 注册：进入 driver core】
  ⑥ i2c_new_client_device(adap, info)                          drivers/i2c/i2c-core-base.c:957
       client->addr / flags / init_irq                          drivers/i2c/i2c-core-base.c:971-983
       strscpy(client->name, info->type, ...)                  drivers/i2c/i2c-core-base.c:978
       client->dev.parent = &client->adapter->dev              drivers/i2c/i2c-core-base.c:996
       client->dev.bus    = &i2c_bus_type                      drivers/i2c/i2c-core-base.c:997
       client->dev.of_node = of_node_get(info->of_node)        drivers/i2c/i2c-core-base.c:999
       i2c_dev_set_name(adap, client, info)                    drivers/i2c/i2c-core-base.c:1014
           → dev_set_name(&client->dev, "%d-%04x", adap->nr, addr)  drivers/i2c/i2c-core-base.c:889
       device_register(&client->dev)      ← 进入 driver core    drivers/i2c/i2c-core-base.c:1015

【D. 匹配与 probe：driver core 通用流程】
  ⑦ device_register(dev)                                       drivers/base/core.c:3779
       device_initialize(dev); device_add(dev)                  drivers/base/core.c:3781
  ⑧ device_add(dev)                                            drivers/base/core.c:3567
       → bus_probe_device(dev)                                  drivers/base/bus.c:571
           → device_initial_probe(dev)                          drivers/base/dd.c:1156
               → __device_attach(dev, true)                     drivers/base/dd.c:1079
                   → bus_for_each_drv(..., __device_attach_driver) drivers/base/dd.c:1066
  ⑨ __device_attach_driver(drv, data)                          drivers/base/dd.c:1000
       ret = driver_match_device(drv, dev)                       drivers/base/dd.c:1007
           → drv->bus->match(dev, drv)     ← i2c_bus_type.match   drivers/base/base.h:165
               = i2c_device_match()                              drivers/i2c/i2c-core-base.c:140
                   1) i2c_of_match_device(drv->of_match_table, client)  :143（OF 优先）
                   2) acpi_driver_match_device(dev, drv)                :148
                   3) i2c_match_id(driver->id_table, client)            :157
  ⑩ driver_probe_device(drv, dev)                               drivers/base/dd.c:903
       → __driver_probe_device()                                drivers/base/dd.c:838
           → really_probe(dev, drv)                             drivers/base/dd.c:662
               → call_driver_probe(dev, drv)                    drivers/base/dd.c:631
                   dev->bus->probe(dev)  ← i2c 总线的 probe      drivers/base/dd.c:635
                     = i2c_device_probe()                       drivers/i2c/i2c-core-base.c:492
                       （i2c_bus_type 注在 :699）
                         解析 IRQ（of_irq_get / "irq" 属性）       drivers/i2c/i2c-core-base.c:504-535
                         id_table 可选的判定（OF/ACPI 可替代）     drivers/i2c/i2c-core-base.c:536-544
                         driver->probe(client) ★ 真正的 probe      drivers/i2c/i2c-core-base.c:588
                           = sensor_probe(client)                driver/sensor_char.c:587
```

**实测落点**（`logs/20260914-040819-zz-kb-probe.log`，同一份驱动、同一次构建；`logs/20260914-040921-02-i2c-driver.log:260-263` 的 probe 序列与此一致，只是时间戳不同）：

```
[    0.815258] virt_i2c virt-i2c: registered i2c adapter i2c-0, of_node=/virt-i2c (sub-devices enumerated from DT)
[    0.831570] sensor_char 0-0048: probe: i2c client addr=0x48 interval=500ms
[    0.832420] sensor_char 0-0048: chip detected: config=0x0001 -> enable continuous conversion
[    0.833412] sensor_char 0-0048: irq registered: virq=21
[    0.833536] sensor_char 0-0048: probe done: major=511 minor=0 /dev/sensor0
```

注意日志前缀 `sensor_char 0-0048:`：`dev_driver_string()` 取的是 **client 上绑定的
i2c_driver 名**（`sensor_char`），`dev_name()` 取的是 client 的 sysfs 名（`0-0048`）。
改造前是 `i2c 0-0048:`（`logs/20260914-025014-01-io-models.log:269`）—— 因为那时
client 是驱动手工造的，**没有任何 i2c_driver 绑定到它**，`dev_driver_string()` 退化成
总线名 `i2c`。这一行日志前缀的差别就是"旧路子 vs 新路子"的直接证据。

### 2.2 为什么 `adapter.of_node` 是关键

`of_i2c_register_devices()` 的第一句就是守卫：

```c
/* drivers/i2c/i2c-core-of.c:85 */
void of_i2c_register_devices(struct i2c_adapter *adap)
{
	/* Only register child devices if the adapter has a node pointer set */
	if (!adap->dev.of_node)
		return;                       /* :91 —— 没有 of_node，设备树枚举整条链路直接断掉 */
	...
	bus = of_get_child_by_name(adap->dev.of_node, "i2c-bus");
	if (!bus)
		bus = of_node_get(adap->dev.of_node);    /* :96-98 —— 找 "i2c-bus" 子节点，否则用自身 */
	for_each_available_child_of_node(bus, node) { ... }    /* :100 */
```

两个结论：

1. **`of_node` 是"这根总线在设备树里的身份"**。i2c 核心要枚举子设备，唯一的入口就是它；
   它是 `struct device` 的标准字段（`include/linux/device.h:795`），i2c 核心没有别的替代通道。
   `platform_device` 的 `of_node` 是 platform 总线在匹配时填好的（`of_driver_match_device`
   建立关联），所以控制器驱动只要做一次赋值即可。
2. 枚举的是 **`adap->dev.of_node` 的（可用）子节点**。因此设备树里"控制器 → 从设备"必须是
   **父子关系**，`reg = <0x48>` 是子节点属性。旧写法用 `i2c-bus = <0>`（总线编号）+
   `sensor-addr = <0x48>`（独立属性）描述同一件事，但这两个属性没有任何内核代码认识它们 ——
   只有本驱动自己 `of_property_read_u32` 去读（`driver/sensor_char.c@53f1498:437-438`）。
   即"私有属性 = 非标准 = 内核不认"，这是旧设计最别扭的地方。

**实测证据**（`logs/20260914-040819-zz-kb-probe.log`）：

```
--- ls -l /sys/bus/i2c/devices/i2c-0/of_node ---
/sys/bus/i2c/devices/i2c-0/of_node -> ../../../../firmware/devicetree/base/virt-i2c
--- ls -l /sys/bus/i2c/devices/0-0048/of_node ---
/sys/bus/i2c/devices/0-0048/of_node -> ../../../../../firmware/devicetree/base/virt-i2c/sensor@48
```

父子关系一目了然：`.../base/virt-i2c/sensor@48`。

### 2.3 名字是怎么来的（三个名字，别混）

| 名字 | 值（实测） | 来源 |
|---|---|---|
| `client->name` | `sensor-char` | `of_alias_from_compatible()` 取 `compatible` 逗号后的部分（`drivers/of/base.c:1041`） |
| `dev_name(&client->dev)`（sysfs 名） | `0-0048` | `dev_set_name("%d-%04x", adap->nr, addr)`（`drivers/i2c/i2c-core-base.c:889`） |
| adapter 名 | `virt-i2c` / sysfs 里是 `i2c-0` | 驱动里 `strscpy(chip->adap.name, DRV_NAME, ...)`（`driver/virt_i2c.c:327`）；sysfs 名由 `dev_set_name("i2c-%d")`（`drivers/i2c/i2c-core-base.c:1549`） |

**这条是本项目测试的"照妖镜"**：`tests/phases/02-i2c-driver.sh` 里断言
`/sys/bus/i2c/devices/*-0048/name` == `sensor-char`。旧实现手工创建时写死
`strscpy(info.type, "sensor_demo", I2C_NAME_SIZE)`（`driver/sensor_char.c@53f1498:464`），
所以 `name` 一定是 `sensor_demo` —— 这个检查项能区分"内核从设备树枚举"和"驱动自己造"。
实测：`--- cat /sys/bus/i2c/devices/0-0048/name --- → sensor-char`
（`logs/20260914-040819-zz-kb-probe.log`）。

### 2.4 modalias 与 uevent：自动加载靠它们

实测（`logs/20260914-040819-zz-kb-probe.log`）：

```
--- cat /sys/bus/i2c/devices/0-0048/modalias ---
of:NsensorT(null)Clucien,sensor-char

--- cat /sys/bus/i2c/devices/0-0048/uevent ---
DRIVER=sensor_char
OF_NAME=sensor
OF_FULLNAME=/virt-i2c/sensor@48
OF_COMPATIBLE_0=lucien,sensor-char
OF_COMPATIBLE_N=1
MODALIAS=of:NsensorT(null)Clucien,sensor-char
```

`of:N<node名>T<type>C<compatible>` 由 `of_device_uevent_modalias()`
（`drivers/of/device.c:285`）生成。这个字符串就是用户态 udev / `modprobe` 用来
"按设备找驱动"的关键字（见 §4.4）。

### 2.5 board file 手工实例化为什么被设备树取代

内核自己列了 4 种实例化方式（`Documentation/i2c/instantiating-devices.rst:12,107,178,229`）：

| 方式 | 内核接口 | 谁描述硬件 | 本项目的命运 |
|---|---|---|---|
| Method 1 静态声明 | `i2c_register_board_info(busnum, info, len)`（`drivers/i2c/i2c-boardinfo.c:51`） | 板级 C 文件 | 淘汰 |
| Method 1' 设备树 | `of_i2c_register_devices()`（`drivers/i2c/i2c-core-of.c:85`） | `.dts` | **本项目采用** |
| Method 2 显式实例化 | 驱动里 `i2c_new_client_device()`（`drivers/i2c/i2c-core-base.c:957`） | 驱动自己 | 阶段 02 **删除**（旧实现用的就是这个，`driver/sensor_char.c@53f1498:466`） |
| Method 3 探测 | `i2c_new_scanned_device()`（`drivers/i2c/i2c-core-base.c:2597`）+ `driver.detect` | 靠"戳地址看谁应答" | 不用 |
| Method 4 用户态 | `/sys/bus/i2c/devices/i2c-N/new_device` | 用户自己 | 只用于调试 |

**board file 被取代的四个硬理由**（都能在源码里找到对应）：

1. **信息位置错误**：`i2c_register_board_info()` 把"芯片型号 + 地址 + IRQ"写进板级 C 代码
   （`drivers/i2c/i2c-boardinfo.c:51`），一个内核镜像只能描述一种板子；设备树把这份数据
   外置成数据文件，一个内核镜像配多个 `.dtb` 就能支持多种板子。
2. **创建时机耦合**：`i2c_register_board_info()` 只能把信息挂到全局链表 `__i2c_board_list`
   （`drivers/i2c/i2c-boardinfo.c:22,87`），要等 adapter 注册时由
   `i2c_scan_static_board_info()` 扫描（`drivers/i2c/i2c-core-base.c:1401,1598`）。
   设备树枚举与 ACPI 枚举并列放在同一处（`drivers/i2c/i2c-core-base.c:1593-1595`），
   是"一等公民"。
3. **驱动自己造 client = 生命周期自己扛**：`i2c_new_client_device()` 返回的 client
   必须由创建者 `i2c_unregister_device()`/`put_device()`（`drivers/i2c/i2c-core-base.c:1050`），
   忘了就是 `-ENOMEM`/悬挂引用；而 DT 枚举创建的 client 由核心在 adapter 注销时统一回收。
   旧实现里必须配对写 `i2c_put_adapter()`（`driver/sensor_char.c@53f1498:527,546`）。
4. **拿不到 driver core 的公共能力**：手工造的 client 上**没有 i2c_driver 绑定**
   （实测日志前缀 `i2c 0-0048:`，`logs/20260914-025014-01-io-models.log:269`），
   于是 `remove` 顺序、`devres` 分组释放（`drivers/i2c/i2c-core-base.c:580` 的
   `devres_open_group`）、电源管理、`uevent` 通知这些机制都用不上。

**追问点：没有设备树怎么办？**
三条标准替代路径：
- ACPI（x86 平台，`acpi_driver_match_device()`，`drivers/i2c/i2c-core-base.c:151`）；
- 平台数据结构/`software_node`（`info.swnode`，`drivers/i2c/i2c-core-base.c:1005`）；
- 用户态 sysfs：`echo <name> <addr> > /sys/bus/i2c/devices/i2c-N/new_device`
  （`new_device_store()`，`drivers/i2c/i2c-core-base.c:1252`）。

第三条本项目**实测过**（`logs/20260914-040819-zz-kb-probe.log`）：

```
echo kbprobe 0x4a > /sys/bus/i2c/devices/i2c-0/new_device
[    0.895142] i2c i2c-0: new_device: Instantiated device kbprobe at 0x4a
--- cat name ---     kbprobe
--- ls -l of_node（应不存在）---  ls: /sys/bus/i2c/devices/0-004a/of_node: No such file or directory
--- ls -l driver（应不存在，无名匹配）--- ls: /sys/bus/i2c/devices/0-004a/driver: No such file or directory
```

三条实测结论：
1. `new_device` 走的是同一个 `i2c_new_client_device()`（`drivers/i2c/i2c-core-base.c:1295`），
   所以**手工实例化只是"换一个入口"，底层是同一套对象模型**；
2. 手工 client **没有 `of_node`**（`info.of_node` 为 NULL），因此**不可能**参与
   `of_match_table` 匹配；
3. `kbprobe` 这个名字匹配不上 `sensor_id[]`（`driver/sensor_char.c:712`），所以没有驱动绑定，
   `driver` 符号链接不存在。用 `echo 0x4a > .../delete_device` 撤销
   （`delete_device_store()`，`drivers/i2c/i2c-core-base.c:1320`），实测删除成功。

### 2.6 加载顺序无关（这本身就是"总线模型"的价值）

两条路径都能到 probe：

- **先 client 后 driver**：`virt_i2c` 注册 adapter → 枚举出 client（无驱动可用，挂在总线上）；
  之后 `sensor_char` 执行 `i2c_add_driver()`（`driver/sensor_char.c:743`，宏展开为
  `i2c_register_driver()`，`include/linux/i2c.h:885`）→ `i2c_register_driver()`
  → `driver_register()`（`drivers/i2c/i2c-core-base.c:2027,2043`）→ `bus_add_driver()`
  （`drivers/base/bus.c:692`）→ `driver_attach()`（`drivers/base/dd.c:1310`）→
  `__driver_attach()`（`drivers/base/dd.c:1235`）→ 匹配已有 client → probe。
- **先 driver 后 client**：`i2c_add_driver()` 时没有匹配对象，返回 0；之后 adapter
  注册时 `device_add()` → `__device_attach()` → 匹配到已注册的 driver → probe。

`i2c_register_driver()` 的注释把这件事写得很直白：
"When registration returns, the driver core will have called probe() for all
matching-but-unbound devices."（`drivers/i2c/i2c-core-base.c:2040-2042`）

本项目实测走的是"先 client 后 driver"（`driver/modules.load` 顺序 `virt_i2c.ko` →
`sensor_char.ko`），`logs/20260914-040921-02-i2c-driver.log:260-263` 的 probe 日志
出现在 `insmod sensor_char.ko` 之后，即由 `i2c_add_driver()` 触发的匹配。

---

## 3. regmap 机制

### 3.1 regmap 抽象的是什么

`regmap`（register map access API）是"**寄存器访问的公共层**"。
判断一个驱动该不该用 regmap，看它是否要为下面这些事重复写代码：

| 维度 | 没有 regmap 时的写法 | regmap 的对应机制 | 源码位置 |
|---|---|---|---|
| 寄存器地址位宽 | 手工拼 `buf[0]=reg>>8; buf[1]=reg` | `reg_bits` / `reg_stride` / `reg_shift` / `pad_bits` | `include/linux/regmap.h:391,392,393,395` |
| 数据位宽 | 手工做 `buf[1]<<8\|buf[0]` | `val_bits` | `include/linux/regmap.h:396` |
| **字节序** | 每个驱动自己 `swab16` 或 `be16_to_cpu` | `reg_format_endian` / `val_format_endian` | `include/linux/regmap.h:446,447` |
| 寄存器窗口（地址分页） | 手工算 page + offset | `ranges` / `num_ranges`（`struct regmap_range_cfg`） | `include/linux/regmap.h:449,450` |
| 缓存的初值 | 手工 memset | `reg_defaults` / `reg_defaults_raw` | `include/linux/regmap.h:431,434` |
| 读写缓存策略 | 手工维护影子数组 | `cache_type`（NONE/RBTREE/FLAT/MAPLE） | `include/linux/regmap.h:433`、`include/linux/regmap.h:58` |
| 并发保护 | 每个驱动自己加 mutex / spinlock | `disable_locking` / `lock` / `unlock` / `use_hwlock` | `include/linux/regmap.h:405-407,459` |
| 访问权限（哪些寄存器可读/可写/易失） | 手工 if | `readable_reg/writeable_reg/volatile_reg/precious_reg` 及 `*_table` | `include/linux/regmap.h:398,399,400,401,425-432` |
| **总线差异** | 驱动里写死 `i2c_smbus_*` | `regmap_bus` 后端（i2c/spi/mmio/ac97/sdw…） | `drivers/base/regmap/regmap-i2c.c`、`regmap-mmio.c` 等 |
| 调试 | 自己写 debugfs | 自动挂 `/sys/kernel/debug/regmap/<dev>/` | `drivers/base/regmap/regmap-debugfs.c:546` |

**一句话总结**：regmap 把"一颗寄存器型外设"的**访问协议**与**访问策略**从驱动逻辑里剥离，
驱动只剩"读哪个寄存器、怎么换算"。

**本项目实际用到的只有 4 个字段**：

```c
/* driver/sensor_char.c:580 */
static const struct regmap_config sensor_regmap_cfg = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.max_register		= SENSOR_REG_CONFIG,       /* 0x02 */
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,    /* ★ 见 §3.3 */
};
/* driver/sensor_char.c:613 */
sd->regmap = devm_regmap_init_i2c(client, &sensor_regmap_cfg);
```

`devm_regmap_init_i2c()`（`include/linux/regmap.h:989`）→ `__devm_regmap_init_i2c()`
（`drivers/base/regmap/regmap-i2c.c:385`）→ `regmap_get_i2c_bus()` 选后端
→ `__devm_regmap_init()`。**选后端发生在初始化时，只此一次**。

### 3.2 `regmap_i2c` 如何按 `val_bits` 与适配器能力选后端

```c
/* drivers/base/regmap/regmap-i2c.c:306 */
static const struct regmap_bus *regmap_get_i2c_bus(struct i2c_client *i2c,
					const struct regmap_config *config)
{
	if (i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		bus = &regmap_i2c;                                  /* :313 raw i2c_transfer 路径 */
	else if (config->val_bits == 8 && config->reg_bits == 8 &&
		 i2c_check_functionality(i2c->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		bus = &regmap_i2c_smbus_i2c_block;                   /* :316 */
	else if (config->val_bits == 8 && config->reg_bits == 16 &&
		i2c_check_functionality(i2c->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		bus = &regmap_i2c_smbus_i2c_block_reg16;             /* :321 */
	else if (config->val_bits == 16 && config->reg_bits == 8 &&
		 i2c_check_functionality(i2c->adapter, I2C_FUNC_SMBUS_WORD_DATA))
		switch (regmap_get_val_endian(&i2c->dev, NULL, config)) {   /* :327 */
		case REGMAP_ENDIAN_LITTLE: bus = &regmap_smbus_word;             break;
		case REGMAP_ENDIAN_BIG:    bus = &regmap_smbus_word_swapped;     break;
		default: /* everything else is not supported */                  break;
		}
	else if (config->val_bits == 8 && config->reg_bits == 8 &&
		 i2c_check_functionality(i2c->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		bus = &regmap_smbus_byte;                            /* :340 */
	if (!bus)
		return ERR_PTR(-ENOTSUPP);                           /* :343 */
```

**判定表（自上而下，先命中先返回）**：

| 顺序 | 条件（`val_bits`/`reg_bits` + adapter 能力） | 选中后端 | 底层调用 |
|---|---|---|---|
| 1 | adapter 有 `I2C_FUNC_I2C` | `regmap_i2c` | `i2c_transfer()` → `master_xfer`（`regmap-i2c.c:163,191`） |
| 2 | 8/8 + `SMBUS_I2C_BLOCK` | `regmap_i2c_smbus_i2c_block` | `i2c_smbus_read_i2c_block_data()` |
| 3 | 8/16 + `SMBUS_I2C_BLOCK` | `..._block_reg16` | 同上（16 位寄存器地址） |
| 4 | **16/8 + `SMBUS_WORD_DATA`** | 看字节序：`regmap_smbus_word` 或 `regmap_smbus_word_swapped` | `i2c_smbus_read_word_data()`（`regmap-i2c.c:61`） |
| 5 | 8/8 + `SMBUS_BYTE_DATA` | `regmap_smbus_byte` | `i2c_smbus_read_byte_data()`（`regmap-i2c.c:22`） |
| — | 都不满足 | `-ENOTSUPP` | `regmap_init` 失败 |

**本项目的落点**：`cfg` 是 16/8，adapter 只声明 `SMBUS_BYTE_DATA|SMBUS_WORD_DATA|SMBUS_BYTE`
（`driver/virt_i2c.c:212`），因此命中**第 4 行**。
完整调用链：

```text
regmap_read(map, 0x00, &raw)                       driver/sensor_char.c:201
 → map->lock(); _regmap_read()                     drivers/base/regmap/regmap.c:2861,2819
    → regcache_read()（本配置没有 cache，直接 miss） drivers/base/regmap/regcache.c:237
    → regmap_readable() → map->reg_read()          drivers/base/regmap/regmap.c:2837
    → regmap_smbus_word_reg_read()                 drivers/base/regmap/regmap-i2c.c:51
       → i2c_smbus_read_word_data(client, reg)     drivers/i2c/i2c-core-smbus.c:177
          → i2c_smbus_xfer(..., I2C_SMBUS_WORD_DATA) drivers/i2c/i2c-core-smbus.c:184
             → __i2c_smbus_xfer()                  drivers/i2c/i2c-core-smbus.c:553
                → xfer_func = algo->smbus_xfer     drivers/i2c/i2c-core-smbus.c:578
                   → virt_i2c_smbus_xfer()         driver/virt_i2c.c:137
```

**反过来，为什么"声明 capability 不能随便抄"**：如果 `virt_i2c` 声明了 `I2C_FUNC_I2C`，
第 1 行先命中 → `regmap_i2c` 后端 → `i2c_transfer()` → `master_xfer == NULL` →
`-EOPNOTSUPP`（`drivers/i2c/i2c-core-base.c:2264`）。regmap 初始化**不会失败**
（它只查位图），失败发生在第一次读写 —— 症状是 probe 里第一次 `regmap_read()` 就报错，
`driver/sensor_char.c:625` 会打印 `chip not responding`。

**注意 i2c-stub 的对照**：`i2c-stub` 默认能力里有 `I2C_FUNC_SMBUS_I2C_BLOCK`
（`drivers/i2c/i2c-stub.c:24-26` 的 `STUB_FUNC_DEFAULT`），所以 16/8 配置在它上面
仍然走到第 4 行（顺序 2/3 要求 `val_bits==8`，不命中）。这说明"后端选择"与"chip 模拟"
是两件独立的事，别把二者混在一起推理。

### 3.3 字节序默认 BIG 的坑（本项目真实踩过）

#### 3.3.1 源码：默认值就是 BIG

```c
/* drivers/base/regmap/regmap.c:638 */
enum regmap_endian regmap_get_val_endian(struct device *dev,
					 const struct regmap_bus *bus,
					 const struct regmap_config *config)
{
	struct fwnode_handle *fwnode = dev ? dev_fwnode(dev) : NULL;
	enum regmap_endian endian;

	/* Retrieve the endianness specification from the regmap config */
	endian = config->val_format_endian;
	if (endian != REGMAP_ENDIAN_DEFAULT)          /* ① 驱动显式指定优先 */
		return endian;

	if (fwnode_property_read_bool(fwnode, "big-endian"))         /* ② 设备树属性 */
		endian = REGMAP_ENDIAN_BIG;
	else if (fwnode_property_read_bool(fwnode, "little-endian"))
		endian = REGMAP_ENDIAN_LITTLE;
	else if (fwnode_property_read_bool(fwnode, "native-endian"))
		endian = REGMAP_ENDIAN_NATIVE;
	if (endian != REGMAP_ENDIAN_DEFAULT)
		return endian;

	if (bus && bus->val_format_endian_default)    /* ③ 后端默认（regmap_smbus_* 没设，为 0） */
		endian = bus->val_format_endian_default;
	if (endian != REGMAP_ENDIAN_DEFAULT)
		return endian;

	return REGMAP_ENDIAN_BIG;                     /* ④ 兜底：BIG（regmap.c:673） */
}
```

（`reg_format_endian` 的同名逻辑在 `drivers/base/regmap/regmap.c:614-636`，兜底同样是 BIG。）

**决策顺序**：驱动 `config` 显式值 → 设备树 `big-endian`/`little-endian`/`native-endian`
→ `regmap_bus.val_format_endian_default` → **默认 `REGMAP_ENDIAN_BIG`**。

本项目的情况：`config` 没写（`REGMAP_ENDIAN_DEFAULT`）、设备树节点里没有 `little-endian`、
`regmap_smbus_word` 这个 bus 也没有设 `val_format_endian_default`
（`drivers/base/regmap/regmap-i2c.c:82-85` 只有 `reg_write/reg_read`）→ **落到 ④，取 BIG**。

#### 3.3.2 BIG 意味着什么：走 `_swapped` 后端，做 `swab16()`

```c
/* drivers/base/regmap/regmap-i2c.c:87 */
static int regmap_smbus_word_read_swapped(void *context, unsigned int reg,
					  unsigned int *val)
{
	...
	ret = i2c_smbus_read_word_swapped(i2c, reg);      /* :95 */
	if (ret < 0) return ret;
	*val = ret;
	return 0;
}
/* drivers/base/regmap/regmap-i2c.c:118 */
static const struct regmap_bus regmap_smbus_word_swapped = {
	.reg_write	= regmap_smbus_word_write_swapped,
	.reg_read	= regmap_smbus_word_read_swapped,
};
```

`i2c_smbus_read_word_swapped()` 是 `include/linux/i2c.h` 里的 inline：

```c
/* include/linux/i2c.h:163 */
static inline s32
i2c_smbus_read_word_swapped(const struct i2c_client *client, u8 command)
{
	s32 value = i2c_smbus_read_word_data(client, command);

	return (value < 0) ? value : swab16(value);        /* ★ 无条件字节交换 */
}
```

而 SMBus "Read Word" 协议本身是**低字节在前**（`Documentation/i2c/smbus-protocol.rst:117-123`
明确写了 word 的字节顺序，并提示 `i2c_smbus_read_word_swapped()` 是为反序芯片准备的便利函数）。
所以：**寄存器型芯片 + 小端数值 + 没声明字节序 = 被多交换一次 = 读数是错的。**

#### 3.3.3 现象（负控实验实测）

我把 `driver/sensor_char.c:584` 临时改成 `REGMAP_ENDIAN_BIG` 重编重跑，
得到与修好前一致的错误形态（`logs/20260914-040851-zz-kb-probe.log`）：

```
--- cat /sys/kernel/debug/regmap/0-0048/registers ---
0: 8101            ← 芯片内部真实值 temp_raw=0x0181（见同日志 virt_i2c/stats）
1: a10f            ← 真实值 0x0fa1
2: 0000

--- cat /sys/kernel/debug/virt_i2c/stats（芯片内部真实值）---
transfers=5 errors=0 ... bank=0x48 samples=1 temp_raw=0x0181 humidity_raw=4001 config=0x0000

--- grep temp= /tmp/utest.txt ---
temp=2224.062 C

[    0.852371] sensor_char 0-0048: chip detected: config=0x0100 -> enable continuous conversion
                                                       ↑ 真实 config=0x0001，被交换成 0x0100
```

逐条解释：

| 观测 | 解释 |
|---|---|
| `registers` 显示 `8101`，芯片真实 `0181` | `swab16(0x0181) == 0x8101`，字节交换直接被 debugfs 暴露 |
| probe 读到 `config=0x0100`，真实 `0x0001` | 同一个交换作用在 `regmap_read(..., SENSOR_REG_CONFIG)` 上（`driver/sensor_char.c:625`） |
| 用户态读到 `temp=2224.062 C` | 该样本芯片真实 raw = `0x018B`（=395；反推芯片当时产出的是 24700 m°C = **24.700 ℃**：`24700*16/1000 = 395.2` 截断成 395，来自锯齿公式 `25000 + (samples%21-10)*100`，`driver/virt_i2c.c:112-121`）。`swab16(0x018B)=0x8B01=35585`；驱动换算 `RAW_TO_MILLI(35585) = 35585*1000/16 = 2224062`（`driver/sensor_char.c:83`）→ **2224.062 ℃**，与实测逐位吻合 |

**按算法推出的完整错值区间**：温度 24.0~26.0 ℃ 对应 raw `0x0180`~`0x01A0`，
交换后 `0x8001`~`0xA001` = 32769~40961，换算成 **2048.062 ℃ ~ 2560.062 ℃**
（逐点验证：`0x0180→0x8001→2048.062`、`0x0190→0x9001→2304.062`、`0x01A0→0xA001→2560.062`，
与 `python3 -c "for m in (24000,25000,26000): raw=int(m*16/1000); sw=int.from_bytes(raw.to_bytes(2,'little'),'big'); print(m, hex(sw), sw*1000//16)"` 的输出一致）。
即"读数整体变成原来的约 83 倍，且符号位（bit15）被置起来"。

> **`driver/sensor_char.c:577` 注释里的"2304 ℃"是什么意思**：那是**锯齿波中心那一格**
> （25.000 ℃，raw `0x0190`）的错值。`0x0190` → `swab16` → `0x9001` = 36865，
> `36865*1000/16 = 2304062` → **2304.062 ℃**。三个观测点因此自洽，都是同一条直线上不同的采样位置：
>
> | 芯片当时真实温度 | 真实 raw | `swab16` 后 | 用户态读到 | 证据 |
> |---|---|---|---|---|
> | 24.700 ℃（本节的 run） | `0x018B` | `0x8B01` = 35585 | **2224.062 ℃** | `logs/20260914-040851-zz-kb-probe.log` |
> | 25.000 ℃（锯齿中心） | `0x0190` | `0x9001` = 36865 | **2304.062 ℃** | `driver/sensor_char.c:577` 注释；由本节算法推算 |
> | 约 24.2 ℃（独立负控 A） | `0x0183` | `0x8301` = 33537 | **2096.062 ℃** | `logs/20260914-040716-02-i2c-driver.log:292`（`[CHECK:FAIL] ... 实测 temp=2096.062`） |
>
> 也就是说：`driver/sensor_char.c:577` 注释里的 2304 ℃ **是对的**（它是中心格的取值），
> 本文 7.4 节的负控实验恰好抽到另一格（2224.062 ℃）—— **同一个 `swab16`，不同的采样点**，
> 不要把它当成两个不同的 bug。

**这个 bug 的测试价值**：`tests/phases/02-i2c-driver.sh` 里"温度读数落在 24.0~26.0 ℃"
这条检查会失败（`2048.062` 不匹配 `temp=2[456]\.`），所以它不是"看不见的 bug"——
**前提是你有一个能判断"数值是否合理"的断言**。这就是虚拟芯片必须模拟出
"物理上合理的量程"的原因（见 §5.2）。

#### 3.3.4 修法（两种，都写在源码里）

**修法 A（本项目采用）：在 `regmap_config` 里显式声明。**

```c
/* driver/sensor_char.c:580-585 */
static const struct regmap_config sensor_regmap_cfg = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.max_register		= SENSOR_REG_CONFIG,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
};
```

命中 `regmap_get_val_endian()` 的第 ① 步（`drivers/base/regmap/regmap.c:646-648`），
后端变成 `regmap_smbus_word`（`drivers/base/regmap/regmap-i2c.c:82`），读值不再交换。

**修法 B（等价，走设备树）：在 `sensor@48` 节点里加 `little-endian;`。**

```dts
sensor0: sensor@48 {
	compatible = "lucien,sensor-char";
	reg = <0x48>;
	poll-interval-ms = <0x1f4>;
	little-endian;              /* 会被 fwnode_property_read_bool() 读到 */
};
```

命中 `regmap_get_val_endian()` 的第 ② 步（`drivers/base/regmap/regmap.c:653-654`）。
本项目**没有采用 B**（单一事实来源留在驱动代码里，避免 DT 与驱动两边不一致），
该路径**未在本项目实测**。

**为什么不用 `native-endian`**：`REGMAP_ENDIAN_NATIVE` 是"用 CPU 字节序"，
它只对 `regmap-mmio` 这类"寄存器映射到内存、CPU 直接 load/store"的后端有定义；
`regmap_get_i2c_bus()` 的 `switch` 只处理 `LITTLE`/`BIG`，NATIVE 会落到
`default:` 分支 → `bus == NULL` → `-ENOTSUPP`（`drivers/base/regmap/regmap-i2c.c:326-336`）。

**排查这类问题的标准手法**（可背）：
1. 先看 `/sys/kernel/debug/regmap/<dev>/registers` 的**原始值**；
2. 跟数据手册/芯片真实值对比，若"高低字节互换" → 就是这里；
3. 再确认 `regmap_get_val_endian()` 的 4 步决策落在哪一步；
4. 修 `val_format_endian` 或补 DT 属性。

### 3.4 regmap 的锁与缓存（被追问时的加分项）

- **锁**：`__regmap_init()` 按配置装填 `map->lock/unlock`
  （`drivers/base/regmap/regmap.c:705-754`）：有 `use_raw_spinlock`/`use_hwlock` 用硬件锁，
  默认是 `regmap_lock_mutex`（`:754`）。`regmap_read()` 自己在外面加锁
  （`drivers/base/regmap/regmap.c:2868-2872`）。**因此 `regmap_read()` 可能睡眠，
  不能在硬中断上下文调用** —— 本项目把它放在线程化下半部（`driver/sensor_char.c:201`，
  位于 `sensor_irq_thread()` 内）正是这个原因。
- **缓存**：`cache_type` 默认为 0（`REGCACHE_NONE`），此时 `regcache_read()` 直接 miss
  （`drivers/base/regmap/regcache.c:237`），每次读都真的打到总线上。本项目的
  `/sys/kernel/debug/regmap/0-0048/` 下**没有** `cache_only`/`cache_dirty`/`cache_bypass`
  三个文件，就是"没有开缓存"的直接证据（`drivers/base/regmap/regmap-debugfs.c:629` 才有
  这三个文件）。
- **`_regmap_read()` 的顺序**（`drivers/base/regmap/regmap.c:2819`）：
  `regcache_read()` → `map->cache_only` → `regmap_readable()` → `map->reg_read()` →
  `regcache_write()`。所以"读一次就自动更新缓存""cache_only 时返回 `-EBUSY`"这两条
  都能从源码读出来。
- **本项目没声明 `readable_reg`/`writeable_reg`**，于是 `regmap_readable()` 走到最后
  `return true`（`drivers/base/regmap/regmap.c:144`），`regmap_writeable()` 同理，
  `regmap_volatile()` 在没有 cache 时返回 `true`（`drivers/base/regmap/regmap.c:161`）。
  这就是 `/sys/kernel/debug/regmap/0-0048/access` 输出 `0: y y y n` 的原因
  （四列依次是 readable / writeable / volatile / precious，
  `drivers/base/regmap/regmap-debugfs.c:447-451`）。
  **注意 regmap 认为 0x00（温度，只读）也可写** —— 真正的只读保护在芯片侧
  （`driver/virt_i2c.c:180-186` 返回 `-EIO` 并累加 `ro_writes`）。
  改进方向：声明 `.rd_table`/`.wr_table`，让 regmap 自己就拒绝非法写。
  **这条"regmap 的权限表是软约束、真正拦得住的是硬件"是很好的追问点。**

### 3.5 regmap 的 debugfs 调试接口

`regmap_debugfs_init()`（`drivers/base/regmap/regmap-debugfs.c:546`）在
`/sys/kernel/debug/regmap/` 下建目录，目录名取 `dev_name(map->dev)`
（`drivers/base/regmap/regmap-debugfs.c:582-586`）；根目录由
`regmap_debugfs_initcall()` 建（`drivers/base/regmap/regmap-debugfs.c:690-694`，
即 debugfs 必须可用：`CONFIG_DEBUG_FS=y`）。

实测目录与文件（`logs/20260914-040819-zz-kb-probe.log`）：

```
--- ls /sys/kernel/debug/regmap/ ---
0-0048
--- ls /sys/kernel/debug/regmap/0-0048/ ---
access     name       range      registers
--- cat name ---      sensor_char
--- cat registers --- 0: 0181 / 1: 0fa1 / 2: 0000
--- cat access ---    0: y y y n / 1: y y y n / 2: y y y n
```

| 文件 | 生成条件 | 内容 | 源码 |
|---|---|---|---|
| `name` | 总是 | **`map->dev->driver->name`**（不是设备名！实测 `sensor_char`） | `regmap-debugfs.c:606`、`:34-56` |
| `range` | 总是 | 可连续访问的寄存器区间列表 | `regmap-debugfs.c:609`、`:360` |
| `registers` | `max_register` 非 0 | 逐寄存器 dump，`%x: %0*val` | `regmap-debugfs.c:623`、`:284-294` |
| `access` | 需要 `max_register` 或 reg0 可读 | `y/n` 四列：readable/writeable/volatile/precious | `regmap-debugfs.c:626`、`:434-457` |
| `cache_only`/`cache_dirty`/`cache_bypass` | 仅当 `cache_type != 0` | 缓存调试（本项目无） | `regmap-debugfs.c:629-636` |

两个很实用的细节：

1. **`registers` 是逐寄存器真的去读硬件的**（`regmap_read(map, i, &val)`，
   `regmap-debugfs.c:254`），读失败就打印 `X` 填充（`:255-257`）。
   实测：注入故障后 `cat registers` 输出 `0: XXXX / 1: XXXX / 2: XXXX`
   （`logs/20260914-040819-zz-kb-probe.log`）—— **这个文件本身就是"总线通不通"的探测器**。
2. `registers` 默认 `0400`（只读）；只有内核编译时手工定义
   `REGMAP_ALLOW_WRITE_DEBUGFS` 才会变成 `0600`（`regmap-debugfs.c:616-621`）。
   所以**不能**用写 debugfs 的方式改寄存器，这是有意为之的安全设计。

---

## 4. `platform_driver` 与 `i2c_driver`

### 4.1 区别

| 维度 | `platform_driver` | `i2c_driver` |
|---|---|---|
| 定义 | `include/linux/platform_device.h:231` | `include/linux/i2c.h:271` |
| probe 参数 | `struct platform_device *` | `struct i2c_client *` |
| 挂在哪条总线 | `platform_bus_type` | `i2c_bus_type` |
| match 回调 | `platform_match()`（`drivers/base/platform.c:1305`）：OF → ACPI → `id_table` → **名字字符串比较** | `i2c_device_match()`（`drivers/i2c/i2c-core-base.c:140`）：**OF → ACPI → `id_table`（无名字比较兜底）** |
| 设备从哪来 | 设备树节点（"简单设备"，无总线语义）、`platform_device_register*()` | i2c 核心枚举（DT 子节点 / ACPI / `i2c_register_board_info` / sysfs） |
| 地址语义 | 通常用 `reg` 表示 MMIO 地址/`reg-names` | `reg` = **7 位从机地址**（`drivers/i2c/i2c-core-of.c:35-46`） |
| 典型使用者 | SoC 内部控制器、无总线从属关系的设备 | 挂在 I2C 总线上的芯片 |

**最本质的区别**：`platform_driver` 的匹配兜底是
`strcmp(pdev->name, drv->name) == 0`（`drivers/base/platform.c:1329`），
而 `i2c_driver` **没有**这一条 —— `i2c_device_match()` 只认
`of_match_table` / ACPI / `id_table`（`drivers/i2c/i2c-core-base.c:140-160`）。
这解释了 §2.5 的实测：手写 `new_device kbprobe 0x4a` 创建出来的 client
名字既不在 `sensor_id[]` 里也没有 of_node，于是**永远不会被绑定**。

### 4.2 选择依据（决策树，可以直接背）

```
这块硬件是不是"挂在 I2C 总线上的从设备"（有自己的 7 位地址）？
├─ 是 → 用 i2c_driver，让 i2c 核心去枚举/匹配；地址从 device tree 的 reg 得到
│        （如果芯片还带 SPI 接口，就在 probe 里按 client->dev 的总线类型分流，
│          或者写成"core + i2c 薄层"两层，i2c 层只负责 probe 与 regmap 后端）
└─ 否 → 它是不是 SoC 内部、有 MMIO 寄存器/IRQ/clock 的"控制器"？
    ├─ 是 → platform_driver（本项目 virt_i2c.c 就属于这一类：它模拟的是控制器，
    │        它自己不是从设备，所以是 platform_driver）
    └─ 否 → 看有没有更专门的子系统总线：
            SPI → spi_driver、USB → usb_driver、PCI → pci_driver、IIO → iio_dev …
```

**本项目的两个驱动正好是这两种角色的教科书例子**：

| 文件 | 角色 | 驱动类型 | 为什么 |
|---|---|---|---|
| `driver/virt_i2c.c` | I2C **控制器**（+ 芯片模拟） | `platform_driver`（`:376`，`module_platform_driver` `:384`） | 它是 SoC/板级器件，没有"从机地址"这种从属语义；它出现在设备树根节点下的 `virt-i2c` 节点里 |
| `driver/sensor_char.c` | I2C **从设备驱动**（温度传感器） | `i2c_driver`（`:718`） | 它在 `virt-i2c` 的**子节点** `sensor@48`，`reg = <0x48>` 就是从机地址 |

> 追问点："既然 `virt_i2c` 也能被当成 platform 设备，为什么不让 `sensor_char` 继续用
> platform_driver，自己在 probe 里 `i2c_get_adapter()`？" —— 见 §2.5 的四条理由；
> 一句话：那样做等于**在驱动里重写一遍 i2c 核心的枚举+匹配逻辑**，且拿不到 driver core
> 的生命周期与 PM 能力。

### 4.3 `of_match_table` 与 `i2c_device_id` 各管什么

```c
/* driver/sensor_char.c:706 */
static const struct of_device_id sensor_of_match[] = {
	{ .compatible = "lucien,sensor-char" },
	{ }
};
MODULE_DEVICE_TABLE(of, sensor_of_match);          /* :710 */

/* driver/sensor_char.c:712 */
static const struct i2c_device_id sensor_id[] = {
	{ "sensor_char", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sensor_id);               /* :716 */

/* driver/sensor_char.c:718 */
static struct i2c_driver sensor_i2c_driver = {
	.driver = { .name = DRV_NAME, .of_match_table = sensor_of_match },
	.probe = sensor_probe,
	.remove = sensor_remove,
	.id_table = sensor_id,
};
```

| 表 | 结构体 | 匹配字段 | 谁在什么时候用它 | 本项目实测证据 |
|---|---|---|---|---|
| `of_match_table` | `struct of_device_id`（`include/linux/mod_devicetable.h:282`） | `.compatible` | `i2c_device_match()` 第一步（`drivers/i2c/i2c-core-base.c:147`）→ `i2c_of_match_device()`（`drivers/i2c/i2c-core-of.c:146`）→ `of_match_device()` | `/sys/bus/i2c/devices/0-0048/name` == `sensor-char`（取自 compatible），并且 `driver` 链接指向 `sensor_char` |
| `id_table` | `struct i2c_device_id`（`include/linux/mod_devicetable.h:478`） | `.name` 字符串 | `i2c_device_match()` 最后一步（`drivers/i2c/i2c-core-base.c:157`）→ `i2c_match_id()`（`:106`） | 手工 `new_device kbprobe 0x4a` 名字不匹配 → 无绑定（§2.5 实测） |

三条容易答错的知识点：

1. **`i2c_device_match()` 里 OF 优先**（`:143` 在 `:157` 之前），ACPI 居中（`:148`）。
   所以 DT 系统里 `id_table` 常常"看起来没用"。
2. **`id_table` 本身可以被省略** —— 前提是有 OF 或 ACPI 表：
   ```c
   /* drivers/i2c/i2c-core-base.c:536 */
   /* An I2C ID table is not mandatory, if and only if, a suitable OF
    * or ACPI ID table is supplied for the probing device. */
   if (!driver->id_table &&
       !acpi_driver_match_device(dev, dev->driver) &&
       !i2c_of_match_device(dev->driver->of_match_table, client)) {
   	status = -ENODEV;
   	goto put_sync_adapter;
   }
   ```
   （`drivers/i2c/i2c-core-base.c:537-544`）本项目**两个表都留着**：`id_table` 是
   非 DT 场景（老平台、`new_device` 手工实例化时用驱动名 `sensor_char`）的兜底，
   另外它还能携带 `driver_data`（本项目传 0，没用到）。
3. **两张表的名字不要求一致**：`compatible = "lucien,sensor-char"` 与
   `id_table` 的 `"sensor_char"` 是两套命名空间，各自独立匹配。这也是
   `dev_name` 打印 `0-0048`、`client->name` 打印 `sensor-char`、
   driver 名打印 `sensor_char` 三个名字并存的原因。

**`i2c_of_match_device()` 还有个"sysfs 兜底"**（`drivers/i2c/i2c-core-of.c:117-160`）：
先 `of_match_device()`（需要真的 of_node），失败后再 `i2c_of_match_device_sysfs()`，
用 `client->name` 去比 `compatible` 全串或逗号后的部分：

```c
/* drivers/i2c/i2c-core-of.c:129-141 */
if (sysfs_streq(client->name, matches->compatible))
	return matches;
name = strchr(matches->compatible, ',');
if (!name) name = matches->compatible; else name++;
if (sysfs_streq(client->name, name))
	return matches;
```

这段就是"用 `new_device sensor-char 0x4a` 手工实例化后仍能被 OF 表匹配上"的原因：
`client->name` == `sensor-char` == compatible 逗号后的部分。本项目**未构造该实验**（未验证）。

### 4.4 `MODULE_DEVICE_TABLE` 的作用

```c
/* include/linux/module.h:245 */
#define MODULE_DEVICE_TABLE(type, name)					\
extern typeof(name) __mod_##type##__##name##_device_table		\
	__attribute__ ((unused, alias(__stringify(name))))
```

它只做一件事：**给表起一个固定名字的外部符号，让编译期的 `modpost` 能找到它**。
`scripts/mod/file2alias.c` 再把它翻译成 `MODULE_ALIAS()`：

- OF：`do_of_entry_multi()` 生成 `of:N<name>T<type>C<compatible>` 形式的 alias
  （`scripts/mod/file2alias.c:356-381`，`sprintf(alias, "of:N%sT%s", ...)` 在 `:366`）；
- i2c：`do_i2c_entry()`（`scripts/mod/file2alias.c:933`，`{ "i2c", SIZE_i2c_device_id, do_i2c_entry }`
  在 `:1528`）生成 `i2c:<name>`。

**实测（虚拟机里 `modinfo ~/lab/driver/sensor_char.ko`）**：

```
alias:          of:N*T*Clucien,sensor-charC*
alias:          of:N*T*Clucien,sensor-char
alias:          i2c:sensor_char
name:           sensor_char
```

对应关系：
| 表 | 生成的 alias | 与谁匹配 |
|---|---|---|
| `sensor_of_match[]` | `of:N*T*Clucien,sensor-char` | 设备 `uevent` 里的 `MODALIAS=of:NsensorT(null)Clucien,sensor-char`（§2.4 实测） |
| `sensor_id[]` | `i2c:sensor_char` | 无 of_node 的 client 名匹配场景 |

**它和"模块自动加载"的关系（面试必答，三层）**：

1. **它自己不加载任何东西**。它只把 alias 写进模块文件/`modules.alias`。
2. **内核发现新设备**时，`struct device` 通过总线 uevent 把 `MODALIAS` 报给用户态
   （i2c 的 uevent 里由 `of_device_uevent_modalias()` 填，
   `drivers/i2c/i2c-core-base.c:168`、`drivers/of/device.c:285`）。
3. **用户态 udev 拿 `MODALIAS` 去匹配 `/lib/modules/$(uname -r)/modules.alias`**，
   命中就 `modprobe` 对应模块；模块加载后由 `i2c_add_driver()` 触发匹配 → probe。

**本项目为什么看不到自动加载**：initramfs 里没有 `modules.dep`/`modules.alias`
（未打包 modprobe 数据库），模块由 `/etc/modules.load` 里 `insmod` 手动加载
（`tests/runner/init.sh` 的加载循环、`scripts/13-vm-fast-cycle.sh` 生成 `/etc/modules.load`）。
`limactl shell dev` 里 `modinfo` 能看到 alias，证明 **alias 被正确导出了**，
只是没有用户态程序去用它。

**没有 `MODULE_DEVICE_TABLE` 会怎样**：模块能手动 `insmod`（`i2c_add_driver` 照样匹配），
但**热插拔/需要时才加载**的场景会失效 —— 设备出现时 udev 找不到该模块。
这是"内建（=y）时无影响、模块（=m）+ 真实发行版时才有感知"的典型差异，值得说清楚。

---

## 5. 本项目设计取舍

### 5.1 为什么必须自己写一个虚拟 I2C 控制器（`i2c-stub` 为什么不够）

**前提事实**：QEMU 的 `virt` 机器**没有** I2C 控制器。实测：反编译 `artifacts/virt-sensor.dts`，
除我们自己插进去的 `virt_i2c: virt-i2c { ... }`（`artifacts/virt-sensor.dts:399`）之外，
**没有任何 `i2c`/`smbus` 节点**。没有控制器 → 没有 `i2c_adapter` → 从设备无处依附。

`i2c-stub` 能造出一条"总线"，但不够，三条硬伤（都能在源码里指出）：

| # | i2c-stub 的事实 | 源码位置 | 对本项目的后果 |
|---|---|---|---|
| 1 | 它是一个普通模块里的**静态 adapter**，从未设置 `of_node`（全文没有 `of_node` 字样） | `drivers/i2c/i2c-stub.c:314-320`（`static struct i2c_adapter stub_adapter`）、`:396`（`i2c_add_adapter`） | `of_i2c_register_devices()` 在 `drivers/i2c/i2c-core-of.c:91` **直接 return** → 设备树子节点永远不会被枚举成 client → **本阶段的核心目标（DT → i2c_driver probe）无法验证** |
| 2 | 它模拟的是"数组"，不是"芯片"：寄存器是 `u16 words[256]`，读回来就是你上次写进去的东西 | `drivers/i2c/i2c-stub.c:69-72`（`struct stub_chip`）、`:195-207`（`I2C_SMBUS_WORD_DATA` 分支） | 温度永远不变 → 无法断言"样本序号递增""温度落在 24~26 ℃""poll 能唤醒"等**动态行为** |
| 3 | 没有故障注入/统计，能力位图是模块参数写死的 | `drivers/i2c/i2c-stub.c:24-26`（`STUB_FUNC_DEFAULT`）、`:40-42`（`functionality` 模块参数）、`:311`（`smbus_xfer = stub_xfer`） | 错误处理代码路径（`regmap_read()` 失败 → `i2c_errors++`）**永远执行不到** → 见 §5.3 |

**改造前 vs 改造后的实测对照**（这是最有说服力的一段）：

```text
【改造前】logs/20260914-025014-01-io-models.log
--- insmod i2c-stub.ko chip_addr=0x48 ---
[    0.792833] i2c-stub: Virtual chip at 0x48
--- insmod sensor_char.ko ---
[    0.809626] sensor_char sensor-char: DT: i2c-bus=0 sensor-addr=0x48 interval=500ms   ← 驱动自己解析私有属性
[    0.810169] sensor_char sensor-char: i2c client on bus 0 addr 0x48                  ← 驱动自己造 client
[    1.288814] i2c 0-0048: opened (count=1)                                            ← 无 i2c_driver 绑定

【改造后】logs/20260914-040819-zz-kb-probe.log
--- insmod virt_i2c.ko ---
[    0.815258] virt_i2c virt-i2c: registered i2c adapter i2c-0, of_node=/virt-i2c (sub-devices enumerated from DT)
--- insmod sensor_char.ko ---
[    0.831570] sensor_char 0-0048: probe: i2c client addr=0x48 interval=500ms           ← i2c 核心枚举后调用 probe
```

**`virt_i2c` 提供的、`i2c-stub` 给不了的四样东西**：

1. **带 `of_node` 的 adapter**（`:337`）→ 打通设备树枚举；
2. **芯片级语义**：温度寄存器每次读自增并换算（`virt_i2c_convert()`，`driver/virt_i2c.c:112-126`）、
   只读寄存器拒写并计数 `ro_writes`（`:180-186,194-198`）、越界寄存器返回 `-EIO`
   （`:154-156`）、不存在的地址返回 `-ENXIO`（`:147-152`）；
3. **确定性数据源**（见 §5.2）；
4. **故障注入 + 统计 debugfs**（`:345-348`），这是 §5.3 的前提。

### 5.2 `virt_i2c` 如何做到"芯片行为确定性可复现"

```c
/* driver/virt_i2c.c:112 */
static void virt_i2c_convert(struct virt_i2c_bank *bank)
{
	bank->samples++;
	milli = TEMP_BASE_MILLI +
		((s32)(bank->samples % TEMP_PERIOD) - (TEMP_PERIOD / 2)) * TEMP_STEP_MILLI;
	bank->regs[VREG_TEMP] = MILLI_TO_RAW(milli);
	bank->regs[VREG_HUMIDITY] = (u16)(4000 + (bank->samples % 200));
}
```

| 设计点 | 常量/公式 | 为什么这么定 |
|---|---|---|
| 数据源 | **计数器**，不是随机数（`:114` `bank->samples++`） | 同一序列每次启动完全一致 → 失败可复现；随机数会让"温度落在区间内"这类断言变成抽奖 |
| 温度范围 | `TEMP_BASE_MILLI=25000`，`TEMP_STEP_MILLI=100`，`TEMP_PERIOD=21`（`:76-79`） | **24.0 ~ 26.0 ℃**：贴近室温，且**必须**让"字节序 bug"越界暴露（错值时是 2048~2560 ℃，见 §3.3.3）—— 如果模拟值本身就很大，错值就落在"合理区间"里，测试会漏掉 |
| 原始值编码 | `MILLI_TO_RAW(m) = (u16)(s16)(m*16/1000)`（`:82`），LSB = 1/16 ℃ | 贴近真实传感器（TMP105/SHT 都是定点编码）；同时让"符号位"这件事在换算里有意义 |
| 湿度 | 与温度**不同周期**（`%200` vs `%21`，`driver/virt_i2c.c:122`） | 防止两个寄存器"永远同步变化"，掩盖"读错寄存器"的 bug |
| 每个地址一套寄存器 | `banks[VBANK_MAX]`，地址 `0x48~0x4b`（`:72-74`、`struct virt_i2c_bank` `:90-94`） | 一条总线挂多颗同型号芯片：`0x48` 给字符设备版、`0x49` 留给阶段 05 的 IIO 版，互不干扰 |

**"每次读温度才转换"这个设计也有代价**：转换发生在 `smbus_xfer` 里读 `VREG_TEMP` 时
（`driver/virt_i2c.c:177-178`（Byte Data 分支）与 `:191-192`（Word Data 分支）），所以"样本序号"实际等于"温度寄存器被读的次数"。
这解释了实测数据里 `samples=22` 但 `irq=27` 的差异（`logs/20260914-040851-zz-kb-probe.log`）。
把账算平：27 次中断里有 **6 次**因故障注入失败、没走到转换，成功 21 次；
再加上该次运行开头 `cat .../registers` 那次 dump 里读过一次 `0x00`（+1 次转换），
`21 + 1 = 22 == samples`。同一个日志里的 `transfers=32` 也能对平：
probe 读 CONFIG(1) + 写 CONFIG(1) + debugfs dump(3) + 中断里的 27 次 = **32**。
**这类"虚拟硬件的隐式状态"必须写进文档**，
否则测试断言会写成不成立的等式。

### 5.3 故障注入为什么是"验证错误处理的唯一办法"

**命题**：错误处理代码只有**真的发生错误**才会执行。一个永不失败的虚拟芯片，
等于把驱动里所有 `if (ret) { ... }` 分支标记成"未测试代码"。
覆盖率工具能告诉你"这行没执行"，但只有注入能告诉你"执行了之后会发生什么、
恢复路径对不对、会不会死锁/泄漏"。

**本项目的注入实现**：

```c
/* driver/virt_i2c.c:165-172（在 mutex 内递减，保证并发注入计数精确） */
if (chip->injected_left) {
	chip->injected_left--;
	chip->errors++;
	mutex_unlock(&chip->lock);
	dev_info(chip->dev, "injected error: addr=0x%02x reg=0x%02x (left=%u)\n", ...);
	return -EIO;
}
```

**实测证据链**（`logs/20260914-040851-zz-kb-probe.log`）：注入 6 次，等 4 秒
（当时周期 200ms → 约 20 次中断），然后读统计：

```
[    3.000526] virt_i2c virt-i2c: fault injection armed: next 6 transfer(s) will fail
[    3.171351] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=5)
[    3.172052] sensor_char 0-0048: regmap read failed: -5        ← 驱动错误分支被执行
...
[    4.168396] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=0)
[    4.169026] sensor_char 0-0048: regmap read failed: -5
--- 注入前 --- [STAT] open=2 read=6 irq=7  i2c_err=0 interval=200
--- 注入后 --- [STAT] open=3 read=6 irq=27 i2c_err=6 interval=200
```

四条结论：
1. **注入次数精确**：`injected_left` 从 6 递减到 0，恰好 6 条注入日志；
2. **错误真的流到了驱动**：6 条 `regmap read failed: -5` 由
   `driver/sensor_char.c:203` 的 `dev_warn_ratelimited` 打出来，`-5 == -EIO`；
3. **计数器真的累加**：`i2c_errors` 0 → **6**，与注入次数一致（`driver/sensor_char.c:202`）；
4. **系统没有崩**：注入结束后 `irq` 继续从 7 涨到 27，`sensor_test` 退出码 0、
   dmesg 无 `WARNING:`（`logs/20260914-040921-02-i2c-driver.log` 的告警检查为 0）——
   证明"读失败时不应答、不死锁、继续跑"这条恢复路径是对的。

**另一个用途：让 regmap/芯片的"软约束 vs 硬约束"显形**。
同一个 debugfs 文件在两个健康度下表现不同：

```
注入前:  cat /sys/kernel/debug/regmap/0-0048/registers → 0: 0181  1: 0fa1  2: 0000
注入后:  cat /sys/kernel/debug/regmap/0-0048/registers → 0: XXXX  1: XXXX  2: XXXX
```
（`X` 由 `regmap-debugfs.c:255-257` 在 `regmap_read()` 失败时填充。）

**为什么"注入点是总线层"而不是"驱动层桩函数"**：注入点在 `virt_i2c_smbus_xfer()`
（`driver/virt_i2c.c:137`），也就是**真实硬件会失败的那一层**。
这样被验证的是完整链路：总线 → i2c 核心 → `regmap` 错误码传递 → 驱动的 `if (ret)` →
计数与日志。如果在驱动里 `#ifdef` 一个"假的失败"，验证的只是 `if` 语句本身。
（阶段 04 会用这个接口断言"注入 3 次 → `i2c_errors` 增量 ≥ 3"。）

### 5.4 已知简化与真机差异（面试时主动说出来）

| 项 | 本项目 | 真机 | 影响 |
|---|---|---|---|
| 温度编码 | **低 12 位有效**（`RAW_TO_MILLI(raw) = raw*1000/16`，`driver/sensor_char.c:83`） | TMP105 是**高 12 位有效**（低 4 位状态位），SHT30 是 CRC + 定点公式 | 只影响换算函数；驱动栈（枚举/匹配/regmap）完全不变 —— 这正是"设备树解耦"的价值演示 |
| 中断来源 | 自建 `irq_chip`+`irq_domain` + hrtimer 模拟 DRDY（`driver/sensor_char.c:152-172`） | 传感器 ALERT/DRDY 引脚接 GIC，DT 里写 `interrupts = <&gpio ...>` | `request_threaded_irq()` 与上下半部分离的代码**一字不改**；DT 里改一个属性即可 |
| 采样周期 | DT 属性 `poll-interval-ms` + hrtimer | 真实传感器是"数据就绪中断驱动"，不轮询 | 演示属性解析（`driver/sensor_char.c:597`）；真机可删 |
| 从机地址 | DT 的 `reg = <0x48>` | 同 | 无差异 |
| adapter 能力 | 只声明 SMBus Byte/Word（`driver/virt_i2c.c:212`） | SoC 控制器通常两种都实现 | 见 §1.5：能力声明决定 regmap 后端 |
| i2c 总线速率/时序 | 完全不模拟 | 100k/400k/1M、`clock-frequency` | 本项目不验证时序 |
| 芯片"不存在"的语义 | 地址不在 `0x48~0x4b` → `-ENXIO`（`driver/virt_i2c.c:147-152`） | 从机不 ACK → `-ENXIO`（i2c 核心约定） | 一致 |

---

## 6. 面试问答

> 每题都落到代码或源码行号。加粗的是"追问会打到哪"。

### Q1. i2c 驱动的 `probe()` 是怎么被调用的？

两条触发路径，最终都汇到 `driver->probe(client)`：

- **设备先出现**：`i2c_new_client_device()` → `device_register()`
  （`drivers/i2c/i2c-core-base.c:1015`）→ `device_add()`（`drivers/base/core.c:3567`）
  → `bus_probe_device()`（`drivers/base/bus.c:571`）→ `device_initial_probe()`
  （`drivers/base/dd.c:1156`）→ `__device_attach()`（`:1079`）→
  `bus_for_each_drv(__device_attach_driver)`（`:1066`）→ `driver_match_device()`
  （`drivers/base/base.h:165` → `i2c_device_match()`，`drivers/i2c/i2c-core-base.c:140`）
  → `driver_probe_device()`（`drivers/base/dd.c:903`）→ `really_probe()`（`:662`）
  → `call_driver_probe()`（`:631`）→ `dev->bus->probe(dev)`（`:635`，即
  `i2c_device_probe()`，`drivers/i2c/i2c-core-base.c:492`）→ **`driver->probe(client)`**
  （`:588`）。
- **驱动先注册**：`i2c_add_driver()`（`include/linux/i2c.h:885` →
  `i2c_register_driver()`，`drivers/i2c/i2c-core-base.c:2027`）→ `driver_register()`
  （`:2043`）→ `bus_add_driver()`（`drivers/base/bus.c:692`）→ `driver_attach()`
  （`drivers/base/dd.c:1310`）→ `__driver_attach()`（`:1235`）→ 同一个
  `driver_probe_device()`。

**追问 1：`probe` 是谁调的？** `i2c_device_probe()`（i2c 总线自己的 probe），
不是 i2c 核心的某个"枚举函数"直接调用 —— 这是 driver core 的统一入口，
所以 unwind/`devres`/PM 一次都不用重复写。
**追问 2：`probe` 里能睡吗？** 能。它在进程上下文（`__device_attach` 有 `device_lock`），
所以本项目 `probe` 里能 `regmap_read()`（`driver/sensor_char.c:625`）与 `request_threaded_irq()`。
**追问 3：本项目实测是哪条路径？** 设备先出现：`virt_i2c.ko` 先 `insmod`，
client 在 adapter 注册时被枚举；`sensor_char.ko` 的 `i2c_add_driver()` 触发匹配
（`logs/20260914-040921-02-i2c-driver.log:260-263`）。

### Q2. `smbus_xfer` 与 `master_xfer` 的区别？什么时候用哪个？

- `master_xfer(adap, msgs, num)`：**I2C 消息层**，驱动自己组合 START/地址/RW/数据/STOP
  （`include/linux/i2c.h:551`）。
- `smbus_xfer(adap, addr, flags, rw, command, size, data)`：**SMBus 事务层**，
  用固定的协议枚举（`I2C_SMBUS_BYTE_DATA`/`WORD_DATA`/`BLOCK_DATA`…）描述一次访问
  （`include/linux/i2c.h:555`）。

选择规则（全在 i2c 核心里）：
1. `i2c_smbus_*` 入口：有 `smbus_xfer` 就先用它；返回 `-EOPNOTSUPP` 且有 `master_xfer`
   则退回 `i2c_smbus_xfer_emulated()`（`drivers/i2c/i2c-core-smbus.c:578-607`）。
2. `i2c_transfer()` 入口：只认 `master_xfer`，没有就 `-EOPNOTSUPP`
   （`drivers/i2c/i2c-core-base.c:2264`）。
3. `regmap` 又在其上加一层：按 `val_bits/reg_bits` + `functionality` 位图挑后端
   （`drivers/base/regmap/regmap-i2c.c:306-344`）。

**本项目**：只实现 `smbus_xfer`（`driver/virt_i2c.c:137,223`），因为"寄存器 + Word/Byte"
正好是 SMBus 语义；代价是不支持裸 I2C 消息。
**追问：哪些设备必须用 `master_xfer`？** 需要 ReSTART 的复合事务（先写寄存器地址再读
数据且不允许中间 STOP）、非标准长度（>32 字节 block）、SCCB 之类私有时序的设备。

### Q3. `regmap` 相比手写 `i2c_smbus_*` 好在哪？代价是什么？

好处（对应源码）：
1. **换总线不改驱动**：`regmap_init_i2c` / `regmap_init_spi` / `regmap_init_mmio`
   只是换初始化函数，`regmap_read/write` 一行不改
   （`drivers/base/regmap/regmap-i2c.c:370`，其他后端同名文件）。
2. **位宽/字节序集中处理**：`reg_bits`/`val_bits`/`val_format_endian`
   （`include/linux/regmap.h:391,396,447`）。
3. **锁**：默认 `regmap_lock_mutex`（`drivers/base/regmap/regmap.c:754`），
   `regmap_read()` 自己加锁（`:2866`）—— 本项目因此还能再省一层锁。
4. **缓存**：`cache_type` + `reg_defaults`（`include/linux/regmap.h:433,426`），
   适合"读一次、之后都用影子值"的器件（PMIC）。
5. **调试白拿**：`/sys/kernel/debug/regmap/<dev>/`（`regmap-debugfs.c:546`）。
   本项目就是靠它一眼看出字节序错的（§3.3.3）。
6. **权限表**：`rd_table/wr_table/volatile_reg`（`include/linux/regmap.h:424-432`），
   配合 `regmap_update_bits()` 做 read-modify-write。

代价：
- 多一层调用（`regmap_read` → `_regmap_read` → 后端 → `i2c_smbus_*`），
  寄存器密集访问时是可测量的开销；
- **必须正确声明 `val_bits`/`max_register`/字节序**，声明错会在**第一次读写**才炸
  （§3.2 的 `-ENOTSUPP`、§3.3 的字节交换）；
- 对"非寄存器语义"的器件（无地址的流式设备）没有意义。

**追问：`regmap_read` 能在中断里用吗？** 不能（默认 mutex，且底层 I2C 会睡）。
本项目放在**线程化下半部**（`driver/sensor_char.c:186-230`），
硬中断上半部只做 `sd->irq_count++` 并返回 `IRQ_WAKE_THREAD`（`:161-166`）。

### Q4. `regmap` 的字节序是怎么确定的？

四步（`regmap_get_val_endian()`，`drivers/base/regmap/regmap.c:638-674`）：
① `config->val_format_endian` → ② 设备树 `big-endian`/`little-endian`/`native-endian`
（`fwnode_property_read_bool`，`:651-658`）→ ③ `regmap_bus.val_format_endian_default`
→ ④ **兜底 `REGMAP_ENDIAN_BIG`**（`:673`）。

本项目**踩坑原因**：只写了 ①②③ 都为默认 → 取 BIG → 走 `regmap_smbus_word_swapped`
（`drivers/base/regmap/regmap-i2c.c:331`）→ `i2c_smbus_read_word_swapped()`
无条件 `swab16()`（`include/linux/i2c.h:163-168`）→ 温度读成 2224.062 ℃
（实测 `logs/20260914-040851-zz-kb-probe.log`）。
修法：`driver/sensor_char.c:584` 显式 `.val_format_endian = REGMAP_ENDIAN_LITTLE`
（或 DT 加 `little-endian;`）。
**追问：为什么 SMBus word 是小端，却还要 `_swapped` 函数？** 因为 SMBus 规范定义
word 是"低字节先传"，但**有些芯片实际是反的**，内核于是提供 `_swapped` 便利函数
（`Documentation/i2c/smbus-protocol.rst:117-123`）；regmap 把这个"芯片反序"的开关
做成了字节序配置。

### Q5. i2c client 由谁创建？有哪几种创建方式？

只有**一个**函数真正创建 client：`i2c_new_client_device()`
（`drivers/i2c/i2c-core-base.c:957`）。四种入口：
1. **设备树**：`of_i2c_register_devices()`（`drivers/i2c/i2c-core-of.c:85`）→
   `of_i2c_register_device()`（`:65`）→ `i2c_new_client_device()`（`:78`），
   由 `i2c_register_adapter()` 在 adapter 注册时调用（`drivers/i2c/i2c-core-base.c:1593`）。
   **本项目采用**。
2. **ACPI**：`i2c_acpi_register_devices()`（`drivers/i2c/i2c-core-base.c:1595`）。
3. **board file**：`i2c_register_board_info()` 挂全局链表（`drivers/i2c/i2c-boardinfo.c:51`），
   adapter 注册时 `i2c_scan_static_board_info()` 扫描（`drivers/i2c/i2c-core-base.c:1401,1598`）。
4. **驱动自己/用户态**：`i2c_new_client_device()` 直调（`drivers/i2c/i2c-core-base.c:1295`
   是 sysfs `new_device` 的实现）。

**追问 1：client 谁释放？** 核心。`i2c_unregister_device()`（`drivers/i2c/i2c-core-base.c:1050`）
内部 `of_node_clear_flag` + `device_unregister()`；DT 枚举的 client 在 adapter 注销时
由 `i2c_do_del_adapter()` 统一清理。
**追问 2：手工创建 client 的驱动也能看到 `of_node` 吗？** 不能。`info.of_node` 为 NULL，
所以 `of_match_table` 匹配不上（实测 `0-004a` 没有 `of_node`、没有 `driver` 链接，
`logs/20260914-040819-zz-kb-probe.log`）。

### Q6. 没有设备树时怎么实例化 i2c 设备？

1. **sysfs（最快，调试用）**：`echo <name> <addr> > /sys/bus/i2c/devices/i2c-N/new_device`
   （`new_device_store()`，`drivers/i2c/i2c-core-base.c:1252`；删除用 `delete_device`，
   `:1320`）。本项目实测：`echo kbprobe 0x4a > .../new_device` → 生成 `0-004a`，
   但无 `of_node`、无驱动绑定。
2. **board file**：`i2c_register_board_info(busnum, info, len)`（`drivers/i2c/i2c-boardinfo.c:51`），
   适合老式 ARM 板级代码；缺点是"内核镜像绑死一种板子"。
3. **驱动内 `i2c_new_client_device()`**：需要驱动自己持有 adapter
   （`i2c_get_adapter()`，`drivers/i2c/i2c-core-base.c:2638`，用完 `i2c_put_adapter()`）。
   旧版 `sensor_char.c` 就是这么做的（`driver/sensor_char.c@53f1498:454,466,527`）。
4. **ACPI / `software_node`**：x86 平台或"用 C 结构体描述板子"
   （`info.swnode`，`drivers/i2c/i2c-core-base.c:1005`）。
**追问：`new_device` 创建的名字怎么写才能绑上驱动？**
`client->name` 要等于 `id_table` 里的名字（`i2c_match_id()`，
`drivers/i2c/i2c-core-base.c:106`），或等于 `compatible` 逗号后的部分
（`i2c_of_match_device_sysfs()`，`drivers/i2c/i2c-core-of.c:129-141`）。
本项目本可用 `echo sensor-char 0x4a > new_device` 绑上，但 `probe` 会因
"同一个 class/设备名只能创建一次"而失败 —— 本项目**未做该实验**（未验证）。

### Q7. `adapter.of_node` 到底影响什么？忘了设置会怎样？

影响**唯一的设备树枚举入口**。`of_i2c_register_devices()` 第一件事就是
`if (!adap->dev.of_node) return;`（`drivers/i2c/i2c-core-of.c:91`）。
忘了设置（或用 `i2c-stub` 这种没有 `of_node` 的 adapter）的后果：
- 子节点不会被枚举 → `/sys/bus/i2c/devices/` 里**只有总线没有从设备**；
- 没有 client → `sensor_char` 永远不会 `probe` → `/dev/sensor0` 不存在；
- 但**不会有任何报错**：`i2c_add_adapter()` 照样返回 0，日志只出现在
  `dev_dbg()`（`drivers/i2c/i2c-core-of.c:94`：`dev_dbg(&adap->dev, "of_i2c: walking child nodes\n")`）里。
**这就是本项目必须自己写 `virt_i2c` 而不是用 `i2c-stub` 的根本原因**
（`driver/virt_i2c.c:337` 那一行赋值是整条链路的开关）。
**追问：`of_node` 是给 adapter 的还是给 client 的？** 两个都要：
adapter 的 `of_node` 让核心**知道去哪枚举**（`i2c-core-of.c:91,100`），
client 的 `of_node` 让**驱动能读自己的属性**（`driver/sensor_char.c:597`，
`client->dev.of_node` 由 `of_node_get(info->of_node)` 填，`drivers/i2c/i2c-core-base.c:999`）。

### Q8. 驱动里什么时候该用 `platform_driver`，什么时候该用 `i2c_driver`？

一句话：**有 I2C 从机地址的芯片用 `i2c_driver`，SoC 内部的控制器/无总线语义的器件用
`platform_driver`**。判据看匹配兜底：`platform_match()` 最后会 `strcmp(pdev->name, drv->name)`
（`drivers/base/platform.c:1329`），`i2c_device_match()` 没有这一条
（`drivers/i2c/i2c-core-base.c:140-160`）—— 因为 I2C 从设备必须有"地址"这个身份，
用名字撞车匹配是危险的（同一总线上多颗同型号芯片很常见）。
本项目两个驱动就是这两种：`virt_i2c` = 控制器 = `platform_driver`
（`driver/virt_i2c.c:376`）；`sensor_char` = 从设备 = `i2c_driver`
（`driver/sensor_char.c:718`）。

### Q9. `of_match_table` 和 `i2c_device_id` 有什么不同？为什么两个都写？

| | `of_match_table` | `id_table` |
|---|---|---|
| 结构 | `of_device_id`（`include/linux/mod_devicetable.h:282`） | `i2c_device_id`（`:478`） |
| 匹配键 | `compatible`（"厂商,型号"） | `name`（裸字符串） |
| 匹配时机 | `i2c_device_match()` 第一步（`drivers/i2c/i2c-core-base.c:147`） | 最后一步（`:157`） |
| 依赖 | 需要 `of_node`（或 sysfs 名字兜底，`drivers/i2c/i2c-core-of.c:117`） | 只依赖 `client->name` |
| 可否省略 | 可以（有 ACPI 或 id_table 就行） | 可以（有 OF 或 ACPI 就行，`drivers/i2c/i2c-core-base.c:537-544`） |

两个都写的理由：**DT 是主路径，`id_table` 是非 DT 兜底 + 携带 `driver_data`**。
（`driver/sensor_char.c:706-716`。）

### Q10. `MODULE_DEVICE_TABLE` 有什么用？跟自动加载什么关系？

它把匹配表导出成别名（`include/linux/module.h:245`），由 `modpost`
（`scripts/mod/file2alias.c`）生成 `of:N*T*Clucien,sensor-char` 与 `i2c:sensor_char`
（实测 `modinfo` 输出）。自动加载链路：设备 `uevent` 带 `MODALIAS`
（`drivers/of/device.c:285`）→ udev 查 `modules.alias` → `modprobe`。
**没有它模块仍可手动 `insmod`，但"设备出现时才加载模块"会失效。**
本项目 initramfs 里没有 `modules.alias`，靠 `/etc/modules.load` 手动 `insmod`，
所以只用来"证明 alias 被正确导出"。

### Q11. `regmap` 说 0x00 寄存器"可写"，但芯片是只读的，怎么办？

两件事要分清：`regmap` 的权限（`readable_reg`/`writeable_reg`/`rd_table`/`wr_table`，
`include/linux/regmap.h:398-432`）是**软件层的自证声明**，
`regmap_writeable()` 在没配表时直接返回"允许"
（`drivers/base/regmap/regmap.c:90` 起、`return true` 在 `:101`；`regmap_readable()` 同理 `:127-144`）；
**真正拦得住的是硬件**。本项目实测 `access` 文件输出 `0: y y y n`（regmap 认为可写），
而真的去写 `0x00` 时 `virt_i2c` 返回 `-EIO` 并累加 `ro_writes`
（`driver/virt_i2c.c:180-183`）。工程做法：驱动里补 `rd_table/wr_table`
或 `.readable_reg/.writeable_reg`，让 regmap（以及它的 debugfs/cache 逻辑）
和硬件保持一致。

### Q12. 设备树里 `reg` 属性对 i2c 子节点是什么含义？怎么区分 10 位地址和从机模式？

```c
/* drivers/i2c/i2c-core-of.c:35-51 */
ret = of_property_read_u32(node, "reg", &addr);
if (addr & I2C_TEN_BIT_ADDRESS) { addr &= ~I2C_TEN_BIT_ADDRESS; info->flags |= I2C_CLIENT_TEN; }
if (addr & I2C_OWN_SLAVE_ADDRESS) { addr &= ~I2C_OWN_SLAVE_ADDRESS; info->flags |= I2C_CLIENT_SLAVE; }
info->addr = addr;
info->of_node = node;
```
即 `reg` 是**从机地址**（不是 MMIO 地址！），高位标记位用来表达 10 位地址与"本机作为从机"。
本项目 `reg = <0x48>` → `client->addr == 0x48`，实测
`probe: i2c client addr=0x48 interval=500ms`（`logs/20260914-040921-02-i2c-driver.log:260`）。
**追问：那 `#address-cells = <1>; #size-cells = <0>;` 为什么是 0？**
i2c 子节点没有"大小"概念，`reg` 只有一个 cell（地址）；
写错 `#size-cells` 会在 `of_i2c_get_board_info()` 解析 `reg` 时报错
（`drivers/i2c/i2c-core-of.c:36-39` 打印 `of_i2c: invalid reg`）。

---

## 7. 亲手验证

### 7.1 一键复现（推荐，与 `docs/10-开发与验证守则.md` 一致）

```bash
cd /Users/lucien/workspace/self-study/projects/linux-char-driver

# ① 构建模块 + 用户态程序 + initramfs（约 10s）
limactl shell dev bash -c 'TEST=02-i2c-driver \
    bash /Users/lucien/workspace/self-study/projects/linux-char-driver/scripts/13-vm-fast-cycle.sh'

# ② 产物拷回宿主
bash scripts/21-macos-sync-artifacts.sh

# ③ 阶段测试（QEMU，TCG + cortex-a72）
bash scripts/22-macos-run-test.sh 02-i2c-driver 180

# ④ 回归
bash scripts/22-macos-run-test.sh smoke 180
```

**实测结果**（`logs/20260914-040921-02-i2c-driver.log`）：

```
[TEST:END] 02-i2c-driver pass=16 fail=0
内核 panic: 0    内核告警: 0
```

16 项明细（节选最有信息量的几项）：

```
[CHECK:PASS] 控制器注册日志（adapter 带 of_node）
[CHECK:PASS] 子节点 sensor@48 被实例化成 i2c 从设备
[CHECK:PASS] 从设备名来自设备树 compatible（sensor-char）
[CHECK:PASS] driver 符号链接指向 sensor_char
[CHECK:PASS] regmap 注册了 debugfs 节点（/sys/kernel/debug/regmap/*-0048）
[CHECK:PASS] 能用 regmap 读出寄存器内容
[CHECK:PASS] 设备树 poll-interval-ms 生效（读回 interval=500）
[CHECK:PASS] 温度读数落在芯片模拟区间 24.0~26.0 ℃
[CHECK:PASS] dmesg 无 WARNING/Call trace
```

### 7.2 总线与设备树拓扑（逐条命令 + 实测输出）

来自 `logs/20260914-040819-zz-kb-probe.log`：

```
# ls /sys/bus/i2c/devices/
0-0048  i2c-0
# cat /sys/bus/i2c/devices/i2c-0/name
virt_i2c
# ls -l /sys/bus/i2c/devices/i2c-0/of_node
... /sys/bus/i2c/devices/i2c-0/of_node -> ../../../../firmware/devicetree/base/virt-i2c
# cat /sys/bus/i2c/devices/0-0048/name
sensor-char
# cat /sys/bus/i2c/devices/0-0048/modalias
of:NsensorT(null)Clucien,sensor-char
# ls -l /sys/bus/i2c/devices/0-0048/of_node
... -> ../../../../../firmware/devicetree/base/virt-i2c/sensor@48
# cat /sys/bus/i2c/devices/0-0048/uevent
DRIVER=sensor_char
OF_NAME=sensor
OF_FULLNAME=/virt-i2c/sensor@48
OF_COMPATIBLE_0=lucien,sensor-char
OF_COMPATIBLE_N=1
MODALIAS=of:NsensorT(null)Clucien,sensor-char
# ls -l /sys/bus/i2c/drivers/sensor_char/
0-0048 -> ../../../../devices/platform/virt-i2c/i2c-0/0-0048
--w------- ... bind
lrwxrwxrwx ... module -> ../../../../module/sensor_char
--w------- ... uevent
--w------- ... unbind
# ls -l /sys/bus/i2c/devices/0-0048/driver
... -> ../../../../../bus/i2c/drivers/sensor_char
```

**怎么读这几行**：

- `i2c-0` 是**总线**，`0-0048` 是**从设备**；`0-0048` 的父目录路径
  `devices/platform/virt-i2c/i2c-0/` 把"platform 设备 → i2c 控制器 → 从设备"
  三层对象链在 sysfs 里完整暴露出来（`platform/virt-i2c` 就是 `pdev`）。
- `of_node` 是**符号链接**，指向 devicetree 的实时视图
  （`/sys/firmware/devicetree/base/...`），改 DT 后这里会立刻反映；
  `.../virt-i2c/sensor@48` 证明了**父子关系**（枚举的前提）。
- `driver` 符号链接存在 + `drivers/sensor_char/0-0048` 存在 = **双向绑定成立**，
  这是 `i2c_driver`（而不是手工 client）最直接的证据。

### 7.3 regmap debugfs

```
# ls /sys/kernel/debug/regmap/            →  0-0048
# ls /sys/kernel/debug/regmap/0-0048/     →  access  name  range  registers
# cat /sys/kernel/debug/regmap/0-0048/name        →  sensor_char
# cat /sys/kernel/debug/regmap/0-0048/registers
0: 0181
1: 0fa1
2: 0000
# cat /sys/kernel/debug/regmap/0-0048/access
0: y y y n
1: y y y n
2: y y y n
```

预期值校验（都能算出来，不是"看起来对"）：

| 观测 | 校验 |
|---|---|
| 目录名 `0-0048` | = `dev_name(&client->dev)`（`drivers/base/regmap/regmap-debugfs.c:606`） |
| `name` = `sensor_char` | = `map->dev->driver->name`（`regmap-debugfs.c:49`），不是设备名 |
| `0: 0181` | 芯片 `samples=1` → 温度 24100 m°C → `24100*16/1000 = 385 = 0x0181`（`driver/virt_i2c.c:112-121`） |
| `1: 0fa1` | 湿度 `4000 + (1 % 200) = 4001 = 0x0fa1`（`driver/virt_i2c.c:122`） |
| `2: 0000` | probe 里写过 `regmap_write(CONFIG, 0x0000)`（`driver/sensor_char.c:631`） |
| `y y y n` | readable/writeable/volatile/precious（`regmap-debugfs.c:443-451`），volatile=y 因为没开 cache（`regmap.c:161`） |

**注意**：`registers` 每读一次就**真的**产生 3 次 I2C 传输（`regmap-debugfs.c:254`）。
实测 `transfers` 的账：probe 读 CONFIG(1) + 写 CONFIG(1) + `registers` 3 次读(3) = **5**
（`logs/20260914-040819-zz-kb-probe.log` 的 `transfers=5`）。**这个"读 debugfs 会打总线"
的性质，可以用 `transfers` 计数来验证 regmap 是否真的在访问硬件。**

### 7.4 字节序负控实验（本文档新增，做法可照搬）

```bash
# 1) 把 driver/sensor_char.c:584 改成 REGMAP_ENDIAN_BIG
# 2) 重编 + 重跑（同 §7.1 的 ①②③）
# 3) 观察三处
```

**预期（实测值）**：`registers → 0: 8101`（芯片真实 `0x0181`）、
`chip detected: config=0x0100`（真实 `0x0001`）、`temp=2224.062 C`（真实应是 24.700 ℃）；
`tests/phases/02-i2c-driver.sh` 的"温度落在 24.0~26.0 ℃"检查项 **FAIL**。
证据：`logs/20260914-040851-zz-kb-probe.log`。

**做完必须改回 `REGMAP_ENDIAN_LITTLE` 并重跑**（本文档做完后已 `git checkout` 还原，
并重跑 `02-i2c-driver` = 16 PASS、`smoke` = 15 PASS）。

### 7.5 故障注入实验

```bash
# 基线
cat /sys/kernel/debug/virt_i2c/stats
/bin/sensor_stat
# 注入 3 次并读 regmap debugfs（3 次读正好把注入次数消耗完）
echo 3 > /sys/kernel/debug/virt_i2c/inject_error
cat /sys/kernel/debug/regmap/0-0048/registers
cat /sys/kernel/debug/virt_i2c/stats
```

**实测输出（原文）**：

```
--- 注入前 virt_i2c/stats ---
transfers=5 errors=0 injected_left=0 ro_writes=0
bank=0x48 samples=1 temp_raw=0x0181 humidity_raw=4001 config=0x0000
...
[    0.921698] virt_i2c virt-i2c: fault injection armed: next 3 transfer(s) will fail
--- 注入 3 次后读 registers ---
[    0.924748] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=2)
[    0.924908] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x01 (left=1)
[    0.925030] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x02 (left=0)
0: XXXX
1: XXXX
2: XXXX
--- 注入后 virt_i2c/stats ---
transfers=8 errors=3 injected_left=0 ro_writes=0
```

**可断言的等式**：`transfers` 5→8（+3）、`errors` 0→3、`injected_left` 3→0、
`registers` 显示 `XXXX`。

**把注入打到驱动错误处理路径**（`logs/20260914-040851-zz-kb-probe.log`）：

```bash
/bin/sensor_test                       # 顺带把周期改成 200ms
/bin/sensor_stat                       # i2c_err=0
echo 6 > /sys/kernel/debug/virt_i2c/inject_error
sleep 4                                # 4s / 200ms ≈ 20 次中断
/bin/sensor_stat                       # i2c_err=6  ★
cat /sys/kernel/debug/virt_i2c/stats   # errors=6
```

实测：`[STAT] open=3 read=6 irq=27 i2c_err=6 interval=200`，
dmesg 里 6 条 `sensor_char 0-0048: regmap read failed: -5`。

### 7.6 复现的边界（必须知道的限制）

1. **`zz-kb-probe.sh` 不在仓库里**。§7.2~§7.5 的手工命令不是 `tests/phases/` 下的
   契约脚本；要重放这些命令，需要按 `tests/phases/02-i2c-driver.sh` 的写法
   写一个临时 phase 脚本（`info`/`check_*` 函数来自 `tests/runner/lib.sh`），
   再走 §7.1 的 ①②③；**或者**在 initramfs 里塞一个可交互的 shell ——
   本项目没有这条路径（QEMU 由 `scripts/22` 非交互驱动）。
2. **QEMU 必须 `-accel tcg -cpu cortex-a72`**：`-accel hvf -cpu host` 下 ftrace 会挂死
   （`docs/10-开发与验证守则.md` 第九节）。
3. **`/dev/sensor0` 的 `major=511` 是动态分配**（`driver/sensor_char.c:524` 的
   `alloc_chrdev_region`），别把 511 写进断言。
4. **`irq` 计数 ≠ `samples` 计数**：温寄存器被读才 `samples++`
   （`driver/virt_i2c.c:177-178`），中断里 `regmap_read` 失败的那次不会自增
   （实测 `irq=27` vs `samples=22`）。
5. **`dmesg` 里的 `Unknown kernel command line parameters "test=..."` 是预期噪声**
   （`pr_warn`，不匹配 `WARNING:` 检查）——`docs/10-开发与验证守则.md` 第三节已说明。
6. **`i2c-stub.ko` 仍在 initramfs 里**（`scripts/13-vm-fast-cycle.sh` 会拷它），
   但 `driver/modules.load` 不再加载它；如果手工 `insmod i2c-stub.ko`，
   会多出一条 `i2c-1` 之类的总线，`ls /sys/bus/i2c/devices/` 的输出会变 ——
   测试脚本用 `*-0048` 通配，不受影响。

---

## 附录 A：本文引用的内核源码/符号速查

### I2C 子系统

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct i2c_algorithm` | `include/linux/i2c.h:541` | 总线访问原语集合 |
| `master_xfer` | `include/linux/i2c.h:551` | I2C 消息层 |
| `smbus_xfer` | `include/linux/i2c.h:555` | SMBus 事务层 |
| `functionality` | `include/linux/i2c.h:563` | 能力位图 |
| `i2c_check_functionality()` | `include/linux/i2c.h:911` | 能力查询 |
| `struct i2c_adapter` | `include/linux/i2c.h:719` | 一条总线 |
| `struct i2c_client` | `include/linux/i2c.h:330` | 一个从设备 |
| `struct i2c_driver` | `include/linux/i2c.h:271` | 从设备驱动 |
| `struct i2c_board_info` | `include/linux/i2c.h:424` | 实例化描述 |
| `struct i2c_device_id` | `include/linux/mod_devicetable.h:478` | `id_table` 元素 |
| `struct of_device_id` | `include/linux/mod_devicetable.h:282` | `of_match_table` 元素 |
| `i2c_add_driver()` | `include/linux/i2c.h:885` | 宏 → `i2c_register_driver` |
| `i2c_get/set_clientdata()` | `include/linux/i2c.h:369,374` | client 私有数据 |
| `i2c_get/set_adapdata()` | `include/linux/i2c.h:757,762` | adapter 私有数据 |
| `i2c_smbus_read_word_swapped()` | `include/linux/i2c.h:163` | `swab16()` 版本 |
| `i2c_register_adapter()` | `drivers/i2c/i2c-core-base.c:1509` | adapter 注册主流程 |
| `of_i2c_register_devices()` | `drivers/i2c/i2c-core-base.c:1593`（调用点）/ `drivers/i2c/i2c-core-of.c:85`（实现） | DT 枚举 |
| `i2c_scan_static_board_info()` | `drivers/i2c/i2c-core-base.c:1401` | board file 扫描 |
| `i2c_add_adapter()` | `drivers/i2c/i2c-core-base.c:1661` | 动态总线号 |
| `i2c_add_numbered_adapter()` | `drivers/i2c/i2c-core-base.c:1710` | 固定总线号 |
| `i2c_new_client_device()` | `drivers/i2c/i2c-core-base.c:957` | 唯一的 client 构造函数 |
| `i2c_dev_set_name()` | `drivers/i2c/i2c-core-base.c:873,888` | `"%d-%04x"` |
| `i2c_unregister_device()` | `drivers/i2c/i2c-core-base.c:1050` | client 释放 |
| `i2c_device_match()` | `drivers/i2c/i2c-core-base.c:140` | OF→ACPI→id_table |
| `i2c_match_id()` | `drivers/i2c/i2c-core-base.c:106` | 名字匹配 |
| `i2c_device_probe()` | `drivers/i2c/i2c-core-base.c:492` | i2c 总线 probe |
| `driver->probe(client)` | `drivers/i2c/i2c-core-base.c:588` | 调驱动 probe |
| `id_table` 可选规则 | `drivers/i2c/i2c-core-base.c:536-544` | OF/ACPI 可替代 |
| `i2c_register_driver()` | `drivers/i2c/i2c-core-base.c:2027` | 驱动注册 |
| `i2c_del_driver()` | `drivers/i2c/i2c-core-base.c:2068` | 驱动注销 |
| `__i2c_transfer()` | `drivers/i2c/i2c-core-base.c:2259` | `master_xfer` 路径（NULL 检查 `:2264`） |
| `new_device_store()` / `delete_device_store()` | `drivers/i2c/i2c-core-base.c:1252` / `:1320` | sysfs 实例化 |
| `i2c_get_adapter()` | `drivers/i2c/i2c-core-base.c:2638` | 老路子取 adapter |
| `of_i2c_get_board_info()` | `drivers/i2c/i2c-core-of.c:22` | DT 子节点 → board_info |
| `of_i2c_register_device()` | `drivers/i2c/i2c-core-of.c:65` | 单节点注册 |
| `of_i2c_register_devices()` | `drivers/i2c/i2c-core-of.c:85` | `of_node` 守卫 `:91`，遍历 `:100` |
| `i2c_of_match_device()` | `drivers/i2c/i2c-core-of.c:146` | OF 匹配 + sysfs 兜底（`:117`） |
| `of_alias_from_compatible()` | `drivers/of/base.c:1041` | `client->name` 的来源 |
| `i2c_smbus_read_byte_data()` | `drivers/i2c/i2c-core-smbus.c:137` | Byte Data 读 |
| `i2c_smbus_read_word_data()` | `drivers/i2c/i2c-core-smbus.c:177` | Word Data 读 |
| `i2c_smbus_write_word_data()` | `drivers/i2c/i2c-core-smbus.c:198` | Word Data 写 |
| `i2c_smbus_xfer_emulated()` | `drivers/i2c/i2c-core-smbus.c:322` | SMBus→I2C 模拟 |
| `i2c_smbus_xfer()` / `__i2c_smbus_xfer()` | `drivers/i2c/i2c-core-smbus.c:535` / `:553` | SMBus 入口；选择逻辑 `:577-612` |
| `i2c_register_board_info()` | `drivers/i2c/i2c-boardinfo.c:51` | board file |
| `stub_adapter` / `i2c_add_adapter` | `drivers/i2c/i2c-stub.c:314` / `:396` | i2c-stub 的 adapter（无 `of_node`） |
| `STUB_FUNC_DEFAULT` | `drivers/i2c/i2c-stub.c:24` | i2c-stub 的能力位图 |
| 四种实例化方式 | `Documentation/i2c/instantiating-devices.rst:12,107,178,229` | 方法论 |
| SMBus word 字节序 | `Documentation/i2c/smbus-protocol.rst:117-123` | 协议 + `_swapped` |

### driver core

| 符号 | 位置 |
|---|---|
| `struct device` / `of_node` | `include/linux/device.h:724` / `:795` |
| `dev_of_node()` | `include/linux/device.h:1119` |
| `driver_match_device()` | `drivers/base/base.h:165` |
| `device_register()` / `device_add()` | `drivers/base/core.c:3779` / `:3567` |
| `bus_probe_device()` | `drivers/base/bus.c:571` |
| `bus_add_driver()` | `drivers/base/bus.c:692` |
| `device_initial_probe()` | `drivers/base/dd.c:1156` |
| `__device_attach()` / `__device_attach_driver()` | `drivers/base/dd.c:1079` / `:1000` |
| `driver_attach()` / `__driver_attach()` | `drivers/base/dd.c:1310` / `:1235` |
| `driver_probe_device()` | `drivers/base/dd.c:903` |
| `really_probe()` / `call_driver_probe()` | `drivers/base/dd.c:662` / `:631` |
| `dev->bus->probe(dev)` | `drivers/base/dd.c:635` |
| `struct platform_driver` | `include/linux/platform_device.h:231` |
| `platform_match()` | `drivers/base/platform.c:1305`（名字兜底 `:1328`） |
| `MODULE_DEVICE_TABLE` | `include/linux/module.h:245` |
| alias 生成 | `scripts/mod/file2alias.c:356`（OF）/ `:933`（i2c）/ `:1528`（表注册） |
| `of_device_uevent_modalias()` | `drivers/of/device.c:285` |

### regmap

| 符号 | 位置 |
|---|---|
| `struct regmap_config` | `include/linux/regmap.h:388` |
| `reg_bits` / `val_bits` | `include/linux/regmap.h:391` / `:396` |
| `max_register` / `ranges` | `include/linux/regmap.h:424` / `:449` |
| `cache_type` / `enum regcache_type` | `include/linux/regmap.h:433` / `:58` |
| `reg_format_endian` / `val_format_endian` | `include/linux/regmap.h:446` / `:447` |
| `devm_regmap_init_i2c()` | `include/linux/regmap.h:989` |
| `__devm_regmap_init_i2c()` | `drivers/base/regmap/regmap-i2c.c:385` |
| `regmap_get_i2c_bus()` | `drivers/base/regmap/regmap-i2c.c:306`（分支 `:313,316,321,326,339`） |
| `regmap_smbus_word` / `.._swapped` | `drivers/base/regmap/regmap-i2c.c:82` / `:118` |
| `regmap_smbus_word_read_swapped()` | `drivers/base/regmap/regmap-i2c.c:87`（`swab16` 在 `include/linux/i2c.h:167`） |
| `regmap_read()` / `_regmap_read()` | `drivers/base/regmap/regmap.c:2861` / `:2819` |
| `regmap_write()` / `_regmap_write()` | `drivers/base/regmap/regmap.c:1977` / `:1937` |
| `regmap_get_val_endian()` | `drivers/base/regmap/regmap.c:638`（兜底 BIG `:673`；fwnode `:651-658`） |
| `regmap_get_reg_endian()` | `drivers/base/regmap/regmap.c:614`（兜底 BIG `:635`） |
| 锁初始化 | `drivers/base/regmap/regmap.c:705-754` |
| `regmap_readable()` / `regmap_writeable()` / `regmap_volatile()` | `drivers/base/regmap/regmap.c:127` / `:90` / `:151` |
| `regcache_read()` / `regcache_write()` | `drivers/base/regmap/regcache.c:237` / `:268` |
| `regmap_debugfs_init()` | `drivers/base/regmap/regmap-debugfs.c:546` |
| debugfs 根 `regmap/` | `drivers/base/regmap/regmap-debugfs.c:694` |
| 目录名（`dev_name`） | `drivers/base/regmap/regmap-debugfs.c:582-586` |
| `name` 文件（driver name） | `drivers/base/regmap/regmap-debugfs.c:606` / `:48`（`:34-56` 为读函数） |
| `registers` 文件 | `drivers/base/regmap/regmap-debugfs.c:623` / `:284` |
| 失败填 `X` | `drivers/base/regmap/regmap-debugfs.c:255-257` |
| `access` 文件 | `drivers/base/regmap/regmap-debugfs.c:626` / `:434-457` |

### 本项目源码

| 位置 | 内容 |
|---|---|
| `driver/virt_i2c.c:90-94` | `struct virt_i2c_bank` |
| `driver/virt_i2c.c:112-126` | `virt_i2c_convert()` 确定性锯齿温湿度 |
| `driver/virt_i2c.c:137` | `virt_i2c_smbus_xfer()` |
| `driver/virt_i2c.c:165-172` | 故障注入 |
| `driver/virt_i2c.c:212-216` | `functionality` |
| `driver/virt_i2c.c:222-225` | `i2c_algorithm` |
| `driver/virt_i2c.c:307` | `virt_i2c_probe()` |
| `driver/virt_i2c.c:337` | `adap.dev.of_node = pdev->dev.of_node` ★ |
| `driver/virt_i2c.c:339` | `i2c_add_adapter()` |
| `driver/virt_i2c.c:345-348` | debugfs `stats` / `inject_error` |
| `driver/virt_i2c.c:370-384` | `of_match_table` + `platform_driver` |
| `driver/sensor_char.c:83` | `RAW_TO_MILLI` |
| `driver/sensor_char.c:174-183` | 硬中断上半部 |
| `driver/sensor_char.c:186-230` | 线程化下半部（`regmap_read` + 唤醒 + `kill_fasync`） |
| `driver/sensor_char.c:580-585` | `sensor_regmap_cfg`（含字节序） |
| `driver/sensor_char.c:587-682` | `sensor_probe()` |
| `driver/sensor_char.c:597` | `of_property_read_u32(np, "poll-interval-ms", ...)` |
| `driver/sensor_char.c:613` | `devm_regmap_init_i2c()` |
| `driver/sensor_char.c:625,631` | probe 里读/写配置寄存器（芯片在位探测） |
| `driver/sensor_char.c:684-698` | `sensor_remove()` |
| `driver/sensor_char.c:706-716` | 两张匹配表 + `MODULE_DEVICE_TABLE` |
| `driver/sensor_char.c:718-727` | `i2c_driver` |
| `driver/sensor_char.c:743` | `i2c_add_driver()` |
| `driver/sensor_char.c@53f1498:437-466` | 旧：解析私有属性 + 手工建 client |
| `driver/sensor_char.c@53f1498:527,546` | 旧：`i2c_put_adapter()` |
| `driver/modules.load` | `virt_i2c.ko` → `sensor_char.ko` |
| `dts/sensor-node.dts.inc` | 控制器 + `sensor@48` 子节点 |
| `tests/phases/02-i2c-driver.sh` | 阶段 02 契约检查项（16 项） |

---

## 附录 B：易错点速查

| 现象 | 根因 | 正解 |
|---|---|---|
| `regmap_read` 返回 `-ENOTSUPP` | `val_bits`/`reg_bits` 与 adapter capability 不匹配（`regmap-i2c.c:343`） | 对照 §3.2 判定表；确认 adapter 真的声明了用到的 SMBus 能力 |
| `regmap_read` 成功但数值离谱（字节互换） | `regmap_get_val_endian()` 兜底 BIG（`regmap.c:673`） | 显式 `val_format_endian` 或 DT `little-endian` |
| `/sys/bus/i2c/devices/` 只有 `i2c-N` 没有从设备 | adapter 没设 `of_node`（`i2c-core-of.c:91` 直接返回） | `adap.dev.of_node = pdev->dev.of_node` |
| 从设备有、`name` 是自定义串（如 `sensor_demo`） | 驱动自己 `i2c_new_client_device()` 造的 | 改成 DT 子节点 + `i2c_driver`，让名字来自 `compatible` |
| `/sys/.../<dev>/driver` 不存在 | `client->name` 与 `id_table`/`compatible` 都不匹配（`i2c-core-base.c:140-160`） | 对齐名字，或提供 `of_node` |
| `modprobe` 不自动加载模块 | 缺 `MODULE_DEVICE_TABLE`，或用户态没有 `modules.alias` | 补 `MODULE_DEVICE_TABLE`；确认 udev/`modules.dep` |
| 手工 `new_device` 后驱动不 probe | 手工 client 无 `of_node`，且名字不匹配 `id_table` | 用 `compatible` 逗号后的名字（sysfs 兜底 `i2c-core-of.c:129`），或直接用 DT |
| 在硬中断里调 `regmap_read` | 默认 mutex + I2C 会睡 | 放线程化下半部（`request_threaded_irq`） |
| `i2c_errors` 永远是 0 | 硬件永不失败 → 错误分支未被执行 | 用 `inject_error` 注入后断言（§5.3） |
| `smoke` 里 `i2c-stub` 相关断言失效 | 阶段 02 已不再加载 `i2c-stub` | 断言 i2c **从设备对象**而不是 stub 的存在（`tests/phases/smoke.sh` 已改） |

---

## 附录 C：本文标注"未验证"的结论清单

1. `virt_i2c` 的 adapter 调 `i2c_transfer()` 会返回 `-EOPNOTSUPP`
   —— 由 `master_xfer == NULL` 与 `drivers/i2c/i2c-core-base.c:2264` 推出，**未构造调用**。
2. 用 `echo sensor-char 0x4a > new_device`（走 `i2c_of_match_device_sysfs()` 兜底）
   能否让驱动 probe —— **未测试**；预期会因 `sensor_probe()` 里
   `alloc_chrdev_region`/`device_create("sensor%d")` 与 `0x48` 那次冲突而 probe 失败。
3. 在 DT 里用 `little-endian;` 代替 `config.val_format_endian` 修字节序
   —— 源码路径明确（`drivers/base/regmap/regmap.c:653`），**未在本项目实测**。
4. ~~2304 ℃ 未复现~~ **已解释清楚，不再是疑点**：2304.062 ℃ 是锯齿中心
   （25.000 ℃ / raw `0x0190`）的错值，与本文实测的 2224.062 ℃、独立验证的
   2096.062 ℃ 同属一条 `swab16` 直线（§3.3.3 的三点对照表）。
   若要精确断言"某个温度对应某个错值"，必须先固定采样格点（本项目用计数器生成，
   格点由 `samples` 决定，见 §5.2）。
5. `sysfs` 里 `/sys/bus/i2c/devices/0-0048/power/`、`uevent` 写 `bind`/`unbind`
   等 driver core 细节，本阶段**未逐条实测**。
6. 湿度寄存器（`0x01`）的读数正确性：仅从 `registers` 的 `1: 0fa1` 反推为
   `4000 + samples%200`，**未通过用户态通道读取验证**（阶段 05 才会用）。
