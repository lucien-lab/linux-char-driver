# 阶段 02 独立复验报告：整改是否真的修好了验证报告里的问题

> 复验者：独立复验 agent（不参与实现、不参与整改）
> 复验时间：2026-09-14（本轮日志时间戳 10:46~10:49）
> 被复验对象：工作区未提交状态（源码 = commit `b55a25c` + 整改改动 + 新增文档）
> 复验依据：`docs/verify/02-i2c-driver-验证报告.md`（原报告）、`docs/impl/02-i2c-driver-整改记录.md`（整改说明）、
> `docs/10-开发与验证守则.md`（纪律与 DoD）、`docs/11-阶段任务书.md`「阶段 02」
> 内核 / 环境：6.6.156 / aarch64 / QEMU `-M virt -cpu cortex-a72 -accel tcg`（单核）
> 编号约定：本报告沿用**整改记录**的编号（任务书也是这套），与原报告的对照见整改记录第 0 节。

## 复验方法（与整改者无关的独立路径）

1. **先重建基线**：不改任何源码，跑 `13-vm-fast-cycle.sh` → `21-macos-sync-artifacts.sh` → `22-macos-run-test.sh`，
   确认 artifacts 与工作区源码一致、三项测试全绿，作为后续所有对照的基准。
2. **逐条读代码**核对 F1~F8 的修法与「修没修」。
3. **重做负控**：每项都是「改坏 → 重新编译打包 → 跑测试 → 从备份逐字节还原 → 复跑确认全绿」，
   只信测试输出，不信整改文档的叙述。
4. 对整改文档中**只做了静态论证**的 F8（probe 失败路径）额外补做了一次独立负控（NC-D）。
5. 用 `shasum` + `git status` 证明还原后工作区逐字节回到复验前状态。

---

## 0. 本轮复验用到的全部日志

| 日志文件 | 用途 | 结果 |
|---|---|---|
| `logs/20260914-104604-02-i2c-driver.log` | 复验基线：02（干净源码） | 19 PASS / 0 FAIL |
| `logs/20260914-104609-01-io-models.log` | 复验基线：01 回归 | 16 PASS / 0 FAIL |
| `logs/20260914-104618-smoke.log` | 复验基线：smoke 回归 | 15 PASS / 0 FAIL |
| `logs/20260914-104636-02-i2c-driver.log` | **NC-A**（删 `adapter.of_node` 赋值） | 3 PASS / **12 FAIL** |
| `logs/20260914-104649-02-i2c-driver.log` | NC-A 恢复后复跑 | 19 PASS / 0 FAIL |
| `logs/20260914-104702-02-i2c-driver.log` | **NC-B**（删 `.val_format_endian`） | 16 PASS / **3 FAIL** |
| `logs/20260914-104712-02-i2c-driver.log` | NC-B 恢复后复跑 | 19 PASS / 0 FAIL |
| `logs/20260914-104726-02-i2c-driver.log` | **NC-C**（测试体内注入 `WARN_ON_ONCE`） | 18 PASS / **1 FAIL** |
| `logs/20260914-104739-02-i2c-driver.log` | NC-C 恢复后复跑 | 19 PASS / 0 FAIL |
| `logs/20260914-104847-02-i2c-driver.log` | **NC-D**（额外：强制 CONFIG 写失败，验 F8 probe 失败路径） | 8 PASS / **7 FAIL** |
| `logs/20260914-104902-02-i2c-driver.log` | **最终** 02（全部还原后） | 19 PASS / 0 FAIL |
| `logs/20260914-104907-01-io-models.log` | **最终** 01 回归 | 16 PASS / 0 FAIL |
| `logs/20260914-104916-smoke.log` | **最终** smoke 回归 | 15 PASS / 0 FAIL |

复现命令（全部走项目脚本，未绕过）：

```bash
limactl shell dev bash -c 'TEST=02-i2c-driver bash /Users/lucien/workspace/self-study/projects/linux-char-driver/scripts/13-vm-fast-cycle.sh'
bash scripts/21-macos-sync-artifacts.sh
bash scripts/22-macos-run-test.sh <阶段> 180
```

---

## 1. 原问题逐条复验表

| 编号（本报告/原报告） | 问题 | 整改说明的修法 | 复验结论 | 证据 |
|---|---|---|---|---|
| **F1** / 原 F2 | 「无内核告警」取样窗口为 0 → 假绿 | `tests/phases/02-i2c-driver.sh:106-116` 测试体结束后重新 `dmesg` 取样再判定 | **已修复** | 代码：`02-i2c-driver.sh:108-112`（末尾 `dmesg > /tmp/dmesg-end.txt` 后 `check_eq`）；负控 **NC-C** 实证 |
| **F2** / 原 F3 | 检查项名声称验证 `adapter.of_node`，断言却只 grep adapter 注册行 | 驱动日志改打 `chip->adap.dev.of_node`；断言改 `adapter.of_node=/virt-i2c` | **已修复** | 代码：`driver/virt_i2c.c:362` 赋值仍在、`:383-384` 打印 `chip->adap.dev.of_node`；`02-i2c-driver.sh:27` 断言；负控 **NC-A** 实证 |
| **F3** / 原 F4 | regmap 只判「存在/非空」，不校验数值 | 新增 3 条数值语义断言（温度/湿度/CONFIG） | **已修复** | 代码：`02-i2c-driver.sh:53-86`；负控 **NC-B** 实证（一次破坏抓到 3 条 FAIL） |
| **F4** / 原 F5 | `functionality` 多声明 `I2C_FUNC_SMBUS_BYTE`（over-claim） | 删除该位，只留 `BYTE_DATA|WORD_DATA` | **已修复** | `driver/virt_i2c.c:228` 返回两 bit；注释 `:216-227` 说明为何不能声明 BYTE / I2C_FUNC_I2C。`grep I2C_FUNC_SMBUS_BYTE driver/*.c` 仅命中注释 |
| **F5** / 原 F6 | stats 读缓冲用 `768 - n`，`n>768` 时 `size_t` 下溢 | 改 `off/rem` 剩余空间递减 + 循环条件 `&& rem` | **已修复** | `driver/virt_i2c.c:245` 宏 `VIRT_I2C_STATS_BUF 1024`、`:253` `rem` 初值、`:266/:278` `rem -= (size_t)n`、`:268` `for (… && rem; …)`；`grep '768 -'` 仅命中 `:242` 的说明性注释 |
| **F6** / 原 F7 | CONFIG 语义三处矛盾（注释/日志/写入值） | 统一为 bit0=1 使能，三处互相引用，probe 写使能值并回读打印 | **已修复** | `virt_i2c.c:30`（寄存器图）、`:344`（上电默认注释）、`sensor_char.c:77`（`SENSOR_CFG_CONT_EN`）、`:636-654`（回读→写 `0x0001`→打印）；测试新增 `:81` CONFIG==0001 与 `:90` 日志断言；干净日志 `104902:283` `regmap CONFIG 语义正确（0x0001）`、`:280 registers 2: 0001` |
| **F7** / 原 F8 | `local_irq_disable/enable` 无条件开中断 | 改 `local_irq_save/restore` | **已修复** | `driver/sensor_char.c:442/444`（`flags` 声明于 `:426`）；注释 `:437-441` 说明理由。全量日志 `grep "enabled interrupts" logs/*.log` = 0 命中 |
| **F8** / 原 F9 | probe 失败路径留下悬空 clientdata；`regmap_write` 返回值未检查 | 统一失败出口 `err_clear_clientdata` 清 clientdata；检查配置写返回值 | **已修复（并补做独立负控 NC-D）** | `driver/sensor_char.c:704-706`（`i2c_set_clientdata(client, NULL)`）；`:628/:639/:652/:660` 全部 `goto err_clear_clientdata`；`:649-653` 检查 `regmap_write`。**NC-D** 实证：强制写失败后 probe 中止（日志 `104847:262` `failed to enable continuous conversion: -5`，无 `probe done`、无 `/dev/sensor0`），且无 WARNING/BUG/UAF |
| **原 F1** | 缺 `docs/kb/02-*`（DoD#5 硬伤） | 由并行 lane 产出 | **已修复** | `docs/kb/02-i2c-driver-知识点.md` 存在（104 KB / 1761 行，含机制/源码行号/面试问答/亲手验证）；三份文档齐备 |
| **额外-1** / 原 F10 | 无条件打包已弃用的 `i2c-stub.ko` | 只在 `modules.load` 引用时才打包 | **已修复** | `scripts/13-vm-fast-cycle.sh:104-109`；独立解包本轮 `artifacts/initramfs.cpio.gz`：`find -name 'i2c-stub*' | wc -l` = **0**，`lib/modules/6.6.156/` 仅 `virt_i2c.ko`+`sensor_char.ko`，`etc/modules.load` 仅这两行 |
| **额外-2** / 原 F11 | 日志看不到用户态温度读数 | 测试中回显温度读数 | **已修复** | `02-i2c-driver.sh:98`；干净日志 `104902:296` 有 `sensor_test 温度读数：temp=24.500 …` |

**唯一未修复项：无。** F1~F8 全部落地，两个额外项也落地；原报告「不通过」的两个原因（缺 kb 文档、F2 假绿）均已消除。

---

## 2. 复做的负控实验结果表（复验核心）

每项负控都记录了「改了什么 → 哪条检查项抓住它 → 日志文件:行号」。三位负控均**独立复做**，
不是引用整改记录的结论。

| 编号 | 破坏点（临时改源码） | 结果 | 抓住它的检查项（日志:行） | 恢复后 |
|---|---|---|---|---|
| **NC-A**（对应 F2） | 删除 `driver/virt_i2c.c:362` `chip->adap.dev.of_node = pdev->dev.of_node;` | 3 PASS / **12 FAIL**（`[TEST:END] pass=3 fail=12`，`logs/20260914-104636-02-i2c-driver.log:287`） | 第 1 条即 `[CHECK:FAIL] adapter.of_node 已指向设备树节点（枚举子设备的前提）`（`104636:262`）；同一日志 `:258` 打印 `adapter.of_node=(null)`，故障在日志上直接可见。随后 11 条连锁失败：`子节点 sensor@48 被实例化成 i2c 从设备`（`:266`）、`driver 符号链接`（`:270`）、`probe 由 i2c 核心调用`（`:271`）、`regmap 节点`（`:273`）、`chip detected`（`:275`）、`probe 把芯片配置成连续转换`（`:276`）、`poll-interval-ms`（`:278`）、`sensor_test 退出码`（`:279`）、`温度区间`（`:281`）、`测试正常结束`（`:282`） | ✔ `logs/20260914-104649-02-i2c-driver.log:303` = `pass=19 fail=0` |
| **NC-B**（对应 F3） | 删除 `driver/sensor_char.c:595` `.val_format_endian = REGMAP_ENDIAN_LITTLE,` | 16 PASS / **3 FAIL**（`[TEST:END] pass=16 fail=3`，`logs/20260914-104702-02-i2c-driver.log:303`） | ① `[CHECK:FAIL] regmap 温度 raw 语义正确 -- 0x8301 → 2096062 m℃ 超出 24.0~26.0 ℃`（`104702:281`）② `[CHECK:FAIL] regmap 湿度 raw 语义正确 -- 0xa30f → 41743 超出 40.00~41.99 %RH 区间`（`:282`）③ `[CHECK:FAIL] 温度读数落在芯片模拟区间 24.0~26.0 ℃ -- 实测 temp=2176.062`（`:297`）。同日志 `:280` `registers 内容：0: 8101 1: a10f 2: 0001`（字节被 swab16） | ✔ `logs/20260914-104712-02-i2c-driver.log:303` = `pass=19 fail=0` |
| **NC-C**（对应 F1） | 在 `driver/sensor_char.c:sensor_read()` 函数体（`int len, ret;` 之后）插入 `WARN_ON_ONCE(1);`，使 WARNING 产生在**测试体执行期间** | 18 PASS / **1 FAIL**（`[TEST:END] pass=18 fail=1`，`logs/20260914-104726-02-i2c-driver.log:337`） | `[CHECK:FAIL] dmesg 无 WARNING/Call trace（取样覆盖整段测试） -- 期望='0' 实际='2'`（`104726:331`）；`:332-333` 打印告警上下文，`:293` 为 `WARNING: CPU: 0 PID: 104 at .../sensor_char.c:338 sensor_read+0x54/0x278 [sensor_char]`。外层 `22-macos-run-test.sh` 也报 `内核告警: 4` 且退出码非 0 | ✔ `logs/20260914-104739-02-i2c-driver.log:303` = `pass=19 fail=0` |
| **NC-D**（额外，对应 F8） | 在 `virt_i2c_smbus_xfer()` 里强制「写 CONFIG 寄存器返回 -EIO」，制造 probe 中途失败 | 8 PASS / **7 FAIL**（`logs/20260914-104847-02-i2c-driver.log:291`） | `104847:261` 先 `chip detected: config=0x0001 (continuous conversion already enabled)`，`:262` 立刻 `failed to enable continuous conversion: -5`；随后**没有** `probe done` 行，`driver 符号链接`（`:274`）、`regmap 节点`（`:277`）、`probe 把芯片配置成连续转换`（`:280`）、`/dev/sensor0` 相关（`:283/:285`）全部 FAIL。证明 `regmap_write` 返回值确实被检查、失败即中止 probe，且未产生 WARNING/BUG/UAF | ✔ 还原后 `logs/20260914-104902-02-i2c-driver.log:303` = `pass=19 fail=0` |

**复做负控的结论**：三项检查（F1 告警取样、F2 of_node 断言、F3 regmap 数值）**都确认有效**——
破坏即 FAIL，且失败项的名字能直接读出原因；还原即全绿。整改记录里 NC1/NC2/NC3 的结论与本次独立复做**一致**
（NC-A 3/12、NC-B 16/3、NC-C 18/1），只是日志文件名不同（本轮时间戳 10:46~10:47）。

### 2.1 负控实验的还原完整性

- 负控前后对 `driver/*.c` 与 `tests/phases/*.sh` 取 `shasum`：**完全一致**（`HASHES-IDENTICAL`）。
- `grep -rn "NEGATIVE-CONTROL" driver/ tests/` → **无残留**；`grep -c WARN_ON_ONCE driver/*.c` 中负控痕迹 → 0。
- `git status --short` 与复验开始时**逐条一致**（5 个 `M` + 4 个 `??`，无新增/无遗漏），`git diff --cached` 为空（无暂存文件）。
- 所有负控后的产物都已用干净源码重新构建并同步；最终 artifacts 由 `104902` 那次绿色运行使用。

---

## 3. 独立复现结果

| 阶段 | 结果 | 日志 | 备注 |
|---|---|---|---|
| `02-i2c-driver` | ✅ **19 PASS / 0 FAIL**，到 `[TEST:END]`，panic=0，告警=0 | `logs/20260914-104902-02-i2c-driver.log:303` | 干净源码重建后运行；检查项由 16 增至 19（新增 3 条数值断言，无删除） |
| `01-io-models` | ✅ 16 PASS / 0 FAIL | `logs/20260914-104907-01-io-models.log` | 阶段 01 未回归 |
| `smoke` | ✅ 15 PASS / 0 FAIL | `logs/20260914-104916-smoke.log` | 回归未破；`smoke.sh` 本轮未被改动（`git status` 无该文件） |

复现的一致性前提已核对：`13-vm-fast-cycle.sh` 重新编译并打包（2 个模块、3 个用户态程序、`/etc/modules.load` = `virt_i2c.ko`+`sensor_char.ko`），
`21` 同步后 `artifacts/initramfs.cpio.gz` 时间戳 10:49，与最终三跑使用同一份产物。构建输出中 `error/warning` 命中 **0 行**。

---

## 4. 新发现问题（按严重度）

复验对整改改动做了代码级复查（新增/改动的锁、错误路径、资源释放、是否削弱其它检查项），结论如下。

### 4.1 低：`docs/kb` 与 `docs/impl` 未随整改同步，存在与当前代码不符的陈旧结论（N1）

两份文档成文于整改之前（mtime 04:13 / 04:19），整改后**未更新**，与当前源码/实测日志矛盾：

| 位置 | 文档里的说法 | 当前实际 |
|---|---|---|
| `docs/kb/02-i2c-driver-知识点.md:211-212, 563` | `functionality` 声明 `SMBUS_BYTE` | 已删除（`virt_i2c.c:228` 只有两个 bit） |
| `docs/kb/…:1491, 1532`；`docs/impl/…:545` | CONFIG = `0x0000`、probe 写 `0x0000` | probe 写 `0x0001`，实测 `registers 2: 0001`（`104902:280`） |
| `docs/kb/…:314` | probe 日志 `chip detected: config=0x0001 -> enable continuous conversion` | 已改为 `chip detected: config=0x0001 (continuous conversion already enabled)` |
| `docs/impl/…:746` | 把 CONFIG 三处语义不一致列为「遗留隐患（建议二选一）」 | 已修复（整改 F6） |

影响：DoD#5 要求「文档里的结论必须能被日志复现」。这些结论引用的旧日志文件确实存在（引用可回溯），
但读者按 `docs/kb` §7「亲手验证」用**当前代码**复跑会看到 `2: 0001` 而不是文档写的 `2: 0000`。
严重度低（功能与测试均不受影响，`整改记录` 已把 delta 写清楚），但建议整改者同步更新两份文档以闭合 DoD#5。

### 4.2 低：测试脚本注释里的 `%pOF` NULL 措辞不准（N2）

`tests/phases/02-i2c-driver.sh:27-28` 注释称「`%pOF` 对 NULL 打印 `<no-node>`」，
实测（`104636:258`）打印的是 `adapter.of_node=(null)`。
不断言 NULL 的任何措辞（断言的是正常值 `adapter.of_node=/virt-i2c`），因此**不影响检查有效性**，仅注释不准。

### 4.3 低：`tests/runner/init.sh` 顶部注释仍描述 i2c-stub 加载顺序（N3）

`tests/runner/init.sh` 注释写「`i2c-stub(虚拟 I2C 总线) -> 传感器驱动`」，阶段 02 起实际顺序是
`virt_i2c.ko -> sensor_char.ko`。属陈旧注释，无功能影响。

### 4.4 提示：数值断言的区间耦合与条件性判定（N4，非缺陷）

- 新增 3 条 regmap 数值断言与模拟区间硬绑定（24.0~26.0 ℃ / 40.00~41.99 %RH），整改记录 §7.1 已自陈；
  阶段 03/05 若改 `virt_i2c_convert()` 的区间，必须同步改断言（否则误报）。
- `regmap registers 三个寄存器值可解析` 这条 FAIL 只在 `REGMAPDIR` 非空时参与判定；
  regmap 节点整体缺失时由 `check_nonempty`（`02-i2c-driver.sh:45`）兜底，未形成新的假绿缺口。

### 4.5 提示：`artifacts/sensor_char.ko` 是旧产物（N5，仓库卫生）

`artifacts/sensor_char.ko` 时间戳为 9-13 13:43，`scripts/21-macos-sync-artifacts.sh` 只回传
`initramfs.cpio.gz / Image / i2c-stub.ko`，不更新该文件。运行链只依赖 initramfs 内模块，**不影响任何结论**；
但它会误导「产物是否最新」的判断，建议要么让 21 同步 `.ko`，要么从 artifacts 移除。

### 4.6 复查确认「没有引入新问题」的部分（排除的假设）

- **锁**：`virt_i2c_stats_read` 仍在 `chip->lock` 内做 `scnprintf`、`kfree` 在解锁之后；`rem` 在每次
  `scnprintf` 后按返回值递减，数学上 `rem >= 1` 恒成立（`scnprintf` 返回 ≤ size-1），**不会下溢**；
  `off` 始终 ≤ 1023，`simple_read_from_buffer(…, off)` 合法。`smbus_xfer` 新增分支（NC-D 用）已还原，原锁序未变。
- **错误路径/资源释放**：`sensor_probe` 的 `err_clear_clientdata` 位于 `err_unreg_chrdev` 之后，
  是唯一出口的收尾；`i2c_set_clientdata(client, NULL)` 在 `return` 前执行，无「清早了导致 remove 用到」的窗口
  （失败时不会调用 remove）。regmap 写失败改判为 probe 失败，属**增强**而非削弱。
- **是否削弱其它检查项**：逐条对比 `b55a25c` 与工作区的 02 检查清单——**没有任何检查项被删除或放宽**；
  唯一被替换的是第 1 条（`registered i2c adapter i2c-` → `adapter.of_node=/virt-i2c`）。该替换不削弱覆盖率：
  驱动把该 `dev_info` 放在 `i2c_add_adapter()` 成功之后（`virt_i2c.c:364` → `:383`），所以命中这行仍蕴含「adapter 注册成功」。
  告警检查从「只覆盖启动阶段」变为「覆盖启动 + 测试体」，是**严格增强**（NC-C 实证）。
  `smoke.sh` / `01-io-models.sh` 均未改动。
- **能力位变更的副作用**：删除 `I2C_FUNC_SMBUS_BYTE` 后 regmap 仍走 `regmap_smbus_word` 路径，
  `registers` 读数与温度换算均正常（`104902:280/296`），未破坏阶段 01/02 的任何通路。

---

## 5. Definition of Done 五条逐条回答（docs/10 第五节）

| # | DoD 条目 | 判定 | 依据 |
|---|---|---|---|
| 1 | `make` 无 error、无 warning（新增代码部分） | **通过** | 本轮 5 次干净构建（基线 1 次 + 3 次负控后恢复 + NC-D 恢复 1 次），输出中 `error/warning` 命中 0 行；构建打印 `CC [M]`/`LD [M]` 与 `sensor_char.ko (96K)`/`virt_i2c.ko (64K)` |
| 2 | `bash scripts/22-macos-run-test.sh <本阶段>` 全部 PASS 且跑到 `[TEST:END]` | **通过** | `logs/20260914-104902-02-i2c-driver.log:303` = `[TEST:END] 02-i2c-driver pass=19 fail=0`，`22` 脚本退出 0 |
| 3 | `smoke` 全部 PASS（回归未破） | **通过** | `logs/20260914-104916-smoke.log` = 15 PASS/0 FAIL；另 `01-io-models` 16 PASS/0 FAIL（`104907`） |
| 4 | `panic=0`；`WARNING/BUG/Call trace` 需解释或修掉 | **通过** | 最终三跑 panic=0、告警=0（`22` 脚本「内核告警数: 0」）；且告警检查机制本身已修为「测试体之后重新取样」，NC-C 证明能对测试体内 WARNING 报 FAIL（整改前同场景假绿） |
| 5 | 三份文档已落盘且文档结论可被日志复现 | **通过（附保留意见）** | `docs/impl/02-i2c-driver-实现记录.md`、`docs/kb/02-i2c-driver-知识点.md`、`docs/verify/02-i2c-driver-验证报告.md` 均在；本轮额外核对三份文档 + 整改记录引用的 **31 个日志文件名全部存在**。保留意见：kb/impl 成文于整改前，部分结论已陈旧（见 N1），建议同步更新，否则「按当前代码复跑」会与文档数值不符 |

### 最终结论

**阶段 02 复验结论：通过。**

- 原报告「不通过」的两个原因均已消除：`docs/kb/02-*` 已落盘；02 测试的告警取样窗口假绿已修复并经负控实证。
- 整改记录声称修好的 F1~F8 与两个额外项，经**逐条代码核对 + 三项独立重做负控（外加一项 F8 专项负控）**全部确认成立；
  无一项是「文档说修了、实际没修」。
- 未发现整改引入的新功能性缺陷；所有负控改动已逐字节还原，工作区与复验前完全一致（`git status` 无新增变化、无暂存文件、无负控残留）。
- 唯一遗留是**文档陈旧**（N1）与几条注释/仓库卫生提示（N2~N5），均不阻塞验收，建议整改者顺手同步。

---

## 6. 残留风险

1. **文档漂移（N1）**：`docs/kb`/`docs/impl` 与当前代码不一致（SMBUS_BYTE、CONFIG=0x0000、旧日志措辞）。
   不影响功能与测试，但会影响「面试/复盘时按文档复现」的可信度；阶段 03 开工前建议先同步。
2. **数值断言与模拟区间耦合（N4）**：阶段 03/05 改动 `virt_i2c_convert()` 的区间或增加寄存器时必须同步改 3 条断言。
3. **F8 的失败路径只由「配置写失败」这一种注入验证**：regmap 初始化失败、chrdev 注册失败、IRQ 注册失败三条分支
   未做端到端注入（NC-D 覆盖的是「配置写失败 → 统一出口」这条主干）；`err_clear_clientdata` 本身是单一出口，静态核对无遗漏。
4. **SMP 并发未验证**：QEMU 仍为单核（`nr_cpu_ids=1`），`virt_i2c` 的互斥/注入计数与 `sensor_char` 的计数在 SMP 下未实测（阶段 03 的多进程并发会覆盖）。
5. **真实 I2C 控制器语义保真度**：`smbus_xfer` 仍只实现 BYTE_DATA/WORD_DATA（现已**不再**声明未实现的能力位，over-claim 消除），
   QUICK/BLOCK/PROCESS_CALL 未实现；`/dev/sensor0` 被占用时 `rmmod` 的行为未测——均非本阶段契约。

---

## 7. 复验纪律自证

- 未修改项目源码/测试脚本的**最终状态**；负控期间的临时改动全部还原，`shasum` 与复验前逐文件一致（`driver/*.c`、`tests/phases/*.sh`）。
- `git status --short` 与复验开始时逐条一致：`M docs/11-阶段任务书.md`、`M driver/sensor_char.c`、`M driver/virt_i2c.c`、
  `M scripts/13-vm-fast-cycle.sh`、`M tests/phases/02-i2c-driver.sh`；`?? docs/impl/02-*-实现记录.md`、`?? docs/impl/02-*-整改记录.md`、
  `?? docs/kb/02-*-知识点.md`、`?? docs/verify/02-*-验证报告.md`。`git diff --cached` 为空。
- 负控痕迹扫描：`grep -rn "NEGATIVE-CONTROL" driver/ tests/` = 0。
- 本轮唯一新增文件是本报告 `docs/verify/02-i2c-driver-复验报告.md`；唯一新增产物是 `logs/` 下的日志。
