# 阶段 02 实现记录：虚拟 I2C 控制器 + 标准 i2c_driver + regmap + 设备树子节点匹配

> 对应 commit：`b55a25c`（阶段 02 全部代码与测试改动）
> 对比基线 commit：`53f1498`（升级前的完整项目）
> 本文档由「补写文档」任务产出：代码由前一个 agent 实现，父 agent 在其实施超时后只做了一行修复
> （`sensor_regmap_cfg.val_format_endian`），其余内容均为对现有实现与实测证据的记录。
> 测试与日志路径：`logs/`（QEMU 串口原始输出）、`artifacts/`（内核镜像与 initramfs）。

---

## 0. 结论与证据索引

| 测试 | 结论 | 日志文件（本项目 `logs/` 目录） |
|---|---|---|
| `02-i2c-driver` | **16 PASS / 0 FAIL** | `20260914-040846-02-i2c-driver.log`（`[TEST:END] pass=16 fail=0`，第 298 行） |
| `01-io-models`（回归） | **16 PASS / 0 FAIL** | `20260914-040851-01-io-models.log` |
| `smoke`（回归） | **15 PASS / 0 FAIL** | `20260914-040900-smoke.log` |
| 负控实验：删掉 `adapter.of_node` | **4 PASS / 10 FAIL**（测试能抓住） | `20260914-040748-02-i2c-driver.log` |
| 字节序 bug 复现（修复前） | `温度读数… FAIL -- 实测 temp=2096.062` | `20260914-032815-02-i2c-driver.log`（第 292 行） |
| 字节序修复后 | 同一条检查项 PASS | `20260914-035955-02-i2c-driver.log`（第 292 行） |

三项测试的日志均产生于**工作区与 commit `b55a25c` 完全一致**的条件下
（运行前后 `git diff` 为空；证据的有效性说明见第 6.4 节）。

产物层面的关键事实（行号取自 `20260914-040846-02-i2c-driver.log`，即阶段 02 整改前最后一次完整通过的日志；
CONFIG 相关的两行已按**整改后**实测校正，并在括号内注明新证据）：

| 事实 | 实测值 | 日志行号 |
|---|---|---|
| 虚拟控制器注册的总线号与设备树节点 | `i2c-0, of_node=/virt-i2c` | 258 |
| `probe(struct i2c_client *)` 被调用，地址来自设备树 `reg` | `probe: i2c client addr=0x48 interval=500ms` | 260 |
| probe 阶段探测芯片（读配置寄存器） | `chip detected: config=0x0001` | 261（整改后措辞：`chip detected: config=0x0001 (continuous conversion already enabled)`，`logs/20260914-104902-02-i2c-driver.log:261`） |
| i2c 从设备被枚举出来并绑定 | `/sys/bus/i2c/devices/0-0048`，`name=sensor-char` | 272 |
| regmap 调试接口读到的寄存器 | `0: 0181 1: 0fa1 2: 0001` | 280（`logs/20260914-104902-02-i2c-driver.log`；整改前该行为 `2: 0000`，见 `20260914-040846-02-i2c-driver.log:279`） |
| 设备树 `poll-interval-ms` 生效 | `[STAT] … interval=500` | 285 |
| 温度换算正确 | 检查项 PASS（24.0~26.0 ℃ 区间） | 292 |

---

## 1. 架构前后对比：为什么这么改

### 1.1 旧链路（基线 `53f1498`）

旧实现是 **platform_driver 自己造 i2c client**：

```c
/* 53f1498:driver/sensor_char.c（旧版） */
struct sensor_dev {
	…
	/* I2C */
	struct i2c_adapter	*adapter;	/* 驱动自己持有的适配器引用 */
	struct i2c_client	*client;	/* 驱动自己创建的从设备 */
	u32			sensor_addr;
	…
};
```

```c
/* 旧 probe：从自定义设备树属性取总线号与地址（53f1498:driver/sensor_char.c:437-440） */
of_property_read_u32(np, "i2c-bus", &bus);
of_property_read_u32(np, "sensor-addr", &addr);
of_property_read_u32(np, "poll-interval-ms", &interval);
dev_info(dev, "DT: i2c-bus=%u sensor-addr=0x%02x interval=%ums\n", bus, addr, interval);
…
sd->adapter = i2c_get_adapter(bus);			/* 拿到适配器 */
strscpy(info.type, "sensor_demo", I2C_NAME_SIZE);	/* 从设备名是硬编码的 */
info.addr = addr;
sd->client = i2c_new_client_device(sd->adapter, &info);	/* 手工造一个 i2c client */
```

寄存器访问是直接调 SMBus API：

```c
/* 53f1498:driver/sensor_char.c:162 */
raw = i2c_smbus_read_word_data(sd->client, SENSOR_REG_TEMP);
```

而且**"传感器数据"是驱动自己编出来的**——读出 i2c-stub 的寄存器、算一个锯齿、再写回去：

```c
/* 53f1498:driver/sensor_char.c:171-187 */
/*
 * 模拟真实传感器数据变化：
 * 实验环境的 i2c-stub 寄存器是可读写的，这里读出后小幅改动再写回，
 * 让数据在 22.000 ~ 28.000 °C 之间缓慢变化。
 * 真机请删除这段写回逻辑，直接使用 raw。
 */
next = RAW_TO_MILLI(raw);
if (sd->latest.seq == 0)
	next = 25000;
next += ((sd->latest.seq % 16) < 8) ? 100 : -100;	/* ±0.1 °C 锯齿 */
if (next > 28000)
	next = 22000;
i2c_smbus_write_word_data(sd->client, SENSOR_REG_TEMP, (s16)(next * 16 / 1000));
```

设备树里也需要两条自定义属性：

```dts
/* 53f1498:dts/sensor-node.dts.inc */
sensor_char: sensor-char {
	compatible = "lucien,sensor-char";
	i2c-bus = <0x00>;	/* 总线号是驱动自定义属性，不是设备树通用语义 */
	sensor-addr = <0x48>;	/* 从机地址也是驱动自定义属性 */
	poll-interval-ms = <0x1f4>;
};
```

这条链路的三个问题：

1. **绕过了 i2c 核心的匹配机制**。`i2c-bus`/`sensor-addr` 是项目自造的属性；
   标准做法是"从设备挂在控制器节点下、用 `reg` 描述地址"，由 i2c 核心实例化。
2. **从设备的生命周期由驱动自己背**。`i2c_get_adapter()` 拿到的是裸指针（要配对 `i2c_put_adapter()`），
   client 由驱动创建、`remove` 时也必须由驱动 `i2c_unregister_device()` 释放；
   一个环节漏掉就是资源泄漏。新实现里 client 由 i2c 核心创建与销毁，驱动完全不管。
3. **数据是假的**。驱动既当"传感器"又当"驱动"，`i2c_smbus_write_word_data` 写回寄存器的行为
   在真机上属于"不该做的事"，也让"驱动只负责换算"这一教学点被稀释。

### 1.2 新链路（阶段 02 之后）

```
① insmod virt_i2c.ko
   platform_driver 匹配 compatible = "lucien,virt-i2c"
     → virt_i2c_probe()（driver/virt_i2c.c:307）
     → chip->adap.dev.of_node = pdev->dev.of_node   （virt_i2c.c:337，关键）
     → i2c_add_adapter()                            （virt_i2c.c:339）
          → i2c 核心调用 of_i2c_register_devices(adap)
              → 遍历 /virt-i2c 的子节点，发现 sensor@48
              → i2c_new_client_device() 创建 i2c client（此时尚无驱动绑定）
② insmod sensor_char.ko
   i2c_add_driver()（driver/sensor_char.c:743）
     → i2c 核心按 of_match_table 匹配 compatible = "lucien,sensor-char"
     → sensor_probe(struct i2c_client *client)（sensor_char.c:587）
     → devm_regmap_init_i2c()（sensor_char.c:613）
     → 注册字符设备 / 中断 / 定时器
③ 采样：hrtimer → 虚拟中断 → 线程化下半部 → regmap_read() → 换算 → waitqueue 唤醒
```

实测日志（`20260914-040846-02-i2c-driver.log`）：

```
258:[    0.822239] virt_i2c virt-i2c: registered i2c adapter i2c-0, of_node=/virt-i2c (sub-devices enumerated from DT)
260:[    0.838680] sensor_char 0-0048: probe: i2c client addr=0x48 interval=500ms
261:[    0.839574] sensor_char 0-0048: chip detected: config=0x0001 -> enable continuous conversion
```

注意第 260 行的设备名前缀 **`0-0048`**：这是 i2c 核心给从设备起的标准名字（`<总线号>-<地址十六进制>`），
说明 client 真的是内核按设备树枚举出来的，而不是驱动自己 `strscpy(info.type, "sensor_demo")` 造出来的
（旧实现造的 client 名字是 `sensor_demo`，因此测试脚本可以用"名字必须是 `sensor-char`"来区分两者，
见 `tests/phases/02-i2c-driver.sh:24-34`）。

### 1.3 差异对照表

| 维度 | 旧（`53f1498`） | 新（`b55a25c`） |
|---|---|---|
| 驱动类型 | `platform_driver` | `i2c_driver` |
| 从设备来源 | 驱动 `i2c_get_adapter()` + `i2c_new_client_device()` 手工创建 | i2c 核心按设备树子节点自动枚举 |
| 从设备名 | 硬编码 `"sensor_demo"` | 由 `compatible` 派生（`sensor-char`） |
| 从机地址来源 | 自定义属性 `sensor-addr` | 子节点标准属性 `reg = <0x48>`（填进 `client->addr`） |
| 从设备生命周期 | 驱动负责（`i2c_get_adapter` / `i2c_unregister_device` 配对） | i2c 核心负责 |
| 寄存器访问 | 裸 `i2c_smbus_read_word_data()` | `regmap_read()`（后端 `regmap_smbus_word`） |
| 位宽/字节序 | 写死在调用点 | 由 `regmap_config` 声明（`reg_bits`/`val_bits`/`val_format_endian`） |
| 芯片行为 | 驱动读改写寄存器来"制造"数据 | `virt_i2c` 模拟芯片寄存器，驱动只读+换算 |
| 设备树 | 根节点下 `sensor-char` + 自定义属性 | `virt-i2c` 控制器节点 + `sensor@48` 子节点 |
| 故障注入 | 无 | `/sys/kernel/debug/virt_i2c/inject_error` |

### 1.4 为什么必须自己写一个虚拟 I2C 控制器

这是本阶段最关键的设计决策，理由是"内核里没有现成的替代品"：

- **QEMU `virt` 机器没有 I2C 控制器**，所以不可能直接用真机那套"控制器节点 + 子设备节点"。
- **内核自带的 `i2c-stub` 不行**。它确实能造出一条虚拟总线（`drivers/i2c/i2c-stub.c:314` 里是一个
  `static struct i2c_adapter stub_adapter`，`:396` 用 `i2c_add_adapter()` 注册），
  但**从不设置 `adap->dev.of_node`**（该文件里没有对 `of_node` 的赋值）。
  而 i2c 核心的枚举入口第一行就是这道门：

```c
/* drivers/i2c/i2c-core-of.c:85-93 */
void of_i2c_register_devices(struct i2c_adapter *adap)
{
	/* Only register child devices if the adapter has a node pointer set */
	if (!adap->dev.of_node)
		return;
	…
	/* 96-98 行：先找名为 "i2c-bus" 的子节点，否则用适配器自己的节点 */
}
```

  也就是说：**没有 `of_node`，设备树里的从设备节点永远不会被枚举**，
  "设备树描述硬件 → 内核实例化从设备 → 驱动匹配"这条链路根本无法验证。
- 自己写控制器的另一个好处：可以顺手实现 **寄存器级故障注入**，
  让驱动的错误处理路径（`regmap_read` 失败 → `i2c_errors++` → 不更新样本）
  在本阶段和阶段 04 都能被真实执行到——这是"能制造故障才能验证错误处理"的落地。

---

## 2. `driver/virt_i2c.c` 实现要点

文件：`driver/virt_i2c.c`（389 行，新增）。它是"板子上那颗 I2C 控制器 + 挂在总线上的传感器芯片"的软件替身。

### 2.1 platform_driver 与设备树匹配

```c
/* virt_i2c.c:370-384（of_match_table 与 driver 结构体） */
static const struct of_device_id virt_i2c_of_match[] = {
	{ .compatible = "lucien,virt-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, virt_i2c_of_match);

static struct platform_driver virt_i2c_driver = {
	.probe		= virt_i2c_probe,
	.remove		= virt_i2c_remove,
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= virt_i2c_of_match,
	},
};
module_platform_driver(virt_i2c_driver);
```

控制器本身**用 platform 总线**是合理的：I2C 控制器在 SoC 里就是一个挂在内存映射总线上的外设，
它自己的"从属关系"由设备树描述（对比：传感器芯片挂在 I2C 总线上、从属关系由 `reg` 描述）。
这也正好解释了 `platform_driver`（controller）与 `i2c_driver`（client）的分工不是二选一。

### 2.2 `adapter.of_node`：设备树枚举的唯一前提

```c
/* virt_i2c.c:337 */
chip->adap.dev.of_node = pdev->dev.of_node;
```

这一行的作用与证据：

- 作用：`i2c_register_adapter()` 内部会调用 `of_i2c_register_devices()`，
  而它的第一件事就是检查 `adap->dev.of_node`（见 1.4 节引用的 `i2c-core-of.c:90-92`）。
  没有这一行，`sensor@48` 永远不会变成 i2c client。
- 实测证据（正常）：日志第 258 行 `registered i2c adapter i2c-0, of_node=/virt-i2c`；
  第 272 行从设备节点 `/sys/bus/i2c/devices/0-0048` 存在。
- **负控实验**（由并行的独立验证 lane 实施，本轮最有价值的证据）：
  把 `adapter.of_node` 赋值删掉后重跑，`02-i2c-driver` 从 16 PASS 掉到 **4 PASS / 10 FAIL**
  （`logs/20260914-040748-02-i2c-driver.log`），失败项包括

  ```
  [CHECK:FAIL] 子节点 sensor@48 被实例化成 i2c 从设备 -- 值为空
  [CHECK:FAIL] driver 符号链接指向 sensor_char -- 在 /tmp/i2cdrv.txt 中未找到 'sensor_char'
  [CHECK:FAIL] probe 由 i2c 核心调用（含从机地址与 DT 周期） -- 未找到 'probe: i2c client addr=0x48 interval=500ms'
  [CHECK:FAIL] regmap 注册了 debugfs 节点 -- 值为空
  [CHECK:FAIL] sensor_test 退出码为 0 -- 期望='0' 实际='1'
  ```

  随后恢复文件、重跑回到 16 PASS（`logs/20260914-040805-02-i2c-driver.log`）。
  这说明"设备树枚举 + 驱动绑定"这条检查链**确实能发现这一行的缺失**，不是摆设。

### 2.3 SMBus 传输：为什么实现 `smbus_xfer` 而不是 `master_xfer`

```c
/* virt_i2c.c:137-144 */
static s32 virt_i2c_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			       unsigned short flags, char read_write,
			       u8 command, int size, union i2c_smbus_data *data)
{
	struct virt_i2c *chip = i2c_get_adapdata(adap);
	…
```

原因链条（这是理解 regmap + i2c 分层的关键）：

1. 我们的驱动用 `reg_bits = 8`、`val_bits = 16`。
2. regmap_i2c 挑选后端时，`val_bits == 16 && reg_bits == 8` 且适配器声明了
   `I2C_FUNC_SMBUS_WORD_DATA` → 选择 SMBus word 后端（`drivers/base/regmap/regmap-i2c.c:324-336`）。
3. 该后端调用 `i2c_smbus_read_word_data()` / `i2c_smbus_write_word_data()`（`regmap-i2c.c:61, 79`），
   i2c 核心看到适配器实现了 `smbus_xfer` 就直接调用它，**不再走 `master_xfer` 去编码寄存器地址**。

因此控制器侧只要实现 `smbus_xfer` 就够了，实现量远小于 `master_xfer` 的报文拼装。

**反向的坑：不能声明 `I2C_FUNC_I2C`。**

```c
/* virt_i2c.c:214-229（阶段 02 整改后更新：已删除多声明的 I2C_FUNC_SMBUS_BYTE） */
static u32 virt_i2c_functionality(struct i2c_adapter *adap)
{
	/* 只声明确实实现的能力：无命令字节的 SMBus Byte 协议（size=I2C_SMBUS_BYTE）
	 * 在 smbus_xfer 里走 default 返回 -EOPNOTSUPP，声明了却不实现属于 over-claim；
	 * 也不能声明 I2C_FUNC_I2C（那会让 regmap 改走 raw i2c_transfer，需要 master_xfer）。 */
	return I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA;
}
```

因为后端选择的**第一条分支**就是它：

```c
/* drivers/base/regmap/regmap-i2c.c:314-315 */
if (i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
	bus = &regmap_i2c;		/* 走 raw i2c_transfer → 需要 master_xfer */
```

一旦声明了 `I2C_FUNC_I2C`，regmap 会改走 raw transfer 路径，而本实现没有 `master_xfer`，
结果就是 probe 失败。这一点写在了 `virt_i2c.c:129-134` 的注释里，属于"看功能位就知道后端选择"的典型案例。

**报文处理**（`virt_i2c.c:146-205`）：

| 情况 | 行为 | 行号 |
|---|---|---|
| 地址不在模拟范围（`<0x48` 或 `>=0x4c`） | `-ENXIO`（真实从机不会 ACK） | 146-148 |
| 该地址没有芯片（`present == false`） | `-ENXIO` | 150-152 |
| 寄存器越界（`reg >= VREG_COUNT`） | `-EIO`（从机 NACK） | 154-156 |
| 故障注入未消耗完 | 递减计数、`errors++`、`-EIO` | 165-172 |
| `I2C_SMBUS_BYTE_DATA` 读 | 读 `regs[reg] & 0xff`；读温度时先做一次"转换" | 175-179 |
| `I2C_SMBUS_BYTE_DATA` 写 | 只读寄存器（温度/湿度）→ `ro_writes++` 并 `-EIO`；否则改低字节 | 180-187 |
| `I2C_SMBUS_WORD_DATA` 读 | 读整个 16 位寄存器值 | 189-193 |
| `I2C_SMBUS_WORD_DATA` 写 | 只读寄存器 → `-EIO`；否则整字覆盖 | 194-199 |
| 其它 size | `-EOPNOTSUPP` | 202-203 |

### 2.4 芯片寄存器图与"必须确定性"的数据

```
/* virt_i2c.c:26-34 的头注释 */
0x00  TEMP      只读，16 位；数值 = 毫摄氏度 * 16 / 1000（LSB = 1/16 ℃）
0x01  HUMIDITY  只读，16 位；数值 = %RH * 100（LSB = 0.01 %RH）
0x02  CONFIG    读写，16 位；bit0=连续转换使能：1=使能，0=关闭
                （模拟值，行为上只做记录；语义必须三处一致：virt_i2c 头注释、
                  probe 里的上电默认值、sensor_char 的日志与写入值）
```

> **阶段 02 整改后更新**：上面是整改后的头注释（`virt_i2c.c:30-32`）。
> 整改前只写了「bit0=连续转换使能（模拟值，行为上只做记录）」，
> 没有明确 1/0 的含义，也没要求三处一致，直接导致了 §7.1 记录的语义矛盾。

寄存器宏定义在 `virt_i2c.c:66-70`（`VREG_TEMP/VREG_HUMIDITY/VREG_CONFIG/VREG_COUNT`）。

**按从机地址分 bank**（`virt_i2c.c:73-74`、`90-94`、`96-106`）：`0x48` 给字符设备驱动，
`0x49` 留给阶段 05 的 IIO 驱动（两个驱动各自绑定一颗"芯片"，互不抢占同一从设备）。

```c
/* virt_i2c.c:112-123 */
static void virt_i2c_convert(struct virt_i2c_bank *bank)
{
	s32 milli;

	bank->samples++;
	milli = TEMP_BASE_MILLI +
		((s32)(bank->samples % TEMP_PERIOD) - (TEMP_PERIOD / 2)) * TEMP_STEP_MILLI;
	bank->regs[VREG_TEMP] = MILLI_TO_RAW(milli);

	/* 湿度：40.00 ~ 41.99 %RH，与温度用不同的周期，避免两者完全同步 */
	bank->regs[VREG_HUMIDITY] = (u16)(4000 + (bank->samples % 200));
}
```

参数（`virt_i2c.c:77-82`）：`TEMP_BASE_MILLI=25000`、`TEMP_STEP_MILLI=100`、`TEMP_PERIOD=21`
→ 温度在 **24.0 ~ 26.0 ℃** 之间锯齿漂移；`MILLI_TO_RAW(m) = (u16)(s16)(m * 16 / 1000)`
（LSB = 1/16 ℃ = 62.5 m℃）。

**为什么必须确定性、不能用随机数**：测试脚本要断言"温度落在 24.0~26.0 ℃"、
"样本序号递增"。如果芯片返回随机数，失败就**不可复现**——
今天红的检查项明天可能自己变绿，验证结论失去意义。工程上等价的原则是：
**测试用的假数据源必须可复现**，随机性只能出现在被测逻辑的输入边界之外（比如 fuzz）。

顺带一个可验证的细节：因为 LSB 是 62.5 m℃，驱动读回的毫摄氏度不是芯片内部那个"理想的 100 m℃ 步进"，
而是量化后的值（例如 24200 m℃ → raw 387 → 读回 24187.5 m℃）。这也是为什么
`temp` 的断言写成**区间**而不是精确值——这是量化误差，不是 bug。

### 2.5 故障注入：`inject_error` 与 `stats`

```c
/* virt_i2c.c:267-289（故障注入写处理） */
static ssize_t virt_i2c_inject_write(struct file *filp, const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	…
	if (kstrtou32(kbuf, 0, &n))
		return -EINVAL;
	mutex_lock(&chip->lock);
	chip->injected_left = n;
	mutex_unlock(&chip->lock);
	dev_info(chip->dev, "fault injection armed: next %u transfer(s) will fail\n", n);
	return len;
}
```

- 语义：写 `N` → 接下来 **恰好 N 次**传输返回 `-EIO`；写 `0` 取消。
- 递减位置在**锁内**（`virt_i2c.c:165-172`）：`smbus_xfer` 可能被多个上下文并发调用，
  放锁外会出现"注入次数被多消耗/少消耗"的竞态，测试就无法断言"注入 3 次 → 错误恰好 3 次"。
- debugfs 文件在 `virt_i2c_probe()` 里创建（`virt_i2c.c:345-349`）：
  `stats`（`0444`）与 `inject_error`（`0200`，只写）。
- `stats` 输出（`virt_i2c.c:229-261`）：`transfers/errors/injected_left/ro_writes` 汇总行
  + 每个 present bank 的 `samples/temp_raw/humidity_raw/config`。
  实测该文件存在（检查项 `控制器统计接口 /sys/kernel/debug/virt_i2c/stats` PASS，
  `20260914-040846-02-i2c-driver.log` 第 268 行）；**其内容在阶段 02 没有被 dump 出来 → 见第 7 节"未观测项"**。
- 用途：本阶段只用到"接口存在"；真正用它验证错误处理（写入注入 → 观察 `i2c_errors` 增长 →
  确认驱动没崩）被安排在阶段 04。

### 2.6 卸载路径

```c
/* virt_i2c.c:360-368 附近 */
static int virt_i2c_remove(struct platform_device *pdev)
{
	struct virt_i2c *chip = platform_get_drvdata(pdev);

	debugfs_remove_recursive(chip->dbg);
	i2c_del_adapter(&chip->adap);
	dev_info(&pdev->dev, "removed\n");
	return 0;
}
```

顺序合理：先撤掉 debugfs（避免用户态还持有已释放对象），再 `i2c_del_adapter()`
（内核会先解绑其上的从设备驱动，再删除 client）。`chip` 本身是 `devm_kzalloc` 分配的，不用手工释放。

一个小知识点（代码注释里也标了）：`platform_driver.remove` 在 6.6 仍是 **返回 int** 的旧签名，
头文件里明确建议新驱动用 `.remove_new()`：

```c
/* include/linux/platform_device.h:234-243 */
	/*
	 * Traditionally the remove callback returned an int which however is
	 * ignored by the driver core. … To convert to a callback returning void,
	 * new drivers should implement .remove_new() until the conversion it done
	 * that eventually makes .remove() return void.
	 */
	int (*remove)(struct platform_device *);
	void (*remove_new)(struct platform_device *);
```

本实现选了旧签名 `.remove`（6.6 下完全合法），作为"已知技术债"记在第 7 节。

---

## 3. regmap 要点

### 3.1 `reg_bits` / `val_bits` 如何决定后端

```c
/* driver/sensor_char.c:580-585 */
static const struct regmap_config sensor_regmap_cfg = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.max_register		= SENSOR_REG_CONFIG,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
};
```

后端选择逻辑在内核里是**按顺序短路判断**的（`drivers/base/regmap/regmap-i2c.c:306-336`）：

| 顺序 | 条件 | 选中的后端 | 走的总线调用 |
|---|---|---|---|
| 1 | 适配器支持 `I2C_FUNC_I2C` | `regmap_i2c`（raw） | `i2c_transfer()`（需要 `master_xfer`）|
| 2 | `val_bits==8 && reg_bits==8` + `SMBUS_I2C_BLOCK` | `regmap_i2c_smbus_i2c_block` | 块读 |
| 3 | `val_bits==8 && reg_bits==16` + `SMBUS_I2C_BLOCK` | `…_block_reg16` | 块读 |
| 4 | `val_bits==16 && reg_bits==8` + `SMBUS_WORD_DATA` | 见 3.2 的字节序分支 | `i2c_smbus_*_word_data()` |

我们的适配器只声明了 SMBus 能力（第 2.3 节），配置是 `8/16`，所以落到第 4 条。

### 3.2 字节序坑（本项目踩过的真实 bug）

**现象**：阶段 02 首次跑通代码后，唯一失败的是温度断言：

```
[CHECK:FAIL] 温度读数落在芯片模拟区间 24.0~26.0 ℃ -- 实测 temp=2096.062
（logs/20260914-032815-02-i2c-driver.log 第 292 行）
```

**根因**：`regmap_get_val_endian()` 在没有任何地方声明字节序时**默认 BIG**：

```c
/* drivers/base/regmap/regmap.c:638-674（节选关键分支） */
	endian = config->val_format_endian;
	if (endian != REGMAP_ENDIAN_DEFAULT)
		return endian;			/* ① config 显式声明优先 */
	if (fwnode_property_read_bool(fwnode, "big-endian"))   …	/* ② 设备树属性 */
	else if (… "little-endian") …
	if (bus && bus->val_format_endian_default)		/* ③ 总线默认 */
		endian = bus->val_format_endian_default;
	…
	/* Use this if no other value was found */
	return REGMAP_ENDIAN_BIG;		/* ④ 兜底：BIG（坑就在这） */
```

我们三处都没声明 → 走 ④ → 后端被选成 `regmap_smbus_word_swapped`
（`regmap-i2c.c:118` 的 bus 结构、`:87` 的读写函数）：

```c
/* drivers/base/regmap/regmap-i2c.c:327-333 */
		switch (regmap_get_val_endian(&i2c->dev, NULL, config)) {
		case REGMAP_ENDIAN_LITTLE:
			bus = &regmap_smbus_word;	/* 不交换 */
			break;
		case REGMAP_ENDIAN_BIG:
			bus = &regmap_smbus_word_swapped;	/* swab16 */
			break;
```

而 `…_swapped` 后端调用的是**会对 16 位值做 `swab16()`** 的 i2c 核心辅助函数：

```c
/* include/linux/i2c.h:163-168 */
static inline s32
i2c_smbus_read_word_swapped(const struct i2c_client *client, u8 command)
{
	s32 value = i2c_smbus_read_word_data(client, command);

	return (value < 0) ? value : swab16(value);
}
```

我们的芯片（`virt_i2c.c:193`：`data->word = bank->regs[reg];`）是把值**原样**放进 `data->word` 的，
于是驱动的换算函数 `RAW_TO_MILLI(raw) = raw * 1000 / 16`（`sensor_char.c:83`）收到的是被交换过的数。

**算术自证**（这条能让读者彻底看懂现象，也能解释为什么不同时刻数字不一样）：

| 芯片真实温度 | 芯片寄存器 raw | 正确读数 | 被 swab16 后 | 驱动报出 |
|---|---|---|---|---|
| 24.200 ℃ | 0x0183 = 387 | 24187.5 m℃ = 24.1875 ℃ | 0x8301 = 33537 | 33537×1000/16 = **2096.06 ℃** |
| 25.000 ℃ | 0x0190 = 400 | 25000 m℃ | 0x9001 = 36865 | 36865×1000/16 = **2304.06 ℃** |

失败日志里出现的是 2096.062（当时锯齿走到 24.2 ℃ 那一格）；
`sensor_char.c:570-579` 的注释里写的是"2304 ℃ 量级"（锯齿中心 25.0 ℃ 那一格）。
**两个数字都对**，只是采样时刻不同——因为数据是确定性锯齿，读到哪个值取决于读的时刻。

**修法**：在 `regmap_config` 里显式声明

```c
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,	/* sensor_char.c:584 */
```

声明后走 `regmap_smbus_word`（`regmap-i2c.c:51-82`：`regmap_smbus_word_reg_read` →
`i2c_smbus_read_word_data()`，第 61 行），不再交换，读数立即落到 24~26 ℃
（`logs/20260914-035955-02-i2c-driver.log` 第 292 行同一条检查项 PASS）。

**读者如何复现这个坑**：删掉 `.val_format_endian` 这一行 → 重新构建 → 重跑 `02-i2c-driver`，
温度断言会再次失败，且 debugfs 里的寄存器值会变成 `0: 8101 1: a10f`（现为 `0: 0181 1: 0fa1`）。

**为什么真实驱动也必须写清楚**：regmap 的这条默认值（BIG）是为了兼容历史上"寄存器字按大端在总线上传输"的器件；
它不会替驱动做假设，也不看数据手册。真实传感器的数据手册里"16-bit, little endian"这类字样
必须由驱动翻译成 `val_format_endian`（或设备树里的 `little-endian` 属性），否则就是本项目这种
"能编译、能 probe、数据全错"的静默故障——而这类 bug 在真机上极难定位，因为寄存器读写在链路层完全成功。

### 3.3 用 debugfs 交叉验证字节序与读数

`regmap` 会为自己的 map 在 debugfs 建节点（本阶段实测存在）：

```
      · 从设备节点：/sys/bus/i2c/devices/0-0048，name=sensor-char
      · registers 内容：0: 0181 1: 0fa1 2: 0001
（logs/20260914-104902-02-i2c-driver.log 第 273、280 行；**阶段 02 整改后更新**：
 整改前同一处为 `2: 0000`，见 logs/20260914-040846-02-i2c-driver.log:279）
```

这次读数自洽，且能作为独立交叉验证：

- `reg 0`（TEMP）= `0x0181` = 385 → `385 × 1000 / 16` = **24062.5 m℃ = 24.06 ℃**，落在 24~26 ℃ 区间 ✔
- `reg 1`（HUMIDITY）= `0x0fa1` = 4001 → **40.01 %RH**，落在芯片模拟的 4000~4199 ✔
- `reg 2`（CONFIG）= `0x0001` → 与 `sensor_char.c:863` 的 `regmap_write(..., SENSOR_CFG_CONT_EN = 0x0001)` 一致 ✔
  （**阶段 02 整改后更新**：整改前此处为 `0x0000`，与代码里 `regmap_write(..., 0x0000)` 一致但语义矛盾，见 §7.1；
  整改后实测 `2: 0001`，证据 `logs/20260914-104902-02-i2c-driver.log:280`）

如果字节序还是错的，同一处会显示 `0: 8101 1: a10f`（湿度变成 412 %RH，一眼可见不合理）。
注意：`max_register = 0x02`，所以 dump 恰好是 3 个寄存器（0~2），这也反向确认了配置生效。
本驱动没有配置 cache（`regmap_config` 里没有 `cache_type`），所以该文件的读数是**真实经总线取回**的。

---

## 4. 保留与删除清单

### 4.1 删除（阶段 02 的核心动作）

| 删除项 | 原位置 | 说明 |
|---|---|---|
| `#include <linux/platform_device.h>` / `<linux/of_device.h>` | 旧头部 | 不再是平台驱动 |
| `struct sensor_dev` 里的 `struct i2c_adapter *adapter` | 旧 `struct` | 驱动不再持有适配器 |
| `of_property_read_u32(np, "i2c-bus", …)` / `"sensor-addr"` | 旧 probe | 改用标准 `reg` 属性（由 i2c 核心解析） |
| `i2c_get_adapter()` + `i2c_new_client_device()` | 旧 probe | 改由 i2c 核心枚举（第 2.2 节） |
| 裸 `i2c_smbus_read_word_data()` / `i2c_smbus_write_word_data()` | 旧下半部 | 改为 `regmap_read` / `regmap_write` |
| 驱动内"读改写寄存器制造数据"的锯齿逻辑 | 旧下半部（1.1 节引用） | 数据生成搬到芯片模拟（`virt_i2c.c`） |
| `dts` 里的 `i2c-bus` / `sensor-addr` 自定义属性 | 旧 `dts/sensor-node.dts.inc` | 改为控制器节点 + 子节点 `reg` |

校验方式（本次文档任务实测）：

```bash
$ grep -nE "i2c_get_adapter\(|i2c_new_client_device\(|i2c_smbus_(read|write)_|platform_driver_(register|unregister)" driver/sensor_char.c
# 输出全部落在注释里（第 198、554、555、566 行），没有一处真实调用
```

### 4.2 保留（不能被这次重构挤掉的能力）

| 保留项 | 位置（阶段 02 之后） | 为什么保留 |
|---|---|---|
| 字符设备注册（`cdev` + `class_create` + `device_create`） | `sensor_char.c:516-548`；probe 里的调用点在 `sensor_char.c:634` | 项目主线是"字符设备驱动"，`/dev/sensor0` 是它的门面 |
| 四种 IO 模型（per-open `last_seq`、阻塞/非阻塞 read、poll、fasync） | `struct sensor_file` `:124`；`sensor_has_new_sample` `:268`；`sensor_read` `:325`；`sensor_poll` `:381`；`sensor_fasync` `:401` | 阶段 01 的成果；阶段 02 只换了"数据从哪来"，接口语义不变（回归 `01-io-models` 16 PASS 即证明） |
| `file_operations` 全量（含 `.poll`/`.fasync`） | `sensor_char.c:502-513` | 同上 |
| 自建虚拟中断控制器（`irq_chip` + `irq_domain`） | `sensor_char.c:152-166` | 承载"中断子系统"教学点（GIC → `request_threaded_irq` 的整条路） |
| 线程化中断上下半部分离 | `sensor_irq_hard` `:174`、`sensor_irq_thread` `:186`、`request_threaded_irq` `:657` | 下半部要做会睡眠的 I2C 传输，这是标准写法 |
| 统计与 ioctl（`GET_SAMPLE`/`SET_INTERVAL`/`GET_STATS`/`RESET`） | `sensor_ioctl` `:439-500` | ABI 保持兼容（本阶段未新增字段） |
| `poll-interval-ms` 设备树属性解析 | `sensor_char.c:597` | 设备树解耦要点保留 |

### 4.3 中断为什么留在 `sensor_char.c` 而不是迁到 `virt_i2c.c`

任务书给了两种选择（a 留 / b 迁），本实现选了 **(a)**，理由：

1. **与真机形态一致**：真机上 DRDY/ALERT 引脚是**传感器芯片**的特性，
   驱动用 `request_threaded_irq()` 申请它；而"虚拟中断控制器"是本项目为了在没有真实
   中断线的情况下造出中断源而加的脚手架。它是**驱动的调试设施**，不是 I2C 控制器的职责——
   迁进 `virt_i2c` 反而会让"控制器驱动"承担本该由板级/芯片承担的语义。
2. **降低阶段的相互影响**：阶段 05（IIO）会再写一个驱动，若中断源住在 `virt_i2c` 里，
   两个驱动对中断的假设会耦合在一起；留在各自的传感器驱动里更干净。
3. 代码注释已经说明了真机对应关系（`sensor_char.c:29-34`：hrtimer → `generic_handle_domain_irq()`
   这一段在真机上就是"传感器 DRDY 引脚接到 GIC"）。

代价：`virt_i2c` 的"芯片"不会主动产生中断，只能由驱动自己的 hrtimer 触发；
这是"教学环境"与"真机"的已知差距，记在第 7 节。

---

## 5. `smoke.sh` 例外改动逐条说明

任务书为阶段 02 开了一个口子：允许修改 `tests/phases/smoke.sh` 中**与实现细节绑定**的检查项，
但不得删除任何功能性检查。下面是逐行交代（`git diff cb4436a b55a25c -- tests/phases/smoke.sh`，
共 +15 / -2 行）。

### 5.1 被替换的两行（措辞绑定旧实现）

```diff
-check_contains "设备树解析到 i2c-bus/sensor-addr" /tmp/probe.txt "DT: i2c-bus=0 sensor-addr=0x48"
-check_contains "I2C 客户端创建成功" /tmp/probe.txt "i2c client on bus 0 addr 0x48"
+check_contains "从设备树读到从机地址与采样周期" /tmp/probe.txt "probe: i2c client addr=0x48 interval=500ms"
+check_contains "芯片在位探测成功（regmap 读配置寄存器）" /tmp/probe.txt "chip detected: config="
```

为什么必须改：旧的两行 grep 的是旧实现打印的日志文本
（`DT: i2c-bus=0 sensor-addr=0x48`、`i2c client on bus 0 addr 0x48`），
这两个字符串在新实现里**已经不存在**（总线号字面量与自定义属性都删掉了）。
若不改，smoke 会因为"日志说法的变化"而红——那是**假失败**，与真实功能无关。

改后验证的是**同一事实**、且更强：

- 旧：grep 一句日志 → 只能证明"代码跑到了那一行"。
- 新：`addr=0x48 interval=500ms` 同时证明"从机地址来自设备树 `reg`"和"`poll-interval-ms` 被读到"；
  多出来的 `chip detected: config=` 证明 **probe 阶段真的通过 regmap 与芯片通信成功**
  （读不到就 probe 失败，`/dev/sensor0` 根本不会出现）——这一项旧实现没有。

### 5.2 新增的一组检查（净增 3 项）

```diff
+info "== 2b) I2C 从设备由设备树枚举并绑定到驱动 =="
+I2CDEV=$(ls -d /sys/bus/i2c/devices/*-0048 2>/dev/null | head -1)
+check_nonempty "i2c 从设备节点存在（<bus>-0048）" "$I2CDEV"
+if [ -n "$I2CDEV" ]; then
+    cat "$I2CDEV/name" > /tmp/i2cname.txt 2>/dev/null
+    check_contains "从设备名来自设备树 compatible（sensor-char）" /tmp/i2cname.txt "sensor-char"
+    readlink -f "$I2CDEV/driver" > /tmp/i2cdrv.txt 2>/dev/null
+    check_contains "从设备已绑定到 sensor_char 驱动" /tmp/i2cdrv.txt "sensor_char"
+fi
```

新增理由：阶段 02 把平台驱动换成了标准 i2c 驱动，**"内核真的建出了 i2c 从设备并绑定了驱动"
这件事本身就值得进回归集**。这三项检查的是内核对象（`/sys/bus/i2c/devices/*-0048`
与 `driver` 符号链接），而不是 printk，属于"更难被糊弄"的证据；
而且名字检查（必须是 `sensor-char` 而非旧实现的 `sensor_demo`）能识别"退回到手工实例化"。

### 5.3 结论：保护是变强还是变弱？

| | 旧 smoke | 新 smoke |
|---|---|---|
| 检查项数 | 12 | **15** |
| 设备节点存在 | ✔ | ✔ |
| probe 关键步骤（中断、字符设备注册） | ✔ | ✔ |
| 从机地址/采样周期来自设备树 | ✔（grep 旧日志文本） | ✔（更强的单条断言） |
| probe 阶段与芯片真实通信 | ✘ | ✔（新增） |
| i2c 从设备由 DT 枚举 | ✘ | ✔（新增） |
| 从设备绑定到驱动 | ✘ | ✔（新增） |
| 用户态 read/ioctl/write 通路 | ✔ | ✔ |
| ftrace 抓到线程化下半部 | ✔ | ✔ |
| 无 `WARNING`/`Call trace` | ✔ | ✔ |

**没有任何功能性检查被删除或放宽**，净增 3 项；实测 15 PASS / 0 FAIL
（`logs/20260914-040900-smoke.log`）。符合任务书的例外条件。

其余允许范围内的配套改动（不属于 smoke 例外，但同属脚本层）：

- `driver/modules.load`：`i2c-stub.ko` → `virt_i2c.ko`（配合 dts 变化，注释里说明了依赖顺序）。
- `driver/Makefile`：新增 `obj-m += virt_i2c.o`，并把 `install` 目标从写死 `sensor_char.ko`
  改成 `$(patsubst %.o,%.ko,$(obj-m))`（多模块下不再漏装）。
- `scripts/20-macos-gen-dtb.sh`：输出时改 grep `virt-i2c {` 节点（设备树结构变了，仅是打印改动）。
- 新增 `tests/userspace/sensor_stat.c`：只读统计小工具，用于断言"`poll-interval-ms` 是否真的生效"
  （`user/sensor_test.c` 中途会把周期改成 200ms，不适合断言初始值）。它不是测试脚本的一部分，
  而是被 `02-i2c-driver.sh:49` 调用的**取证工具**。

---

## 6. 验证证据

### 6.1 三项测试（阶段 + 回归）

```
02-i2c-driver : pass=16 fail=0   logs/20260914-040846-02-i2c-driver.log（第 298 行 [TEST:END]）
01-io-models  : pass=16 fail=0   logs/20260914-040851-01-io-models.log
smoke         : pass=15 fail=0   logs/20260914-040900-smoke.log
```

`02-i2c-driver` 的 16 项检查全部 PASS（`20260914-040846-02-i2c-driver.log:266-295`），
关键数值已在第 0 节表格中列出（适配器 `i2c-0` + `of_node=/virt-i2c`、
`/sys/bus/i2c/devices/0-0048`、`registers: 0:0181 1:0fa1 2:0000`、
`[STAT] … interval=500`、温度区间 PASS）。

回归的含义：阶段 01 的四模型（16 项）与基线 smoke（15 项）在整个重构后依然全绿，
说明"改设备模型"没有碰坏用户态接口语义 —— 这也是把 IO 模型测试在阶段 01 就固化的价值。

### 6.2 负控实验（证明测试不是摆设）

| 实验 | 结果 | 日志 |
|---|---|---|
| 健康基线（对照） | 16 PASS / 0 FAIL | `20260914-040731-02-i2c-driver.log` |
| 删掉 `virt_i2c.c:337`（`adap.dev.of_node` 赋值） | **4 PASS / 10 FAIL** | `20260914-040748-02-i2c-driver.log` |
| 恢复后 | 16 PASS / 0 FAIL | `20260914-040805-02-i2c-driver.log` |

用途：证明"设备树枚举 + 驱动绑定 + regmap 生效 + 字符设备注册"这一整条检查链
对 **`adapter.of_node` 这一行**是敏感的（该实验由并行的独立验证 lane 执行，
恢复后 `git status` 无残留改动，见 6.4 节）。

### 6.3 字节序 bug 的"修复前 / 修复后"对照

| 状态 | 同一条检查项 | 日志 |
|---|---|---|
| 未声明 `val_format_endian`（regmap 默认 BIG） | `FAIL -- 实测 temp=2096.062` | `20260914-032815-02-i2c-driver.log:292` |
| 声明 `REGMAP_ENDIAN_LITTLE` | `PASS` | `20260914-035955-02-i2c-driver.log:292` |

### 6.4 证据有效性说明（为什么可以引用上面的日志）

1. **工作区与 commit 一致**：引用上述日志的前后，`git diff` 均为空
   （`git status --short` 仅显示并行 lane 新建的临时探针脚本 `tests/phases/02b-…`、`02c-…`，
   没有对已跟踪文件的修改）。
2. **产物与代码一致**：`artifacts/initramfs.cpio.gz` 的 mtime 晚于
   `driver/sensor_char.c`、`driver/virt_i2c.c` 的最后修改时间；且未被重建的测试运行
   （6.1 节三次）其中温度断言 PASS —— 只有带字节序修复的版本才可能通过，
   等价于对"运行的模块确实是修复版"的独立确认。
3. **有反向对照**：同一套检查在 6.2 / 6.3 的"故意破损"状态下会失败，
   说明 6.1 的 PASS 不是因为检查项恒真。
4. **一个需要读者知道的干扰源**：本轮验证期间有**并行的独立验证 lane** 在同一个工作区
   做负控实验（临时改代码 → 重跑 → `git checkout` 恢复），因此 `logs/` 里会出现
   时间相邻但结论相反的日志（例如 `040731` PASS / `040748` FAIL / `040805` PASS）。
   这不是测试不稳定，而是**故意制造破损**的实验记录；引用时应以
   "同一日志内 `[TEST:END]` 的计数 + 该次运行的状态"为准。

---

## 7. 遗留风险与已知取舍

按"会不会咬人"排序，全部标注证据；**本轮任务只写文档、未修改任何代码**。

### 7.1 中：CONFIG 寄存器的语义/日志三处不一致 → **已在阶段 02 整改中修复（commit `730d0ff`）**

> **状态（阶段 02 整改后更新）**：下表是整改前的问题记录，保留作为分析过程；
> 三处已全部统一为「bit0=1 表示连续转换使能」：
> - `virt_i2c.c:30-32`：寄存器图写明 `bit0 = 连续转换使能：1=使能，0=关闭`，并要求三处一致；
> - `virt_i2c.c:344-345`：上电默认值注释改为「连续转换使能（bit0=1）」；
> - `sensor_char.c:855-863`：probe 先读 CONFIG 打印 `chip detected: config=0x%04x (continuous conversion already enabled|off)`，
>   再 `regmap_write(…, SENSOR_CFG_CONT_EN = 0x0001)` 并打印 `continuous conversion enabled (config=0x0001)`。
> 整改后实测：`logs/20260914-104902-02-i2c-driver.log:261-262`、同日志 `:280`（`2: 0001`）。

| 位置 | 内容 | 问题 |
|---|---|---|
| `virt_i2c.c:30` | `0x02  CONFIG … bit0=连续转换使能（模拟值，行为上只做记录）` | 定义 bit0 = **使能** |
| `virt_i2c.c:320` | `chip->banks[i].regs[VREG_CONFIG] = 0x0001;  /* 上电默认：连续转换关 */` | 0x0001 表示 bit0=1=**使能**，注释却说“关” |
| `sensor_char.c:630-631` | 打印 `chip detected: config=0x0001 -> enable continuous conversion` 后 `regmap_write(…, 0x0000)` | 日志说“enable”，紧接着把 bit0 清 0（即 disable）；意图（进入连续转换 or 单次转换）与代码相反 |

影响：`virt_i2c` 对这个寄存器**只做记录、不改变行为**（`:30` 明说），
所以功能与测试结论不受影响（整改前 debugfs 实测 `reg 2 = 0x0000`，与当时的写入一致；
**整改后为 `0x0001`**，证据 `logs/20260914-104902-02-i2c-driver.log:280`）。
但它会让读代码/读日志的人对“芯片当前处于什么模式”得出错误结论。
整改时选择的方案（原建议二选一里的第一条，已实施）：
- 进入连续转换模式：`regmap_write(…, SENSOR_CFG_CONT_EN = 0x0001)`，
  日志改为先读后打 `chip detected: config=0x%04x (continuous conversion already enabled|off)`，
  写完再打 `continuous conversion enabled (config=0x0001)`。

### 7.2 中：`smbus_xfer` 只支持 BYTE / BYTE_DATA / WORD_DATA

其它 size（`I2C_SMBUS_BLOCK_DATA`、`I2C_BLOCK_DATA`、`I2C_SMBUS_PROC_CALL` 等）返回
`-EOPNOTSUPP`（`virt_i2c.c:202-203`），适配器也相应地不声明这些 capability
（`:214-229`，阶段 02 整改后更新：已同时删除 `I2C_FUNC_SMBUS_BYTE` 这一 over-claim 的能力位）。
后果：本控制器**无法模拟需要块读的芯片**（很多 EEPROM/加速度计就是块读），
也无法用来演练"regmap 遇到不支持的操作时如何降级/报错"。
取舍：本项目的传感器只需要 word 读写，先用最小实现；扩展点很清楚（加 case 即可）。

### 7.3 低：4 个 bank 全部 `present = true`，`-ENXIO` 分支缺少测试

`virt_i2c.c:319` 把所有 bank 标成"芯片在位"（`0x48~0x4b`，其中 0x49 预留给阶段 05 的 IIO）。
因此 `-ENXIO` 分支（`addr < 0x48` 或 `>= 0x4c`，`:146-152`）在当前测试下**没有被触发**，
"不存在的从机地址 → probe 失败"这种场景只能靠 0x4c 以上的地址人为构造，而现有测试没有做。

### 7.4 低（**未观测项**）：故障注入的错误路径尚未在本阶段验证

`02-i2c-driver.sh:21-22` 只断言 debugfs 两个文件**存在**；
本阶段日志里也**没有出现过** `inject_error` 被写入后的驱动行为记录
（`grep` 三类 `logs/20260914-04*.log` 未观测到 `fault injection armed` / `injected error`）。
任务书把"注入后 `i2c_errors` 增长"的验证安排在阶段 04，因此这不算本阶段缺陷，
但**当前不能声称"错误处理路径已验证"**。
同时：本阶段也没有 dump `virt_i2c/stats` 的内容（只验证文件存在）→ **其输出格式未观测到**。

### 7.5 低：字节序策略是"硬编码"而非"设备树声明"

`regmap_get_val_endian()` 支持从 fwnode 读 `little-endian`/`big-endian`（`regmap.c:652-662`），
本实现选了在驱动里硬编码 `REGMAP_ENDIAN_LITTLE`（`sensor_char.c:584`）。
- 好处：不依赖设备树是否声明，行为确定、可复现（与本项目"实验可复现优先"一致）。
- 代价：若换成字节序相反的芯片，必须改代码而不能只改 dts；与项目"设备树解耦"的主题略有张力。
（另一种做法：dts 里加 `little-endian;` 并保持 config 默认；两种都能工作，本项目选了前者。）

### 7.6 低：`virt_i2c` 用了 6.6 的旧 `.remove` 签名

`virt_i2c.c:360` 的 `static int virt_i2c_remove(struct platform_device *pdev)` 是 6.6 仍保留的
int 返回形式；头文件明确建议新驱动使用 `.remove_new()`（`include/linux/platform_device.h:234-243`）。
6.6 下合法，但升级内核时需要改。代码里已有注释说明这是**有意为之**（保持 6.6 兼容）。

### 7.7 低：`platform driver` 不产生中断，"虚拟中断"仍由传感器驱动自造

`virt_i2c` 的芯片模拟不会主动拉中断线；采样节拍来自 `sensor_char.c` 里的 hrtimer
（`sensor_timer_fn` → `generic_handle_domain_irq`）。
这与真机（DRDY 引脚 → GIC）存在差距，属于既有设计（第 4.3 节），
代价是"中断完全由被测驱动自己产生"，无法用来验证"外部异步事件"这类场景。

### 7.8 低：regmap 未启用 cache

`sensor_regmap_cfg`（`sensor_char.c:582-585`）没有 `cache_type`，所以每次 `regmap_read`
都真实走总线。对"需要实时值"的传感器这是正确默认；但需要注意 debugfs 的 `registers`
读操作会产生**真实总线访问**（若将来加入"读清中断标志"之类的寄存器，调试 dump 会有副作用）。

### 7.9 已核查、判定为"非问题"的两点

- **非设备树实例化路径不会崩**：`sensor_probe` 里 `of_property_read_u32(np, "poll-interval-ms", …)`
  （`sensor_char.c:597`）在 `np == NULL` 时返回 `-EINVAL`——`drivers/of/base.c:192-198`
  的 `__of_find_property()` 开头就有 `if (!np) return NULL;`，
  因此 `interval` 保持默认 500ms，驱动不会空指针。保留 `id_table`（`sensor_char.c:712-716`）
  使非 DT 场景仍可绑定。
- **"`i2c_errors` 在 regmap 失败时累加"**：实现于 `sensor_char.c:201-207`
  （`regmap_read` 返回非 0 → `sd->i2c_errors++` + `dev_warn_ratelimited`），
  与任务书要求一致；但如 7.4 所述，本阶段没有制造失败来实测这条路径。

---

## 8. 复现命令（在本机直接可跑）

```bash
cd /Users/lucien/workspace/self-study/projects/linux-char-driver

# 1) 构建（源码有改动才需要；约 10 秒）
limactl shell dev bash -c 'TEST=02-i2c-driver bash /Users/lucien/workspace/self-study/projects/linux-char-driver/scripts/13-vm-fast-cycle.sh'

# 2) 同步产物到 artifacts/
bash scripts/21-macos-sync-artifacts.sh

# 3) 跑阶段测试与两项回归（各约 10~30 秒）
bash scripts/22-macos-run-test.sh 02-i2c-driver 180
bash scripts/22-macos-run-test.sh 01-io-models 180
bash scripts/22-macos-run-test.sh smoke 180

# 4) 复现"字节序坑"：注释掉 sensor_char.c:584 的 .val_format_endian → 重跑第 1~3 步，
#    02-i2c-driver 的温度断言会 FAIL（实测 temp≈2000 ℃ 量级），debugfs registers 变成 0: 8101 1: a10f
git diff driver/sensor_char.c        # 实验后确认已恢复
```

QEMU 内手工观察点（需交互模式或放进测试脚本）：

| 观察对象 | 路径/命令 | 期望 |
|---|---|---|
| 从设备是否由 DT 枚举 | `ls /sys/bus/i2c/devices/` | `0-0048` 存在，`name` 为 `sensor-char` |
| 绑定关系 | `readlink -f /sys/bus/i2c/devices/0-0048/driver` | 指向 `.../sensor_char` |
| regmap 后端与寄存器 | `cat /sys/kernel/debug/regmap/0-0048/registers` | `0: 0181 1: 0fa1 2: 0001`（CONFIG 为 probe 写入的 0x0001；整改前为 0000。值随锯齿变化） |
| 控制器统计 | `cat /sys/kernel/debug/virt_i2c/stats` | `transfers=… errors=… injected_left=… ro_writes=…` |
| 故障注入 | `echo 3 > /sys/kernel/debug/virt_i2c/inject_error` | 随后 3 次总线传输失败（阶段 04 验证） |

---

## 9. 本阶段的改动清单与分工记录

| 文件 | 变化 | 说明 |
|---|---|---|
| `driver/virt_i2c.c` | 新增 389 行 | 虚拟 I2C 控制器 + 芯片寄存器模拟 + 故障注入 |
| `driver/sensor_char.c` | 改 220 行（+/-） | `platform_driver` → `i2c_driver`；裸 SMBus → `regmap`；数据生成移出驱动 |
| `driver/Makefile` | +2 / -1 | 新增 `virt_i2c.o`；`install` 改为按 `obj-m` 安装 |
| `driver/modules.load` | 重写注释与两行 | `i2c-stub.ko` → `virt_i2c.ko` → `sensor_char.ko` |
| `dts/sensor-node.dts.inc` | 重写 | 控制器节点 + `sensor@48` 子节点（`reg`/`poll-interval-ms`） |
| `tests/phases/02-i2c-driver.sh` | 新增 64 行 | 本阶段 16 项检查 |
| `tests/phases/smoke.sh` | +15 / -2 | 见第 5 节逐条说明 |
| `tests/userspace/sensor_stat.c` | 新增 47 行 | 只读取证小工具 |
| `scripts/20-macos-gen-dtb.sh` | +2 / -2 | 打印改成新节点结构（仅输出层） |
| `docs/10-开发与验证守则.md`、`docs/11-阶段任务书.md` | 小改 | **由父 agent 在 phase 02 前后更新**（守则里补"同一份产物可用 `test=` 选阶段"的说明与 `Unknown kernel command line parameter` 的噪声解释；任务书里把阶段 01 的强化检查表固化为契约）。这些改动随 `b55a25c` 一起提交，但**不属于阶段 02 实现者的工作**。 |

关键修复的责任划分：**字节序 bug 由父 agent 以一行修复完成**
（`sensor_char.c:584` 新增 `.val_format_endian = REGMAP_ENDIAN_LITTLE`，
并补写了 `:570-581` 的踩坑注释）；其余代码与测试由阶段 02 实现 agent 完成。
