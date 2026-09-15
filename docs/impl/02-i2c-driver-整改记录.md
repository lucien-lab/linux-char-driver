# 阶段 02 整改记录（虚拟 I2C 控制器 + i2c_driver + regmap）

> 整改依据：`docs/verify/02-i2c-driver-验证报告.md`（独立验证者结论「不通过，整改后可转通过」）
> 整改范围：测试质量 3 项（F1~F3，核心）+ 代码质量 5 项（F4~F8）+ 额外 2 项（报告 F10/F11）
> 整改时间：2026-09-14
> 内核 / 运行环境：6.6.156 / aarch64 / QEMU `-M virt -cpu cortex-a72 -accel tcg`
> 基线 commit：`b55a25c`（整改在未提交的工作区上进行，本次只改代码与测试，未 `git commit`）

---

## 0. 编号对照表（本记录 ↔ 验证报告）

验证报告用了自己的 F 编号，为避免混淆，下表给出对照。**本记录沿用整改任务书的编号**。

| 本记录 | 验证报告 | 严重度 | 一句话 |
|---|---|---|---|
| **F1** | F2 | 中 | 「无内核告警」检查取样窗口为 0 → 假绿 |
| **F2** | F3 | 中 | 检查项名声称验证 `adapter.of_node`，实际只 grep adapter 注册行 |
| **F3** | F4 | 低 | regmap 两项检查只判「存在/非空」，不校验数值语义 |
| **F4** | F5 | 低 | `functionality` 多声明 `I2C_FUNC_SMBUS_BYTE`（over-claim） |
| **F5** | F6 | 低 | `stats` 读缓冲累积无下界保护（`768 - n` 可下溢） |
| **F6** | F7 | 低 | CONFIG 寄存器语义三处互相矛盾（注释/日志/写入值） |
| **F7** | F8 | 低 | `local_irq_disable/enable` 未用 `_save/restore` |
| **F8** | F9 | 低 | probe 失败路径留下悬空 `i2c_setclientdata`；`regmap_write` 返回值未检查 |
| 额外-1 | F10 | 低 | 仍无条件打包已弃用的 `i2c-stub.ko` |
| 额外-2 | F11 | 低 | 测试不回显用户态温度读数，日志无法直接查证 |
| — | F1 | 高 | 缺 `docs/kb/02-*` → 已由并行工作树产出的 `docs/kb/02-i2c-driver-知识点.md` 解决（不属本次代码整改） |

---

## 1. 测试质量类（核心：让检查项真的能抓住错误）

### F1 — 「无内核告警」取样窗口为 0（假绿）

- **问题**：`tests/phases/02-i2c-driver.sh` 只在脚本开头 `dmesg > /tmp/probe.txt`，
  第 6 节直接复用这个文件做告警判定。测试体（read / regmap / 中断触发）执行期间产生的
  WARNING 完全落在取样窗口之外 → 内核真的告警了也报「通过」。
- **根因**：取样时机错误。告警检查必须在**被测行为发生之后**取样。
  （对比：`smoke.sh` 在末尾重新 `dmesg`，所以它有效——这正是验证报告里同一二进制
  跑 02 是 16/16、跑 smoke 是 14/1 的原因。）
- **修法**：`tests/phases/02-i2c-driver.sh:106-116`
  测试体结束后重新 `dmesg > /tmp/dmesg-end.txt` 再判定，并把告警上下文（最多 5 行）
  打进串口日志便于事后定位。
- **负控证据（NC1）**：

  | 实验 | 改动 | 结果 | 抓住它的检查项 |
  |---|---|---|---|
  | NC1 | `sensor_read()` 开头插入 `WARN_ON_ONCE(1)` | **18 PASS / 1 FAIL**（`logs/20260914-104142-02-i2c-driver.log`） | `[CHECK:FAIL] dmesg 无 WARNING/Call trace（取样覆盖整段测试） -- 期望='0' 实际='2'` |
  | NC1 恢复 | 还原 `sensor_read()` | **19 PASS / 0 FAIL**（`logs/20260914-104156-02-i2c-driver.log`） | — |

  同一份「有 WARN」的二进制在整改前跑 02 是 16/16 全绿（验证报告负控 D，`logs/20260914-041005-02-i2c-driver.log:289` 有 WARNING、`:326` 却 PASS）。
  **整改后同场景直接 FAIL —— 假绿已消除。**

### F2 — 检查项名不副实（`adapter.of_node` 没被真正验证）

- **问题**：检查项叫「控制器注册日志（adapter 带 of_node）」，断言内容却是
  `grep "registered i2c adapter i2c-"`。而驱动那行日志里打印的 `of_node=` 取的是
  `pdev->dev.of_node`，**不是** `adapter.of_node`（真正被 i2c 核心用于枚举子节点的字段）。
  验证报告负控 B 实证：删掉 `adapter.of_node` 赋值后该检查仍 PASS。
- **根因**：断言对象选错——把「平台设备有设备树节点」误当成「adapter 有设备树节点」。
- **修法**：
  - `driver/virt_i2c.c:383-384`：日志改为打印 `chip->adap.dev.of_node`（用 `%pOF`），
    并加注释说明为什么必须打印这个字段而不是 `pdev->dev.of_node`（否则故障在日志上不可见）；
  - `tests/phases/02-i2c-driver.sh:27`：断言改为 `adapter.of_node=/virt-i2c`。
- **负控证据（NC2）**：

  | 实验 | 改动 | 结果 | 抓住它的检查项 |
  |---|---|---|---|
  | NC2 | 删除 `chip->adap.dev.of_node = pdev->dev.of_node;` | **3 PASS / 12 FAIL**（`logs/20260914-104211-02-i2c-driver.log`） | 第 1 条即 `adapter.of_node 已指向设备树节点…`（日志 `:262`），随后 11 条连锁失败 |
  | NC2 恢复 | 还原赋值行 | **19 PASS / 0 FAIL**（`logs/20260914-104221-02-i2c-driver.log`） | — |

  `%pOF` 对 NULL 节点的实测输出是 `(null)`（NC2 日志 `:258`：`adapter.of_node=(null)`），
  与正常值 `adapter.of_node=/virt-i2c`（恢复后日志 `:258`）可区分，因此该断言对「漏设 of_node」
  这类真实故障有效。

### F3 — regmap 检查只判「存在/非空」

- **问题**：原检查只断言 `/sys/kernel/debug/regmap/<bus>-0048/registers` 存在且非空。
  验证报告负控 A 实证：字节序错误导致内容变成 `0: 8101 1: a10f` 时，这两项依然 PASS，
  真正兜底的只有「温度落在 24.0~26.0 ℃」这一条。
- **修法**：`tests/phases/02-i2c-driver.sh:53-86` 增加三条**数值语义**断言（解析三行寄存器值后换算）：

  | 检查项 | 判定依据 |
  |---|---|
  | `regmap 温度 raw 语义正确` | `raw*1000/16` 落在 24000~26000 毫摄氏度（芯片模拟区间 24.0~26.0 ℃） |
  | `regmap 湿度 raw 语义正确` | raw 落在 4000~4199（= 40.00~41.99 %RH） |
  | `regmap CONFIG 语义正确` | 寄存器值必须等于 `0001`（bit0=1 连续转换使能） |

- **负控证据（NC3）**：

  | 实验 | 改动 | 结果 | 抓住它的检查项 |
  |---|---|---|---|
  | NC3 | 删除 `.val_format_endian = REGMAP_ENDIAN_LITTLE`（回默认 BIG） | **16 PASS / 3 FAIL**（`logs/20260914-104236-02-i2c-driver.log`） | ① `regmap 温度 raw 语义正确 -- 0x8301 → 2096062 m℃ 超出 24.0~26.0 ℃` ② `regmap 湿度 raw 语义正确 -- 0xa30f → 41743 超出 40.00~41.99 %RH 区间` ③ `温度读数落在芯片模拟区间 … 实测 temp=2176.062` |
  | NC3 恢复 | 还原 endian 声明 | **19 PASS / 0 FAIL**（`logs/20260914-104243-02-i2c-driver.log`） | — |

  同一次 NC3 中 `registers 内容：0: 8101 1: a10f 2: 0001`（`logs/20260914-104236-02-i2c-driver.log`），
  证明 regmap 层确实被字节交换；整改前这种状态只有 1 条检查能抓住，**现在有 3 条**。

---

## 2. 代码质量类

### F4 — `functionality` 能力位 over-claim

- **位置/改法**：`driver/virt_i2c.c:220-229` 删除 `I2C_FUNC_SMBUS_BYTE`，
  只保留 `I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA`。
- **理由**：芯片是「寄存器型」器件，所有访问都带寄存器地址（命令字节）；
  无命令字节的 SMBus Byte 协议（`size = I2C_SMBUS_BYTE`）在 `smbus_xfer()` 里没有对应分支，
  会走 `default:` 返回 `-EOPNOTSUPP`。**声明了却不实现属于 over-claim**：
  真实客户端调用 `i2c_smbus_read_byte()` 会先被能力位"骗过"，再拿到一个莫名其妙的 EOPNOTSUPP。
  选择「删声明」而不是「补实现」，因为对带寄存器空间的芯片实现无寄存器访问没有语义。
  注释里同时保留了「为什么也不能声明 `I2C_FUNC_I2C`」（会让 regmap 改走 raw `i2c_transfer`）。
- **验证**：三项测试全绿（regmap 仍走 SMBus word 路径，`registers` 读数不变）。

### F5 — `stats` 读缓冲下界保护

- **位置/改法**：`driver/virt_i2c.c:245-268` 新增 `VIRT_I2C_STATS_BUF`（1024），
  并用 `size_t off / rem` 剩余空间递减替代原来的 `768 - n`，循环条件加 `&& rem`。
- **理由**：原写法在 `n > 768` 时，`size_t` 的 `768 - n` 会**下溢成巨大值**，
  `scnprintf` 随即越界写。当前 `VBANK_MAX=4` 时最长约 420 字节不会触发，
  但一旦增加 bank 或字段就会踩雷——属于「现在没事、改了必炸」的隐患。
  新写法利用了 `scnprintf` 的截断语义（返回值 ≤ rem-1），数学上不可能下溢。
- **验证**：`stats` 接口读出的字段完整（测试脚本第 1 节的 debugfs 存在性检查 + 阶段 04 会做字段级验证）。

### F6 — CONFIG 寄存器语义三处矛盾

- **问题**：三处对同一 bit 的解释不一致：
  1. `driver/virt_i2c.c` 文件头寄存器图：`bit0=连续转换使能`；
  2. `driver/virt_i2c.c` probe 里上电默认值注释写成「连续转换**关**」（值却是 `0x0001`）；
  3. `driver/sensor_char.c` probe 日志打印 `config=0x0001 -> enable continuous conversion`，
     紧接着却 `regmap_write(..., 0x0000)`（按定义等于关闭）。
- **修法**：统一为 **bit0=1 表示使能**，并让三处互相引用：
  - `driver/virt_i2c.c:344`：上电默认注释改为「连续转换使能（bit0=1，见文件头寄存器图中的 CONFIG 定义）」；
  - `driver/virt_i2c.c:30`：寄存器图补上「**bit0 = 连续转换使能：1=使能，0=关闭**」并注明"语义必须三处一致"；
  - `driver/sensor_char.c:77`：新增 `SENSOR_CFG_CONT_EN 0x0001` 宏（带一致性注释）；
  - `driver/sensor_char.c:636-654`：读回 config → 打印真实状态（`already enabled` / `off`）→
    写入使能值 → 打印 `continuous conversion enabled (config=0x0001)`。
- **验证**：测试新增两条断言（`probe 把芯片配置成连续转换（bit0=1）`、
  `regmap CONFIG 语义正确（0x0001）`），实测 `registers` 中 `2: 0001`；
  NC3 中该值仍正确（`2: 0001`），说明它独立于字节序检查、是有效的新增约束。

### F7 — `local_irq_disable/enable` → `local_irq_save/restore`

- **位置/改法**：`driver/sensor_char.c:429-446`（`sensor_write()` 手动触发中断的路径）
  改为 `local_irq_save(flags)` / `local_irq_restore(flags)`，并更新注释。
- **判断依据（为什么不按「教学设计保留」处理）**：读代码意图——这段关中断是
  **为了让调用合法**（避免内核报 `irq N handler … enabled interrupts`），并不是在复现该 WARNING；
  该 WARNING 的复现与讲解已经在注释里以及 `docs/03-调试方法手册.md` 里。
  因此这里不存在"刻意的教学瑕疵"，只有「`enable` 会无条件开中断、冲掉调用者原有中断屏蔽状态」
  这个真实的编码规范问题；`save/restore` 保持同样的行为语义且不破坏调用者状态。
- **验证**：`write(0x01)` 手动触发路径仍工作（smoke 与 02 中的触发检查通过，ftrace 仍抓到
  `sensor_irq_thread`），且 dmesg 无 `enabled interrupts` 告警。

### F8 — probe 失败路径与寄存器写返回值

- **位置/改法**：`driver/sensor_char.c:626-708`
  - 新增统一失败出口 `err_clear_clientdata:`，在其中 `i2c_set_clientdata(client, NULL);`；
    regmap 初始化失败、芯片无应答、配置写失败、字符设备注册失败全部 `goto` 到该标签
    （原有的 `err_remove_domain` / `err_unreg_chrdev` 两级回滚保留，最后统一落到新标签）；
  - `driver/sensor_char.c:649-653`：检查 `regmap_write(SENSOR_REG_CONFIG, SENSOR_CFG_CONT_EN)`
    的返回值，失败即 `dev_err` + 让 probe 失败。
- **理由**：probe 失败后 `sd` 会被 devm 释放，但 `client` 的 drvdata 仍指向它——
  虽然当前没有代码路径再取用，但这属于「悬空指针留在系统里」，是 review 会打回的问题；
  配置写失败被静默忽略则会让设备带着未配置状态继续跑，用户态只能看到一堆读失败。
- **验证**：正常路径（probe 成功）三测全绿；失败路径的分支行为由「配置写入成功」这一新增日志 +
  `regmap CONFIG 语义正确` 检查间接覆盖（写失败会使该检查按预期失败）。

---

## 3. 额外修复

### 额外-1（报告 F10）— 只打包真正被引用的内核树模块

- **位置/改法**：`scripts/13-vm-fast-cycle.sh:88-117`
  把「模块清单计算」提前到「模块拷贝」之前，并改成
  `if grep -q '^i2c-stub\.ko' "$ROOTFS/etc/modules.load"; then cp …`。
  同时删除了因为本次重排而产生的重复模块清单代码块（用 `grep -c 模块加载顺序` 确认只剩 1 处）。
- **理由**：阶段 02 起 `driver/modules.load` 已不含 `i2c-stub`（`virt_i2c` 提供带设备树节点的适配器），
  无条件拷贝会让 initramfs 里躺着一个用不到的桩模块，读者容易误以为流程还在用它。
  阶段 01 及更早的配置（清单里仍有 i2c-stub）行为不变。
- **验证**：
  - 构建输出：`/etc/modules.load (来源: modules.load)` 只含 `virt_i2c.ko` / `sensor_char.ko`；
  - **直接核对产物**（对 `artifacts/initramfs.cpio.gz` 解包）：

    ```
    lib/modules/6.6.156/virt_i2c.ko
    lib/modules/6.6.156/sensor_char.ko
    → grep -c "i2c-stub" → 0（桩模块已不再打包）
    /etc/modules.load 内容：virt_i2c.ko / sensor_char.ko
    ```
  - 三项测试全绿；`bash -n scripts/13-vm-fast-cycle.sh` 语法检查通过；
    `grep -c "模块加载顺序" scripts/13-vm-fast-cycle.sh` = 1（重排产生的重复代码块已删除）。
  - 附：initramfs 里同时包含 `tests/phases/{01-io-models,02-i2c-driver,smoke}.sh`，
    这也是「一次构建可用内核参数 `test=` 依次跑多个阶段」的基础（见 `docs/10-开发与验证守则.md` 第三节）。

### 额外-2（报告 F11）— 回显用户态温度读数

- **位置/改法**：`tests/phases/02-i2c-driver.sh:98`
  在跑完 `sensor_test` 后增加 `info "sensor_test 温度读数：$(grep -o 'temp=[0-9.-]*' …)"`。
- **理由**：此前日志里看不到用户态实际读到的温度，「温度正确」只能从 regmap `registers`
  反推，审计不便。现在串口日志里有直接的数值证据。

---

## 4. 负控实验汇总（本记录的核心产出）

**每一项都是：改坏源码 → 重新编译打包 → 跑测试 → 还原 → 复跑确认全绿。**
所有临时改动都已还原，并用 `diff` 与负控前的备份逐字节比对确认（见第 5 节）。

| 编号 | 对应问题 | 破坏点 | 破坏后结果 | 抓住它的检查项 | 恢复后结果 |
|---|---|---|---|---|---|
| **NC1** | F1 | `sensor_read()` 插入 `WARN_ON_ONCE(1)` | 18 PASS / **1 FAIL**<br>`logs/20260914-104142-…02-i2c-driver.log` | `dmesg 无 WARNING/Call trace（取样覆盖整段测试）`（期望 0 实际 2） | 19 / 0 ✔<br>`…104156-…` |
| **NC2** | F2 | 删除 `chip->adap.dev.of_node = …` | 3 PASS / **12 FAIL**<br>`logs/20260914-104211-…02-i2c-driver.log` | `adapter.of_node 已指向设备树节点`（日志 `:258` 打印 `adapter.of_node=(null)`） | 19 / 0 ✔<br>`…104221-…` |
| **NC3** | F3 | 删除 `.val_format_endian = REGMAP_ENDIAN_LITTLE` | 16 PASS / **3 FAIL**<br>`logs/20260914-104236-…02-i2c-driver.log` | `regmap 温度 raw 语义正确`、`regmap 湿度 raw 语义正确`、`温度读数落在…` | 19 / 0 ✔<br>`…104243-…` |

**对照结论**：

| 问题 | 整改前 | 整改后 |
|---|---|---|
| F1 告警取样窗口 | 内核有 2 条 WARNING 仍显示 16/16 全绿（验证报告负控 D） | 同场景 **FAIL**（NC1） |
| F2 `adapter.of_node` | 删掉赋值后该检查仍 **PASS** | 删掉后 **FAIL**（NC2），且日志能直接看出 `(null)` |
| F3 regmap 数值 | 字节交换后检查仍 PASS（仅温度一条兜底） | 字节交换后 **3 条 FAIL**（NC3） |

---

## 5. 恢复完整性校验

负控实验的临时改动全部还原，并做了逐字节比对与残留扫描：

```
diff -q /tmp/keep-sensor_char.c driver/sensor_char.c   → 一致 ✔
diff -q /tmp/keep-virt_i2c.c   driver/virt_i2c.c       → 一致 ✔
grep -c "WARN_ON_ONCE" driver/*.c                      → 0（负控痕迹已清除）
grep -n "val_format_endian" driver/sensor_char.c        → :595 仍在（已还原）
grep -n "adap.dev.of_node"  driver/virt_i2c.c           → :362 仍在（已还原）
git status --short                                      → 仅本次有意修改的 5 个文件 + 3 个新增文档，无多余改动
```

`git status` 明细（说明：`docs/11-阶段任务书.md` 的修改由集成负责人在本轮之前完成，
不属于本次整改；三个 `??` 文档是并行工作树产出的阶段 02 文档）：

```
 M docs/11-阶段任务书.md
 M driver/sensor_char.c
 M driver/virt_i2c.c
 M scripts/13-vm-fast-cycle.sh
 M tests/phases/02-i2c-driver.sh
?? docs/impl/02-i2c-driver-实现记录.md
?? docs/kb/02-i2c-driver-知识点.md
?? docs/verify/02-i2c-driver-验证报告.md
```

---

## 6. 最终验证结果（整改后，干净源码）

| 测试 | 结果 | 日志 | 变化 |
|---|---|---|---|
| `02-i2c-driver` | ✅ **19 PASS / 0 FAIL**，`[TEST:END]` 到达，panic=0，告警=0 | `logs/20260914-104259-02-i2c-driver.log` | 16 → 19（新增 3 条数值语义断言） |
| `01-io-models` | ✅ 16 PASS / 0 FAIL | `logs/20260914-104305-01-io-models.log` | 不变（阶段 01 未回归） |
| `smoke` | ✅ 15 PASS / 0 FAIL | `logs/20260914-104314-smoke.log` | 不变（回归未破） |

新增/强化的检查项（02，19 项中的关键几条）：

```
[CHECK:PASS] adapter.of_node 已指向设备树节点（枚举子设备的前提）
[CHECK:PASS] regmap 温度 raw 语义正确（0x0183 → 24187 m℃ 落在 24.0~26.0 ℃）
[CHECK:PASS] regmap 湿度 raw 语义正确（0x0fa3 → 4003，即 40.00~41.99 %RH）
[CHECK:PASS] regmap CONFIG 语义正确（0x0001：bit0=1 连续转换已使能）
[CHECK:PASS] probe 把芯片配置成连续转换（bit0=1）
[CHECK:PASS] dmesg 无 WARNING/Call trace（取样覆盖整段测试）
```

设备树属性回归、用户态通路、`sensor_test` 温度区间等原有检查全部保持通过。

---

## 7. 遗留风险与已知取舍

1. **regmap 数值断言的区间耦合**：温度/湿度区间断言依赖
   `virt_i2c_convert()` 的模拟区间（24.0~26.0 ℃ / 40.00~41.99 %RH）。
   若后续阶段（03/05）改动模拟芯片数据范围，必须同步更新这三条断言，
   否则会误报失败——这是"用语义断言换检测力"的必然代价，已在检查项名称里写明区间便于定位。
2. **`%pOF` 对 NULL 打印 `(null)`**：不同内核版本的打印实现可能不同
   （`of_node_full_name(NULL)` 在 6.6 返回 `<no-node>`，但 `%pOF` 路径实测输出 `(null)`）。
   断言写的是**正常值** `adapter.of_node=/virt-i2c`，因此不依赖 NULL 的具体措辞，跨版本稳健。
3. **NC1 采用的注入点**：只在 `sensor_read()` 注入一条 WARN 来证明取样窗口有效；
   更全面的"测试体内任意位置告警"未穷举，但取样窗口这一机制性缺陷已被覆盖。
4. **未验证项（沿用验证报告）**：真实 I2C 控制器的其他 SMBus 协议（QUICK/BLOCK/PROCESS_CALL）
   未实现也未声明；SMP 并发（QEMU 单核）与 `/dev/sensor0` 被占用时 `rmmod` 的行为未测——
   前者留待阶段 03 的多进程并发测试，后者属通用内核语义、非本阶段契约。
5. **F4 的影响面**：移除 `I2C_FUNC_SMBUS_BYTE` 后，若将来有真实客户端需要无命令字节访问，
   需要改为实现该 size 而不是重新声明能力位。当前 regmap 路径（WORD_DATA）不受影响。
