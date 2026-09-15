# 阶段 02 独立验证报告：虚拟 I2C 控制器 + 标准 i2c_driver + regmap + 设备树子节点匹配

> 验证者：独立验证者（不参与实现）
> 验证时间：2026-09-14
> 被验证对象：commit `b55a25c`「阶段02：虚拟 I2C 控制器 + 标准 i2c_driver + regmap + 设备树子节点匹配」
> 契约依据：`docs/11-阶段任务书.md`「阶段 02」节；纪律依据：`docs/10-开发与验证守则.md`
> 内核：6.6.156 / aarch64 / QEMU `-M virt -cpu cortex-a72 -accel tcg`（单核）
> 结论：**不通过（整改后可转通过）** —— 功能实现与三项测试独立复现全部通过、负控实验证明测试有效；
> 但阶段 DoD 第 5 条（三份文档）**未满足**（缺 `docs/kb/02-*`；`docs/impl/02-*` 在验证过程中由并行工作树补齐，
> 已于 04:11:37 落盘且经本报告交叉核对），
> 且 `tests/phases/02-i2c-driver.sh` 存在一处**真实的「假绿」检查项**（见 F2）与两处弱断言。
>
> **时序说明**：本验证开始时（04:06）`docs/impl/02-*` 与 `docs/kb/02-*` 均不存在；
> 04:11:37 并行工作树补出了 `docs/impl/02-i2c-driver-实现记录.md`，本报告据此把原「两份文档缺失」收窄为
> 「`docs/kb/02-*` 缺失」。对该 impl 文档的证据核对见第 10 节。

---

## 0. 本轮验证用到的全部日志

| 日志文件 | 用途 | 结论 |
|---|---|---|
| `logs/20260914-041031-02-i2c-driver.log` | 独立复现：02（干净源码） | 16 PASS / 0 FAIL |
| `logs/20260914-041039-01-io-models.log` | 独立复现：01 回归 | 16 PASS / 0 FAIL |
| `logs/20260914-041048-smoke.log` | 独立复现：smoke 回归 | 15 PASS / 0 FAIL |
| `logs/20260914-040716-02-i2c-driver.log` | 负控 A（删 `.val_format_endian`） | 15 PASS / **1 FAIL** |
| `logs/20260914-040731-02-i2c-driver.log` | 负控 A 恢复后复跑 | 16 PASS / 0 FAIL |
| `logs/20260914-040748-02-i2c-driver.log` | 负控 B（删 `adapter.of_node` 赋值） | 4 PASS / **10 FAIL** |
| `logs/20260914-040805-02-i2c-driver.log` | 负控 B 恢复后复跑 | 16 PASS / 0 FAIL |
| `logs/20260914-040826-02b-inject-probe.log` | 负控 C（故障注入语义，临时脚本） | 6 PASS / 0 FAIL |
| `logs/20260914-040925-02c-rmmod-probe.log` | 负控 E（重复 insmod/rmmod 3 轮，临时脚本） | 32 PASS / 0 FAIL |
| `logs/20260914-041005-02-i2c-driver.log` | 负控 D（测试体内注入 WARN，**假绿实证**） | 16 PASS / 0 FAIL（但内核告警=2） |
| `logs/20260914-041014-smoke.log` | 负控 D 同一二进制跑 smoke | 14 PASS / **1 FAIL** |

复现命令（全部走项目脚本，未绕过）：

```bash
limactl shell dev bash -c 'TEST=<阶段> bash .../scripts/13-vm-fast-cycle.sh'
bash scripts/21-macos-sync-artifacts.sh
bash scripts/22-macos-run-test.sh <阶段> 180
```

---

## 1. 契约核对（docs/11 阶段 02 节，逐条）

| # | 契约要求 | 实测 | 判定 | 证据 |
|---|---|---|---|---|
| 1.1 | `virt_i2c.c` 是 `platform_driver`，`of_match_table` 匹配 `lucien,virt-i2c` | 是（`driver/virt_i2c.c:363-384`，`module_platform_driver`） | ✅ | 02 日志:258 打印 `registered i2c adapter i2c-0` |
| 1.2 | 设置 `adapter.of_node = pdev->dev.of_node` | 是（`virt_i2c.c:337`） | ✅ | 负控 B 删除该行后 10 项 FAIL（日志:266-280），证明该行是枚举的必要条件 |
| 1.3 | 实现 `smbus_xfer` + `functionality` 返回 `WORD_DATA\|BYTE_DATA` | 是，另多声明 `I2C_FUNC_SMBUS_BYTE`（`virt_i2c.c:218-219`） | ✅（多声明见偏差 D3） | 02 日志:279 regmap 读到真实寄存器值 |
| 1.4 | 芯片寄存器 0x00 温度(16 位,符号,LSB=1/16℃) / 0x01 湿度 / 0x02 配置 | 是（`virt_i2c.c:56-66`、`virt_i2c_convert()`） | ✅ | 02 日志:279 `0: 0181 1: 0fa1 2: 0000`（0x0181=24.062℃，0x0fa1=40.01%RH） |
| 1.5 | 温度确定性漂移（非随机），24.0~26.0℃ | 是，内部计数锯齿波（`virt_i2c.c:139-150`） | ✅ | 01/02 多次运行温度落在同一区间；负控 A 抓到乱码 |
| 1.6 | debugfs `inject_error`(WO) + `stats`(RO)，stats 含 `transfers/errors/injected_left` | 是（`virt_i2c.c:263-291`，0444/0200） | ✅ | 02 日志:21-22 两个路径存在；负控 C 日志:266/298 读出字段 |
| 2.1 | `sensor_char.c` 改为 `i2c_driver`（`probe(i2c_client*)`/`remove`/`id_table`/`of_match`） | 是（`sensor_char.c:706-726`） | ✅ | 02 日志:260-263 probe 由 i2c 核心调用；`:272` `/sys/bus/i2c/devices/0-0048` |
| 2.2 | 寄存器访问改走 regmap，`reg_bits=8/val_bits=16/max_register=0x02` | 是（`sensor_char.c:581-585`），另加 `val_format_endian=LITTLE`（必要，见 D2） | ✅ | 02 日志:279；`/sys/bus/i2c/.../driver` 指向 sensor_char（日志:270） |
| 2.3 | `i2c_errors` 在 `regmap_read` 失败时累加 | 是（`sensor_char.c:203`） | ✅ | 负控 C 日志:306/309 `sensor i2c_err=3` 恰好 +3 |
| 2.4 | 保留字符设备/waitqueue/四种 IO 模型/poll/fasync/虚拟 irq_chip+irq_domain+threaded IRQ/`poll-interval-ms` 解析 | 全部保留 | ✅ | `logs/20260914-041039-01-io-models.log:314` 16 PASS；`smoke` 日志 ftrace 抓到 `sensor_irq_thread`；02 日志:285 `interval=500` |
| 2.6 | 中断方案 (a)/(b) 任选其一**并在文档说明理由** | 选 (a)（留在 `sensor_char.c`），理由已写入 `docs/impl/02-i2c-driver-实现记录.md` §4.3 | ✅（文档于 04:11:37 补齐后满足） | impl 文档 `§4.3 中断为什么留在 sensor_char.c` |
| 2.5 | **删除** `i2c_get_adapter()` + `i2c_new_client_device()` 手工实例化 | 源码中已无调用（仅注释提及，`sensor_char.c:552-556`） | ✅ | `grep -rn i2c_get_adapter driver/` 仅命中注释 |
| 3.1 | dts 改为 `virt_i2c` 控制器 + `sensor@48` 子节点 | 是，但节点名与契约字面不同（见 D1） | ✅（有偏差） | `dts/sensor-node.dts.inc`；重新生成后 `dtc -I dtb -O dts artifacts/virt-sensor.dtb` 可见同构节点 |
| 3.2 | 节点插入逻辑仍由 `scripts/20-macos-gen-dtb.sh` 完成 | 是，脚本已重跑成功 | ✅ | `bash scripts/20-macos-gen-dtb.sh` 输出「插入位置：根节点第 398 行闭合括号之前」 |
| 4 | `driver/modules.load` = `virt_i2c.ko` → `sensor_char.ko`，不再需要 i2c-stub | 是 | ✅ | `driver/modules.load:16-17`；13 脚本输出 `/etc/modules.load (来源: modules.load): virt_i2c.ko / sensor_char.ko` |
| 5 | 测试脚本至少覆盖 6 项最小检查表 | 全覆盖（16 项） | ✅ | 见第 2 节表格 |
| 6 | 「regmap 生效」需实测确认 debugfs 路径真实存在 | 真实存在 `0-0048/registers` | ✅ | 02 日志:277-279 |

**契约偏差清单**：D1（dts 节点名）、D2（多写 endian，必要补充）、D3（functionality 多声明能力）、D4（中断方案 (a) 的「文档说明理由」原未满足，已于 04:11:37 由 `docs/impl/02-*` §4.3 补齐）。

---

## 2. 阶段测试检查项逐条核对（干净源码，日志 `logs/20260914-041031-02-i2c-driver.log`）

| 检查项 | 预期 | 实测 | 证据（日志:行） |
|---|---|---|---|
| 控制器注册日志（adapter 带 of_node） | 有注册行 | PASS（**但断言不含 of_node**，见 F3） | :258 |
| 故障注入接口 `inject_error` | 存在 | 存在 | :268 |
| 控制器统计接口 `stats` | 存在 | 存在 | :269 |
| 子节点 sensor@48 被实例化成 i2c 从设备 | `/sys/bus/i2c/devices/*-0048` 非空 | `/sys/bus/i2c/devices/0-0048` | :272 |
| 从设备名来自设备树 compatible | `name` 含 `sensor-char` | `sensor-char` | :272 |
| driver 符号链接指向 sensor_char | `readlink -f` 含 `sensor_char` | 命中 | :270 |
| probe 由 i2c 核心调用（含从机地址与 DT 周期） | `probe: i2c client addr=0x48 interval=500ms` | 逐字命中 | :260 |
| regmap 注册了 debugfs 节点 | `/sys/kernel/debug/regmap/*-0048` | `0-0048` | :277 |
| regmap 提供 `registers` 文件 | 存在 | 存在 | :278 |
| 能用 regmap 读出寄存器内容 | 非空（**不校验数值**，见 F4） | `0: 0181 1: 0fa1 2: 0000` | :279 |
| probe 时读到芯片配置寄存器 | 日志含 `chip detected: config=` | `config=0x0001` | :261 |
| 设备树 poll-interval-ms 生效 | 读回 `interval=500` | `interval=500` | :285 |
| sensor_test 退出码为 0 | 0 | 0 | :293 |
| 温度读数落在 24.0~26.0℃ | 匹配 `temp=2[456]\.` | PASS（正确实现下 0x0181→24.062℃） | :294, :279 |
| 用户态测试正常结束 | 含「测试结束」 | 命中 | :296 |
| dmesg 无 WARNING/Call trace | 计数 0 | 0（**取样窗口为 0**，见 F2） | :294-295 |

`[TEST:END] 02-i2c-driver pass=16 fail=0`（:298），内核 panic=0、内核告警=0。

### 2.1 检查项设计问题（详细见第 4 节 F2/F3/F4）

- **无恒真断言**：所有检查都基于真实文件/内核对象/日志子串，没有 `pass` 写死。
- 但有三项**断言强度与检查项名称不符**（F2 最严重：取样窗口为 0；F3 名称声称验证 of_node 而断言不验证；F4 只判非空）。
- 「温度区间」这一项**确实能区分正确/错误实现**（负控 A 实证，见 3.1），但它是「任一输出行命中即可」的弱形式：若只有部分读数据错，仍会 PASS。它不在契约最小检查表内，属实现者自加的额外检查，强度可接受。

---

## 3. 负控实验（本轮核心产出）

所有负控均**临时修改源码 → 重新编译打包 → 跑测试 → `git checkout` 恢复 → 复跑确认全绿**。
临时脚本（02b/02c）实验后已删除；构建产物已按干净源码重打包（最终 initramfs 内只有
`01-io-models.sh`/`02-i2c-driver.sh`/`smoke.sh` 三个阶段脚本，已核对）。

### 实验汇总表

| 编号 | 破坏点 | 改动 | 结果 | 抓住它的检查项（日志:行） | 恢复验证 |
|---|---|---|---|---|---|
| **A** | `driver/sensor_char.c` 删除 `.val_format_endian = REGMAP_ENDIAN_LITTLE`（回默认 BIG） | 1 行 | 15 PASS / **1 FAIL** | `温度读数落在芯片模拟区间 24.0~26.0 ℃ -- 实测 temp=2096.062`（`logs/20260914-040716-02-i2c-driver.log:292`） | `logs/20260914-040731-02-i2c-driver.log:298` 16/0 ✅ |
| **B** | `driver/virt_i2c.c` 删除 `chip->adap.dev.of_node = pdev->dev.of_node;` | 1 行 | 4 PASS / **10 FAIL** | 子设备未枚举（:266）、name 不含 sensor-char（:267）、driver 链接（:270）、probe 日志（:271）、regmap 节点（:273）、chip detected（:275）、interval（:277）、sensor_test 退出码（:278）、温度（:279）、测试结束（:280）。`logs/20260914-040748-02-i2c-driver.log` | `logs/20260914-040805-02-i2c-driver.log:298` 16/0 ✅ |
| **C** | 故障注入语义（临时脚本 `02b-inject-probe.sh`，已删） | +/- 临时文件 | **6 PASS / 0 FAIL** | 见 3.3 | 脚本已删，源码未动 |
| **D** | `driver/sensor_char.c` 在 `sensor_read()` 插入 `WARN_ON_ONCE(1)`（测试体内制造告警） | 1 行 | 02: 16 PASS/**0 FAIL**（但内核告警=2）；同一二进制 smoke: 14 PASS/**1 FAIL** | 02 假绿：`logs/20260914-041005-02-i2c-driver.log:289` 有 WARNING，`:326` 却 `[CHECK:PASS] dmesg 无 WARNING`；smoke 正确 FAIL：`logs/20260914-041014-smoke.log:320` | 源码 `git checkout` 恢复，最终三跑全绿 |
| **E** | 重复 insmod/rmmod 安全性（临时脚本 `02c-rmmod-probe.sh`，已删） | +/- 临时文件 | **32 PASS / 0 FAIL**（3 轮卸载/加载） | `logs/20260914-040925-02c-rmmod-probe.log:347/381` | 脚本已删，源码未动 |

### 3.1 负控 A 细节（byte-order 负控）

- 破坏后 regmap debugfs 内容从 `0: 0181 1: 0fa1` 变成 `0: 8101 1: a10f`（`:279`），即 16 位值被 `swab16`。
- 用户态首个样本 `temp=2096.062`（= `swab16(0x0183)` → 2096.062℃），被温度区间断言抓住。
- **结论**：温度区间断言确实是唯一能抓住 regmap 字节序错误的检查项；F4 的「regmap 非空」检查对此**完全不敏感**（破坏后仍 PASS）。

### 3.2 负控 B 细节（of_node 负控）

- 删除 `adapter.of_node` 赋值后：`/sys/bus/i2c/devices/0-0048` 根本不存在 → i2c 核心没有枚举子节点 → `sensor_char` 不 probe → `/dev/sensor0` 缺 → 后续 10 项连锁 FAIL。
- **结论**：「设备树枚举 + i2c_driver 绑定」这条链路被测试有效地守住了。
- 注意：检查项「控制器注册日志（adapter 带 of_node）」在破坏后**仍然 PASS**（:258 打印 `of_node=/virt-i2c`，因为该 `dev_info` 打印的是 `pdev->dev.of_node`，不是 `chip->adap.dev.of_node`）。→ 见 F3。

### 3.3 负控 C 细节（故障注入语义，契约「写 N 后恰好 N 次传输返回 -EIO」）

临时脚本：清基线 → `echo 3 > inject_error` → 强制 5 次采样读（`write 0x01` 到 `/dev/sensor0`）→ 观测：

```
baseline errors=0 transfers=2
fault injection armed: next 3 transfer(s) will fail        (02b 日志:276)
injected error: addr=0x48 reg=0x00 (left=2/1/0)            (02b 日志:279/284/289)
after: errors=3 transfers=9 injected_left=0 (delta errors=3 transfers=7)   (02b 日志:303)
after: sensor i2c_err=3                                    (02b 日志:306)
final: errors=3 injected_left=0 sensor i2c_err=3           (02b 日志:322)
```

判定：`errors` 恰好 +3（期间发生 7 次传输，含 2 次 hrtimer 噪声；无论哪 3 次失败，总数精确为 3），
`injected_left=0`，`sensor_char.i2c_errors` 恰好 +3；注入耗尽后再触发 3 次，两个计数都不再增长；
`echo 0`（取消注入）后不产生错误。

**结论**：故障注入语义与契约完全一致（恰好 N 次，而非「永久故障」或「N 次以上」）；
但**阶段 02 的正式测试脚本并未覆盖故障注入**（契约把它们放在阶段 04 测，可接受），
此处由独立验证者补充验证。

### 3.4 负控 D 细节（发现一处真实「假绿」）

同一份含 `WARN_ON_ONCE` 的 `sensor_char.ko`：

- `02-i2c-driver`：`[TEST:END] pass=16 fail=0`，脚本自己打印
  `[CHECK:PASS] dmesg 无 WARNING/Call trace`（`logs/20260914-041005-02-i2c-driver.log:326`），
  而同一日志 `:289` 明明有 `WARNING: CPU: 0 PID: 101 ... sensor_read`；
  外层 `22-macos-run-test.sh` 也只把它当成「内核告警数: 2」，**不影响退出码**，输出仍是
  「✅ 全部检查项通过（内核告警数: 2）」。
- 同一二进制跑 `smoke`：`[CHECK:FAIL] dmesg 无 WARNING/Call trace -- 期望='0' 实际='2'`
  （`logs/20260914-041014-smoke.log:320`）→ smoke 的检查**有效**。

差异根因：`smoke.sh:64` 在测试末尾重新 `dmesg > /tmp/dmesg2.txt`，而
`02-i2c-driver.sh:19` 只在脚本开头抓一次 `/tmp/probe.txt`，`63-64` 行直接复用它。

### 3.5 负控 E 细节（重复 insmod/rmmod）

`rmmod sensor_char; rmmod virt_i2c; insmod virt_i2c.ko; insmod sensor_char.ko` × 3 轮，每轮复验：
`/dev/sensor0` 先消失后重建、i2c 从设备重新枚举、virt_i2c debugfs 重建、`sensor_test` 退出码 0、
dmesg 无 WARNING/BUG/Call trace（32 项全 PASS）。

**结论**：probe/remove 的资源分配-释放是对称的，重复装卸无泄漏/无 use-after-free 迹象。

---

## 4. 代码审查发现的隐患（按严重度）

| 编号 | 严重度 | 位置 | 问题 | 影响 / 验证 |
|---|---|---|---|---|
| **F1** | **高（DoD 硬性）** | `docs/kb/` | 阶段 02 的**知识点文档缺失**（`docs/kb/` 只有 `01-io-models-知识点.md`）。`docs/impl/02-i2c-driver-实现记录.md` 已存在（并行工作树于 04:11:37 补齐，见第 10 节） | 违反 `docs/10` 六节「每阶段产出三份文档」与 DoD 第 5 条。本次验证开始时 impl/kb 均缺失，现仅剩 kb |
| **F2** | **中** | `tests/phases/02-i2c-driver.sh:19,63-64` | 「无内核告警」检查复用脚本**开头**抓取的 `/tmp/probe.txt`，取样窗口 = 0；测试体（read/regmap/trigger）期间产生的 WARNING 不参与判定 | 负控 D 实证：内核有 WARNING 时 02 仍 16/16 显示「全部检查项通过」。缺陷会在阶段关卡被静默放过 |
| **F3** | **中** | `tests/phases/02-i2c-driver.sh:20` | 检查项名「控制器注册日志（**adapter 带 of_node**）」只 grep `registered i2c adapter i2c-`；而日志里的 `of_node=` 打印的是 `pdev->dev.of_node`（`virt_i2c.c:354-355`），并非 `adapter.of_node` | 负控 B 实证：删掉 `virt_i2c.c:337` 的赋值后该检查仍 PASS。名不副实，容易造成「of_node 已被验证」的错觉 |
| **F4** | 低 | `tests/phases/02-i2c-driver.sh:41,44` | regmap 两项检查只判 `registers` 文件**存在/非空**，不校验数值 | 负控 A 实证：内容被字节交换成 `8101/a10f` 时仍 PASS。真正的数值正确性只由温度区间断言兜底 |
| **F5** | 低 | `driver/virt_i2c.c:218-219` | `functionality` 多声明 `I2C_FUNC_SMBUS_BYTE`，但 `smbus_xfer` 对 `size=I2C_SMBUS_BYTE` 走 `default:` 返回 `-EOPNOTSUPP`（`virt_i2c.c:199-200`） | 能力位与实际实现不符（over-claim）。本项目 regmap 走 word 路径不受影响，但真实客户端调 `i2c_smbus_read_byte()` 会得到 EOPNOTSUPP 且被计入 `errors` |
| **F6** | 低 | `driver/virt_i2c.c:237,242,251` | stats 读缓冲固定 `kzalloc(768)`，`scnprintf(buf+n, 768-n, ...)` 无下界保护 | 当前 `VBANK_MAX=4` 时最长约 420 字节，安全；但一旦增加 bank 或字段，`768-n` 会整型下溢（巨大 size_t） |
| **F7** | 低 | `driver/sensor_char.c:630-631`、`driver/virt_i2c.c:320` | 语义矛盾三连：probe 打印 `config=0x0001 -> enable continuous conversion` 却随后 `regmap_write(..., 0x0000)`（按 `virt_i2c.c:55` 的定义 bit0=1 才是「连续转换使能」，写 0 等于关闭）；`virt_i2c.c:320` 又把默认值 `0x0001` 注释成「连续转换关」 | 仅注释/日志错误（CONFIG 在模拟芯片里「只做记录」），但对外产出 `chip detected: config=0x0001 -> enable...` 的日志会误导读者 |
| **F8** | 低 | `driver/sensor_char.c:431-433` | `write()` 里用 `local_irq_disable()/local_irq_enable()` 模拟中断上下文，未用 `local_irq_save/restore` | 无条件开中断，会破坏调用点原有的中断屏蔽状态。单核 VM 未暴露问题；是内核编码规范上的隐患 |
| **F9** | 低 | `driver/sensor_char.c:610,627-633` | probe 失败分支直接 `return ret`，未清 `i2c_set_clientdata(client, sd)`；`regmap_write(SENSOR_REG_CONFIG, 0)` 的返回值未检查 | probe 失败后 devm 释放 `sd`，client 的 drvdata 悬空（当前无代码路径再取用，属常见惯用法）；config 写失败被静默忽略 |
| **F10** | 低 | `scripts/13-vm-fast-cycle.sh:96,113` | 仍无条件拷贝 `i2c-stub.ko`；无 `modules.load` 时的 fallback 仍写 `i2c-stub.ko chip_addr=0x48` | 阶段 02 已不需要 i2c-stub（`driver/modules.load:11` 也这么写）。属遗留，不影响正确性，但产物里多了一个用不到的模块 |
| **F11** | 低 | `tests/phases/02-i2c-driver.sh:43,49-50` | 未回显 `/tmp/utest.txt`，串口日志里看不到逐次温度读数 | 事后只能从 regmap `registers` 反推温度（0x0181→24.062℃），「温度值」这一实测值在日志中不可直接查证 |

### 4.1 已核查为「无问题」的高风险点（排除的假设）

- `virt_i2c_remove()`（`virt_i2c.c:360-367`）：`debugfs_remove_recursive` → `i2c_del_adapter` 顺序正确；`chip` 由 devm 释放。
- `sensor_probe()` 错误路径（`sensor_char.c:674-681`）逐级回滚 domain/chrdev/device/cdev/region，完整；`request_threaded_irq` 失败经 `err_remove_domain` 由 `irq_domain_remove()` 一并回收 mapping。
- `sensor_remove()`（`sensor_char.c:684-695`）：`hrtimer_cancel` → `free_irq` → `irq_domain_remove` 顺序正确（先停采样源、再同步中断、最后销毁 domain），无 UAF 窗口；负控 E 三轮装卸无告警。
- `smbus_xfer` 锁与并发：`chip->lock` 保护 banks/统计/注入计数，注入递减在锁内，`transfers++` 在锁内；边界情况 `addr` 越界→`-ENXIO`、`reg>=VREG_COUNT`→`-EIO`、未知 size→`-EOPNOTSUPP`，与注释一致。
- `sensor_irq_thread` 持 `sd->lock` 调 `regmap_read`（会睡眠，但位于线程化下半部，合法）；与 `virt_i2c.chip->lock` 无嵌套关系，无锁序反转。
- `sensor_release` 先 `sensor_fasync(-1,file,0)` 再 `kfree(sf)`（避免 UAF），正确。
- `sensor_ioctl` SET_INTERVAL 先改值再 `hrtimer_cancel`+`hrtimer_start`，语义正确；未知命令返回 `-ENOTTY`。
- 阶段 01 契约项全部保留未回归（01 测试 16/16）。

---

## 5. 独立复现结果

| 阶段 | 结果 | 日志 | 备注 |
|---|---|---|---|
| `02-i2c-driver` | ✅ 16 PASS / 0 FAIL，`[TEST:END]` 到达，panic=0，告警=0 | `logs/20260914-041031-02-i2c-driver.log:298` | 干净源码、重新编译打包后运行 |
| `01-io-models` | ✅ 16 PASS / 0 FAIL | `logs/20260914-041039-01-io-models.log:314` | 回归未破 |
| `smoke` | ✅ 15 PASS / 0 FAIL | `logs/20260914-041048-smoke.log:292` | 回归未破 |

## 6. 日志交叉核对（关键实测值）

| 关键量 | 实测值 | 证据（日志:行） |
|---|---|---|
| i2c 设备路径 | `/sys/bus/i2c/devices/0-0048`，`name=sensor-char` | `041031-02:272` |
| 驱动绑定 | `driver` 符号链接指向 `.../sensor_char` | `041031-02:270` |
| regmap debugfs | `/sys/kernel/debug/regmap/0-0048/registers` | `041031-02:277-278` |
| regmap 寄存器内容 | `0: 0181 1: 0fa1 2: 0000`（温度 24.062℃ / 湿度 40.01%RH / 配置 0x0000） | `041031-02:279` |
| 温度值 | 正确实现：raw 0x0181 → 24.062℃（落在 24.0~26.0℃）；错误实现（NC-A）：`temp=2096.062` | `041031-02:279`；`040716-02:292` |
| 注入后 i2c_err 增量 | `errors: 0→3`（恰好 +3），`sensor i2c_err: 0→3` | `040826-02b:266,298,303,306` |
| 故障注入逐次日志 | `injected error ... (left=2/1/0)` | `040826-02b:279,284,289` |
| DT 周期 | `interval=500`（回到内核当前值，非 printk） | `041031-02:285` |

---

## 7. 遗留疑点

1. **F1 的 `docs/kb/02-*`** 在本次验证时不存在，需确认是「漏做」还是「尚未产出」；`docs/impl/02-*` 已于 04:11:37 由并行工作树补齐并经本报告交叉核对（第 10 节）。
2. **F2 的取样窗口**是否要修：修法有两种——(a) 末尾重新 `dmesg`（与 smoke 一致）；(b) 用 `dmesg -c` 在测试开头清空再在末尾重抓。当前不修则存在假绿。
3. **真实 I2C 控制器语义保真度**：`smbus_xfer` 直接操作 `data->word`，未实现 `I2C_FUNC_SMBUS_QUICK/BLOCK/PROCESS_CALL`；`i2c-stub` 已从 modules.load 移除但仍在产物里（F10）。这些不影响阶段 02 的验收目标。
4. **多核并发未验证**：QEMU 固定 `-cpu cortex-a72` 单核（`nr_cpu_ids=1`），`virt_i2c` 的互斥与注入计数、`sensor_char` 的 `open_count` 自增在 SMP 下的行为未实测（阶段 03 会做多进程并发）。
5. **`/dev/sensor0` 被占用时 rmmod** 的行为（cdev 已删、fd 残留）未测试；属通用内核语义，非本阶段契约。
6. **F5 的能力位 over-claim** 是否会在阶段 05（IIO 复用同一 virt_i2c）造成影响，需在阶段 05 关注。

---

## 8. Definition of Done 五条逐条回答（docs/10 第五节）

| # | DoD 条目 | 判定 | 依据 |
|---|---|---|---|
| 1 | `make` 无 error、无 warning（新增代码部分） | ✅ 满足 | 三次干净构建（含负控前后）`13-vm-fast-cycle.sh` 输出中无 `error/warning` 行；只打印 `CC [M]`/`LD [M]` 与生成物清单 |
| 2 | `22-macos-run-test.sh <本阶段>` 全部 PASS 且跑到 `[TEST:END]` | ✅ 满足 | `logs/20260914-041031-02-i2c-driver.log:298` = `pass=16 fail=0`（干净源码，本验证者独立复现） |
| 3 | `smoke` 全部 PASS（回归未破） | ✅ 满足 | `logs/20260914-041048-smoke.log:292` = `pass=15 fail=0`；另 01 回归 16/16 |
| 4 | panic=0；WARNING/BUG/Call trace 需解释或修掉 | ⚠️ **部分满足** | 干净源码三次运行 panic=0、WARNING=0（满足字面）。**但**02 脚本自身的告警检查取样窗口为 0（F2），负控 D 证明它能对测试体内的真实 WARNING 报「通过」；机制上不可靠，需修 |
| 5 | 三份文档落盘（impl/kb/verify）且结论可被日志复现 | ⚠️ **部分满足** | `docs/verify/02-*.md`（本报告）✅；`docs/impl/02-i2c-driver-实现记录.md` ✅（04:11:37 落盘，其引用的日志与行号经本报告逐条复查，见第 10 节）；`docs/kb/02-*.md` **不存在** ❌ |

---

## 9. 总体结论

- **实现侧**：阶段 02 的全部契约项均已落地，未发现功能性缺陷。字符设备/IO 模型/poll/fasync/线程化中断均未回归。
- **测试侧**：负控 A/B 证明核心链路（regmap 字节序、设备树枚举 + i2c_driver 绑定）**确实被测住**；负控 C/E 由验证者补充，分别证明故障注入「恰好 N 次」语义正确、重复装卸安全。
- **不通过的原因（两项必修）**：
  1. **F1**：缺 `docs/kb/02-*`（违反 DoD #5）。`docs/impl/02-*` 已由并行工作树补齐。
  2. **F2**：`tests/phases/02-i2c-driver.sh` 的「无内核告警」检查取样窗口为 0，已用负控 D 实证会对真实 WARNING 报假绿。
- **建议（非阻塞）**：修 F3/F4 的断言强度或改名，避免「名不副实」的检查项；F5 的能力位、F7 的日志语义建议一并修正。

---

## 10. 对 `docs/impl/02-i2c-driver-实现记录.md` 的交叉核对

该文档于 04:11:37（本验证进行中）由并行工作树产出，属「阶段 02 产出物」的一部分，故一并核对其
「结论必须能被日志复现」（DoD #5）：

| 文档声明 | 核对结果 |
|---|---|
| 引用日志 `20260914-040846-02-i2c-driver.log` 第 298 行为 `pass=16 fail=0` | ✅ 实际文件存在，第 298 行为 `[TEST:END] 02-i2c-driver pass=16 fail=0` |
| 引用第 258/260/279/285 行的四个关键数值 | ✅ 逐行命中（`of_node=/virt-i2c`、`addr=0x48 interval=500ms`、`0: 0181 1: 0fa1 2: 0000`、`interval=500`） |
| 引用 `20260914-040851-01-io-models.log` / `20260914-040900-smoke.log` | ✅ 两文件均存在 |
| 引用 `20260914-032815-02-i2c-driver.log:292` 为字节序 FAIL | ✅ 第 292 行确为 `实测 temp=2096.062` |
| 引用 `20260914-040748-02-i2c-driver.log` 为 of_node 负控 4/10 | ✅ 与本报告负控 B 一致 |
| 声称「本轮只写文档、未修改代码」 | ✅ 与本验证观察一致（`git diff` 全程为空，仅本报告与 impl 文档为新增未跟踪文件） |
| §6.4 已主动说明「并行验证工作树的负控日志会造成相邻时间结论相反」 | ✅ 与 `logs/` 实际相符，说明属实 |
| §7.1 已主动记录 CONFIG 语义三处不一致 | ✅ 与本报告 F7 一致（该隐患已被实现方文档自陈） |

