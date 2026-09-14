# 阶段 05（IIO 子系统驱动）独立验证报告

> 验证者：独立验证 lane（LANE=05，worktree `/Users/lucien/workspace/self-study/projects/wt-05`，分支 `phase05-iio`）
> 验证时间：本次会话；宿主 macOS / QEMU TCG(`thread=multi`)-cortex-a72 / `-smp 2`；内核 6.6.156
> 内核模式确认：构建前 `grep -m1 '^CONFIG_KCSAN=y' ~/kernel-build/linux-6.6.156/.config` **无输出**（内核已回到普通模式，Image 39 MiB，非 KCSAN 的 51 MiB）
> 本报告只做**复核**，未改动任何交付源码；仅做临时负控/诊断改动并全部还原（见第四节、第七节）。

---

## 〇、结论速览

| 项 | 结论 |
|---|---|
| 05-iio 阶段测试 | **22 PASS / 0 FAIL**（`logs/20260914-134111-05-05-iio.log`，`[TEST:END]` 在 305 行） |
| smoke 回归 | **15 PASS / 0 FAIL**（`logs/20260914-134114-05-smoke.log`，`[TEST:END]` 在 296 行） |
| 内核 panic / WARNING / BUG / Call trace | 0 |
| 负控有效性 | 4 个负控（A/B/C/D）**全部被对应检查项抓住**，检查项不是恒真 |
| DoD 五条 | **通过**（逐条见第五节） |
| **首要真实问题** | 验证开始时工作树里残留一处**未还原的负控实验**（`trigger_handler` 温度/湿度通道互换，带 `NEGCTRL-B` 标记），导致交付源码开箱即 19/3 FAIL。**验证者已还原**（见第三节 S1） |
| 契约偏差 | `docs/11` 阶段 05 节的两处命令/期望与 6.6 内核实际行为不符（见第六节） |

---

## 一、契约核对（`docs/11` 阶段 05 节）

| 契约要求 | 实现情况 | 判定 |
|---|---|---|
| 新增 `driver/sensor_iio.c`（`sensor_iio.ko`）、`i2c_driver` 形态 | `driver/sensor_iio.c:337` `module_i2c_driver(sensor_iio_driver)`；`driver/Makefile` 增加 `obj-m += sensor_iio.o` | ✅ |
| 匹配 `compatible = "lucien,sensor-iio"`，设备树加 `sensor@49` | `sensor_iio.c:345` `of_match_table`；`dts/sensor-node.dts.inc` 增 `sensor1: sensor@49`；运行期 `/sys/bus/i2c/devices/0-0049` 存在且绑定 `sensor_iio` | ✅ |
| `IIO_TEMP` → 毫摄氏度（`IIO_VAL_INT`），写清换算与 scale | `sensor_iio.c:145-160` PROCESSED 返回 `raw*1000/16`（m℃）；SCALE 返回 `62 + 500000/1e6`（`IIO_VAL_INT_PLUS_MICRO`）。实测 `in_temp_input=24187`、`in_temp_scale=62.500000`，自洽 | ✅ |
| `IIO_HUMIDITYRELATIVE`（可选，用湿度寄存器） | `sensor_iio.c:216-233` 通道 + `read_raw` 湿度分支（scale=10 毫%RH/LSB）。实测 `in_humidityrelative_raw=4002` | ✅ |
| `read_raw()` 经 `regmap_read()` 取原始值 → 换算 | `sensor_iio.c:113-125` `sensor_iio_read_reg` → `regmap_read`；`read_raw` 三个分支 | ✅ |
| `devm_iio_triggered_buffer_setup(dev, indio_dev, NULL, handler, NULL)` | `sensor_iio.c:312-316` 完全一致（top=NULL，thread=handler） | ✅ |
| 触发源用 hrtimer software trigger，configfs 实例化 | 测试用 `mkdir .../iio/triggers/hrtimer/inst0`，运行期 `/sys/bus/iio/devices/trigger0/name=inst0` | ✅ |
| `current_trigger` 绑定 + `buffer/enable=1` | 测试按 `current_trigger → scan_elements → buffer/enable` 顺序；实测回读一致 | ✅ |
| 用户态读 `/dev/iio:deviceN` 得到扫描元素，解析温度并校验 15000~45000 | `user/iio_read_test.c` 镜像内核布局算法后按 stride 读；实测 5 样本全合法 | ✅ |
| 阶段只加载 `virt_i2c.ko + sensor_iio.ko`（不加载 `sensor_char.ko`） | `tests/phases/05-iio.modules` 恰为这两行；运行日志 `模块清单: /etc/modules/05-iio.load` | ✅ |

**结论：实现与契约主体一致。** 唯一"偏差"在契约文档本身（见第六节）。

---

## 二、检查项表格（名称 / 预期 / 实测 / 证据行号）

证据文件：`logs/20260914-134111-05-05-iio.log`（最终 clean 构建，22/22）。脚本行号指 `tests/phases/05-iio.sh`。

| # | 检查项（脚本行） | 预期 | 实测 | 证据（日志:行） |
|---|---|---|---|---|
| 1 | IIO 设备已注册（name=sensor_iio）(26) | `iio:deviceN/name=sensor_iio` | 找到 `/sys/bus/iio/devices/iio:device0` | 266 |
| 2 | i2c 从设备 0x49 由设备树枚举 (43) | `0-0049` 存在 | 存在 | 271 |
| 3 | 0x49 绑定 sensor_iio 驱动 | driver 链接指向 sensor_iio | `/sys/bus/i2c/drivers/sensor_iio` | 272 |
| 4 | 从设备名来自 compatible (50) | name 含 `sensor-iio` | `sensor-iio` | 273 |
| 5 | in_temp_raw 落在 240~416 | 24~26℃ → raw≈385 | 385 | 276 |
| 6 | in_temp_input 在 15000~45000 m℃ | 毫摄氏度 | 24187 | 278 |
| 7 | in_temp_scale = 62.500000 (81) | 62.5 m℃/LSB | 62.500000 | 279 |
| 8 | 湿度通道 raw 可读 (85) | 非空 | 4002 | 281 |
| 9 | configfs 实例化 hrtimer trigger | 目录存在 | `/sys/kernel/config/iio/triggers/hrtimer/inst0` | 283 |
| 10 | trigger 注册到 IIO（name=inst0）(105) | triggerN/name=inst0 | `/sys/bus/iio/devices/trigger0` | 284 |
| 11 | 可设采样频率 | 写 50 回读 50.000000 | 50.000000 | 287 |
| 12 | current_trigger 绑定 inst0 (127) | 回读 inst0 | inst0 | 289 |
| 13 | scan_elements/in_temp_en=1 (134) | 1 | 1 | 290 |
| 14 | scan_elements/in_humidityrelative_en=1 (134) | 1 | 1 | 291 |
| 15 | scan_elements/in_timestamp_en=1 (134) | 1 | 1 | 292 |
| 16 | buffer/length 可设 64 (140) | 64 | 64 | 293 |
| 17 | buffer/enable=1 (143) | 1 | 1 | 294 |
| 18 | iio_read_test 退出码 0 (147) | 0 | 0 | 296 |
| 19 | 扫描元素跨度 = 16 字节 (153) | 16 | 16 | 298 |
| 20 | 读到 `OVERALL=PASS` (154) | 存在 | 存在 | 299 |
| 21 | 成功样本 ≥3 (156) | >2 | dev_reads=5 | 300 |
| 22 | dmesg 无 WARNING/Call trace (161) | 0 | 0 | 302 |

smoke 回归（`logs/20260914-134114-05-smoke.log`）：15 项全 PASS（设备节点、probe、中断、ftrace 抓 `sensor_irq_thread`=11、字符设备用户态通路、无告警），296 行 `[TEST:END] smoke pass=15 fail=0`。

### 扫描布局（独立取证）

`05-iio.sh` 默认只 `info` 打印 `layout stride=` 一行，**不打印逐通道 offset**。为取得完整布局证据，验证者临时在脚本中加入一行诊断回显 `/tmp/iio.txt`（见表 1 说明，运行后已删除），得到（`logs/20260914-133853-05-05-iio.log:298-306`）：

```
[IIO] layout stride=16 channels=3
[IIO]   chan in_temp               scan_index=0 en=1 type=le:s16/16>>0 offset=0
[IIO]   chan in_humidityrelative   scan_index=1 en=1 type=le:u16/16>>0 offset=2
[IIO]   chan in_timestamp          scan_index=2 en=1 type=le:s64/64>>0 offset=0   ← 见 S2
[IIO] dev_reads=5 temp_milli=24250 humidity_milli=40030 OVERALL=PASS
```

对齐内核 `iio_compute_scan_bytes()`（已验证源码 `drivers/iio/industrialio-buffer.c:709`）：temp(2)@0、humidity(2)@2、时间戳(8)@ALIGN(4,8)=8，`stride=ALIGN(16,8)=16`。与驱动 `struct { s16 temp; s16 humidity; s64 ts __aligned(8); }`（`sensor_iio.c:230-234`）逐字段一致。

---

## 三、代码审查（按严重度）

内核源码引用均已核对（见第八节）。

### S1【严重·已修复】工作树残留未还原的负控实验（交付源码开箱即 FAIL）

验证开始时 `driver/sensor_iio.c:239,243` 为：

```c
	ret = sensor_iio_read_reg(st, SENSOR_REG_HUMIDITY, &raw); /* NEGCTRL-B: 错通道 */
	scan.temp = ret ? 0 : (s16)raw;        // 温度槽里放了湿度
	...
	ret = sensor_iio_read_reg(st, SENSOR_REG_TEMP, &raw);     /* NEGCTRL-B: 错通道 */
	scan.humidity = ret ? 0 : (s16)raw;    // 湿度槽里放了温度
```

- 该文件为**未跟踪文件**（`git status` 显示 `?? driver/sensor_iio.c`），因此 `git diff` 看不到它，只靠 git 状态无法发现。
- 后果：以该状态重建后 `05-iio` 为 **19 PASS / 3 FAIL**（`logs/20260914-133551-05-05-iio.log`，296/299/300 行 FAIL）。功能通路（sysfs raw/scale、trigger、buffer）全绿，**只有缓冲数据校验红**——很容易被误读为"环境抖动"。
- 与 `docs/impl/05-iio-实现记录.md` 宣称的 22/22 矛盾。
- **处理**：验证者将两处改回 `SENSOR_REG_TEMP`/`SENSOR_REG_HUMIDITY`（恢复后文件 SHA256 `02d9d5ef…`），重建后 22/22。日志证据 `logs/20260914-133551-05-05-iio.log`（红）与 `logs/20260914-134111-05-05-iio.log`（绿）。
- **给父 agent 的建议**：交付前必须有一条"负控标记清零 + 重建全绿"的收尾动作；本类残留建议在 `.gitignore` 之外，用 `grep -rn NEGCTRL` 作为 pre-commit 自检。

### S2【低】用户程序把时间戳通道的 `offset` 打印成 0（误导性，且与实现记录不符）

`user/iio_read_test.c` 的 `compute_layout()` 在 `have_ts` 分支中**只累加 bytes、不写 `ch[i].byte_offset`**，于是扫描布局打印中时间戳恒为 `offset=0`（实际应为 8）。`docs/impl/05-iio-实现记录.md` 第 3.2 节给出的"实测"片段写的是 `in_timestamp … offset=8`，**与实际程序输出不一致**（实现记录的该段证据不可复现）。

- 影响：仅日志可读性/证据可信度；时间戳 offset 不参与解析，`stride=16` 与 temp/humidity 解析正确，功能无影响。
- 位置：`user/iio_read_test.c` `compute_layout()` 的 `if (have_ts)` 分支。
- 建议：`have_ts` 分支里补 `ch[i].byte_offset = <对齐后的字节>`；并修正实现记录里的日志片段。

### S3【低】`read_errors` 自增在锁外，存在数据竞争（统计数据）

`sensor_iio.c:118-123`：

```c
	mutex_lock(&st->lock);
	ret = regmap_read(...);
	mutex_unlock(&st->lock);
	if (ret) {
		st->read_errors++;   // ← 锁外自增
```

`read_raw()`（进程上下文）与 `trigger_handler()`（线程上下文）可能并发执行，`read_errors++` 是无锁读改写。仅影响统计精度；但若内核带 KCSAN，这里会被报为 data race。建议把自增移入 `mutex_lock` 区间，或改用 `atomic_t`。

### S4【低】`read_errors` 无任何导出通道（死代码/观测缺口）

`sensor_iio.c:89` 定义了 `read_errors`，仅在 `sensor_iio_read_reg` 累加，**没有任何 sysfs/debugfs/日志导出**。对比阶段 04 的字符设备版有 debugfs `stats`。IIO 版可考虑用 `iio_priv` + dev_info 或 `iio_info.debugfs_reg_access`。当前属于"声明了但观测不到"。

### S5【信息】`scan` 结构体 padding 未初始化

`sensor_iio.c:230-234`：`{ s16 temp; s16 humidity; s64 ts __aligned(8); }` 的 offset 4~7 是填充字节，未初始化即被 `iio_push_to_buffers_with_timestamp` 拷入缓冲（对外不可见，属 IIO 常态）。可用 `memset(&scan, 0, sizeof(scan))` 消除"向用户态泄漏栈内容"的洁癖问题。

### S6【信息】未设 `available_scan_masks`

内核允许任意通道组合；仅在"三通道全开"下验证过布局（stride=16）。仅开温度通道时布局变为 `[temp u16][pad4][ts s64]`（stride=16 不变，因最大对齐=8），但未实测。与实现记录"遗留风险 2"一致。

### 已确认**没有**的问题

- **注册/注销顺序正确**：probe 全程 `devm_*`（`devm_iio_device_alloc → devm_regmap_init_i2c → devm_iio_triggered_buffer_setup → devm_iio_device_register`，`sensor_iio.c:279,285,312,319`），remove（`sensor_iio.c:336-339`）只打日志，由 devres 按逆序自动拆除。**临时 `rmmod`/`insmod` 实测**：`sensor_iio 0-0049: removed` 正常，重装后再次 `chip detected` + `IIO device registered`，全程 dmesg 告警 0（`logs/20260914-134059-05-05-iio.log:303-314`）。
- **错误路径回滚正确**：所有失败分支直接 `return`，无手动 kfree/注销，无泄漏。
- **`read_raw` 上下文/锁**：进程上下文，`sensor_iio_read_reg` 内持 mutex 访问 regmap —— 允许睡眠，正确。
- **`trigger_handler` 上下文**：top half = NULL，thread = `sensor_iio_trigger_handler`；`iio_alloc_pollfunc(h=NULL, thread, …)`（`industrialio-trigger.c:371`）+ `request_threaded_irq(pf->irq, pf->h, pf->thread, …)`（同文件 `:307`），h=NULL 时走默认 primary handler → **thread 在进程上下文运行，可以睡眠** —— 与注释宣称一致。
- **handler 参数用法**：`struct iio_poll_func *pf = p; indio_dev = pf->indio_dev;`（`sensor_iio.c:224-226`）与同版本内核 `drivers/iio/humidity/hdc100x.c:314` 写法一致，无空指针。
- **`scan_index` 与 `active_scan_mask` 位序一致**：`test_bit(0/1, active_scan_mask)` 对应 `scan_index 0/1`（`sensor_iio.c:238,242`），与通道定义顺序一致。
- **无 over-claim**：通道只声明 `RAW|PROCESSED|SCALE`，三者均实现；未声明 `sampling_frequency`（正确——该属性属于 trigger 设备）。
- **模块依赖打包**：构建脚本按需打包 `industrialio / industrialio-triggered-buffer / iio-trig-hrtimer / kfifo-buf / regmap-i2c`（`scripts/13-vm-fast-cycle.sh`），实测模块全部加载成功。

---

## 四、负控实验表（验证者亲自执行，逐次恢复）

每个负控后都重建、跑 `05-iio`、记录日志，然后恢复源码并确认 `grep -rn NEGCTRL` 为空。

| 实验 | 改法（临时） | 期望被抓住的检查 | 实测结果 | 证据（日志:行） |
|---|---|---|---|---|
| **A** 换算单位错 | `read_raw` PROCESSED：`*val = sensor_iio_raw_to_milli(raw);` → `*val = (s32)(s16)raw / 16;`（℃ 而非 m℃） | `in_temp_input 在合理区间` | **21 PASS / 1 FAIL**：`实测 24 毫℃` | `logs/20260914-133717-05-05-iio.log:278`（`[TEST:END]` 305 行 `pass=21 fail=1`） |
| **B** handler 通道互换 | `trigger_handler`：temp 槽读 HUMIDITY、humidity 槽读 TEMP | 缓冲读取三项 | **19 PASS / 3 FAIL**（296/299/300 行） | `logs/20260914-133735-05-05-iio.log`（`[TEST:END]` 305 行 `pass=19 fail=3`） |
| **C** 不装配 buffer | 注释掉 `devm_iio_triggered_buffer_setup(...)` | buffer/enable、scan_elements、current_trigger、用户态读样本 | **12 PASS / 10 FAIL**（current_trigger / 三项 `_en` / buffer/length / buffer/enable / iio_read_test / stride / OVERALL / dev_reads 全红） | `logs/20260914-133754-05-05-iio.log:290-306`（`[TEST:END]` 312 行 `pass=12 fail=10`） |
| **D** 复现 P1 解析缺陷 | `user/iio_read_test.c::compute_layout` 改为把 `_index` 当字节偏移（`offset=scan_index; bytes=offset+storagebits/8`） | `stride=16` 断言 + 读样本 | **18 PASS / 4 FAIL**，`stride=16` 实测 **10** | `logs/20260914-133820-05-05-iio.log:296,298,299,300`（`[TEST:END]` 305 行 `pass=18 fail=4`） |

**结论：4 个负控全部被对应检查项抓到**，且报错信息可定位（A 打印实际值 24、D 打印实际 stride=10）。**检查项不是恒真**，`stride=16` 断言确实能挡住"index 当偏移"这类解析错误（D 实测 stride=10 被抓住）。C 说明"buffer 未装配/未 enable/trigger 未绑定"类缺陷会大面积报红，不会漏过。

---

## 五、独立复现结果

命令（LANE=05 固定 `~/lab-05`，与主树并行 lane 隔离）：

```bash
WT=/Users/lucien/workspace/self-study/projects/wt-05
bash $WT/scripts/20-macos-gen-dtb.sh
limactl shell dev bash -c "LANE=05 TEST=05-iio bash $WT/scripts/13-vm-fast-cycle.sh"
LANE=05 bash $WT/scripts/21-macos-sync-artifacts.sh
LANE=05 bash $WT/scripts/22-macos-run-test.sh 05-iio 300
LANE=05 bash $WT/scripts/22-macos-run-test.sh smoke 180
```

| 构建/运行 | 结果 |
|---|---|
| 内核模块编译 | `make` 无 error、无 warning（3 个 .ko + 4 个用户态程序） |
| 05-iio（最终 clean） | **22 PASS / 0 FAIL**，`[TEST:END]`，panic 0，告警 0 —— `logs/20260914-134111-05-05-iio.log` |
| smoke（最终 clean） | **15 PASS / 0 FAIL**，`[TEST:END]`，panic 0，告警 0 —— `logs/20260914-134114-05-smoke.log` |
| 开箱（未修复 S1 时） | 19 PASS / 3 FAIL —— 见 S1 |

> 最终 clean 构建的产物：`Image` 39 MiB（普通模式）、`initramfs.cpio.gz` 2.3 MiB。两份产物与交付源码一致。

---

## 六、契约偏差

| 偏差 | 位置 | 说明 | 影响 |
|---|---|---|---|
| **`sampling_frequency` 路径** | `docs/11` 阶段 05 节示例 `echo 100 > /sys/kernel/config/iio/triggers/hrtimer/inst0/sampling_frequency` | 6.6 中该属性挂在 **trigger 设备**（`drivers/iio/trigger/iio-trig-hrtimer.c:80` `DEVICE_ATTR(sampling_frequency,…)`），configfs 实例目录的 `config_item_type` 只填 `.ct_owner`。往 configfs 路径写会得 `EACCES`（不是 ENOENT）。实现与测试正确地用了 `/sys/bus/iio/devices/triggerN/sampling_frequency`。 | 契约文档错误（实现已绕开），需父 agent 修订 `docs/11` |
| **trigger 名字期望** | `docs/11` 阶段 05 节检查项："`trigger*/name` 出现 `hrtimer`" | 实际 `iio_trigger_alloc(NULL, "%s", name)`（`iio-trig-hrtimer.c:137`）使**名字等于 configfs 实例名**，即 `inst0`（不是 `hrtimer`）。测试按 `inst0` 校验，正确。 | 契约文档错误 |
| `in_temp_input_raw` / `in_temp_input` | `docs/11` 检查项表格 | 实现暴露 `in_temp_raw`（原始）与 `in_temp_input`（processed），命名符合 IIO 惯例；文档措辞含糊，无实质冲突 | 无 |
| 模块清单文件名 | `docs/11` 写 `driver/modules.load` 或 `tests/phases/05-iio.sh.modules` | 实现用 `tests/phases/05-iio.modules`，符合 `docs/10 §4.3` 的 `tests/phases/<阶段名>.modules` | 无 |

---

## 七、遗留疑点 / 残余风险

1. **未覆盖通道子集布局**（同 S6）：只验证三通道全开；`available_scan_masks` 未设。
2. **未做 libiio 交叉验证**：湿度单位（毫%RH）与 `iio_generic_buffer`/libiio 的约定未交叉确认（实现记录遗留风险 4）。
3. **大端未验证**：`scan_type.endianness = IIO_CPU`，用户程序有 le/be 分支，但仅在 aarch64 小端验证。
4. **测试脚本的可诊断性**：`05-iio.sh` 在用户程序失败时只回显 `layout stride=` 一行，`/tmp/iio.txt` 里的逐样本越界信息（stderr）**不打印到串口**，失败时需重建诊断；这是恒真检查之外的"观测缺口"（非正确性问题）。
5. **测试脚本硬编码期望 `stride=16`**：这是刻意的回归护栏，但若日后合法地改动 `storagebits`（如换 8 位通道），需同步改测试——属于可接受的强断言。
6. **跨 lane 基础设施改动**：本 lane 修改了 `scripts/13/20/21/22` 与 `tests/runner/init.sh`（模块清单运行期选择、`LANE` 隔离、`-smp 2`/`thread=multi`）。这些是**跨阶段/跨 lane 的公共脚本**，合并到主树时需与主树阶段 03/04 改动对账（实现记录 P0/P3 已说明）。
7. **实现记录的一处证据不实**：见 S2（`in_timestamp offset=8` 与实际 `offset=0` 不符）。

> 关于 KCSAN：本次最终日志与所有负控/诊断日志中**均未出现 KCSAN 报告**（`grep -i KCSAN` 无命中）。内核已回到普通模式，本阶段结论不依赖 KCSAN 内核。

---

## 八、内核源码引用核对（用于验证实现记录与测试注释）

| 引用点 | 实现记录/脚本声称 | 实测源码 | 一致 |
|---|---|---|---|
| `scan_elements/*_index` 语义 | `industrialio-buffer.c:362` 返回 `c->scan_index` | `iio_show_scan_index()`：`sysfs_emit(buf,"%u\n", …->c->scan_index)` | ✅ |
| 布局算法 | `industrialio-buffer.c:709` `iio_compute_scan_bytes()` | 存在，逻辑 = `bytes=ALIGN(bytes,len); bytes+=len; largest=max`，时间戳分支 + 末尾 `ALIGN(bytes,largest)` | ✅ |
| `iio_storage_bytes_for_si` | 用户程序用 `storagebits/8` | 源码：`storagebits/8 * (repeat>1 ? repeat : 1)`，本项目 repeat=1 | ✅ |
| `sampling_frequency` 归属 | `iio-trig-hrtimer.c:80` | `DEVICE_ATTR(sampling_frequency, S_IRUGO\|S_IWUSR, …)` 于 80 行 | ✅ |
| trigger 命名 | `iio-trig-hrtimer.c:135` | `iio_trigger_alloc(NULL, "%s", name)` 在 137 行 | ✅ |
| 绑定顺序不可颠倒 | `industrialio-trigger.c:455` `current_trigger_store` 返回 `-EBUSY` | `if (currentmode==INDIO_BUFFER_TRIGGERED) return -EBUSY;` | ✅ |
| setup 追加 buffer 模式 | `industrialio-triggered-buffer.c:81` | `indio_dev->modes \|= INDIO_BUFFER_TRIGGERED;` | ✅ |
| hdc100x 写法 | `hdc100x.c:315` `pf=p; indio_dev=pf->indio_dev` | 一致 | ✅ |
| 时间戳通道 | `IIO_CHAN_SOFT_TIMESTAMP` storagebits=64/realbits=64 | `include/linux/iio/iio.h:309` 一致 | ✅ |

---

## 九、DoD 五条逐条判定（`docs/10 §5`）

| # | DoD 条款 | 判定 | 依据 |
|---|---|---|---|
| 1 | `make` 无 error、无 warning（新增代码部分） | **通过** | 最终构建 3 个 .ko + 4 个用户程序编译输出无 error/warning；负控 A 曾出现 `-Wunused-function` 属临时改码，已随恢复消失 |
| 2 | 本阶段测试全 PASS 且跑到 `[TEST:END]` | **通过** | `logs/20260914-134111-05-05-iio.log` 22/22，`[TEST:END]` 305 行 |
| 3 | smoke 回归全 PASS | **通过** | `logs/20260914-134114-05-smoke.log` 15/15，`[TEST:END]` 296 行 |
| 4 | 内核 panic=0；WARNING/BUG/Call trace 必须在文档解释 | **通过** | 最终两份日志 panic=0、告警=0；无 KCSAN 报告 |
| 5 | 三份文档落盘且结论可被日志复现 | **部分不通过 → 已可复现** | `docs/impl`、`docs/kb`、`docs/verify` 均存在（本报告即 `docs/verify/05-iio-验证报告.md`）。但 **`docs/impl` 有一处日志证据不可复现**（S2：`in_timestamp offset=8` 实为 `offset=0`）；**开箱源码曾含未还原负控**（S1）与实现记录宣称的 22/22 不符。**两项均已由验证者指出，其中 S1 已由验证者修复并复现全绿**。 |

**总体判定：通过（附条件）** —— 在验证者修复 S1 后，阶段 05 满足 DoD 的功能与回归门槛；S2/S3/S4 为文档/统计层面的低severity问题，不阻塞，但建议实现者一并修订实现记录与代码。

---

## 十、给父 agent / 实现者的整改清单

| 优先级 | 事项 | 位置 |
|---|---|---|
| 高 | 交付前清零负控标记并重建全绿（本次 S1 已暴露风险）；建议 pre-commit `grep -rn NEGCTRL` | `driver/` |
| 中 | 修订 `docs/11` 阶段 05：`sampling_frequency` 改为 trigger 设备路径、trigger 名字期望改为实例名 | `docs/11-阶段任务书.md` |
| 中 | 修正 `docs/impl` 中不可复现的日志片段（`in_timestamp offset=8` → `offset=0`），或修正程序使 offset 正确 | `docs/impl/05-iio-实现记录.md` / `user/iio_read_test.c` |
| 低 | `read_errors` 自增移入锁内（或 atomic） | `driver/sensor_iio.c:118-123` |
| 低 | `read_errors` 导出（debugfs/sysfs）或在注释中说明"仅供内部调试" | `driver/sensor_iio.c:89` |
| 低 | 在 `05-iio.sh` 失败时回显 `/tmp/iio.txt`，提升可诊断性 | `tests/phases/05-iio.sh` |

---

## 附：本次会话产生的日志清单

| 用途 | 日志 |
|---|---|
| 开箱（含残留负控 B）红 | `logs/20260914-133551-05-05-iio.log`（19/3） |
| 负控 A | `logs/20260914-133717-05-05-iio.log`（21/1） |
| 负控 B | `logs/20260914-133735-05-05-iio.log`（19/3） |
| 负控 C | `logs/20260914-133754-05-05-iio.log`（12/10） |
| 负控 D | `logs/20260914-133820-05-05-iio.log`（18/4） |
| 布局诊断 | `logs/20260914-133853-05-05-iio.log`（22/0，含逐通道 offset） |
| rmmod/insmod 诊断 | `logs/20260914-134059-05-05-iio.log`（22/0，含卸载路径） |
| **最终 clean 05-iio** | **`logs/20260914-134111-05-05-iio.log`（22/0）** |
| **最终 clean smoke** | **`logs/20260914-134114-05-smoke.log`（15/0）** |
