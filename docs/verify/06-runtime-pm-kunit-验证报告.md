# 阶段 06 验证报告：Runtime PM + KUnit（独立复核）

> 复核对象：commit `d3e9445`（阶段06：Runtime PM + KUnit 内核单元测试，31 项检查，六套件合计 163 项全绿）
> 判定者：独立验证 agent（LANE=06，构建目录 `~/lab-06`）
> 主机：macOS（Apple Silicon），bash 3.2，QEMU TCG/cortex-a72，内核 6.6.156
> 复核日期：2026-09-14

---

## 0. 结论

**通过（DoD 五条全部满足）**。契约要求的 dev_pm_ops 挂接、autosuspend 参数、open/release 的 get/put 配平、
suspend 真停采样、KUnit 边界覆盖均已实测确认；六套测试独立复现 163/163 全绿；三个负控实验中两个命中预期、
一个（NC-B1）暴露了检查盲点。发现 1 个中等严重度代码竞态、2 个低严重度测试盲点，均不阻塞本阶段验收，
但建议在真机移植与后续阶段处理。

**负控结论一句话**：现有 31 项检查对"引用计数不平衡"（NC-A，7 项 FAIL）和"只改标志的假省电"（NC-B2，2 项 FAIL）
有判别力；**对"hrtimer 仍触发但中断被 mask"（NC-B1）没有判别力（31/31 仍 PASS）**，这是检查的真实盲区。

---

## 1. 交付版关键文件 sha256（供合并前逐一比对）

复核时（2026-09-14 14:06）`git status --short` 显示交付工作树**无被跟踪文件修改**，四个关键文件与 `HEAD` 一致：

| 文件 | sha256（HEAD = wt-06 工作树 = 隔离基线，三者相同） |
|---|---|
| `driver/sensor_char.c` | `8c78fd37454d8e41ee3e8dca658ec85257a9e7880efed305d99560be0bfb5d1e` |
| `driver/sensor_calc.h` | `3182b5a94538510ff2710ab282070eb2a9841129f36e298208e96a39ccb21f0a` |
| `driver/sensor_kunit.c` | `43a0e3186babaa7f59cfe732e2c3be43b2876b334f9e4fba287edfc7abd34ff1` |
| `tests/phases/06-runtime-pm-kunit.sh` | `3c1b676d94811716c31930fa34be83cf9117752fcf12828bd09983c53410e78c` |

**说明**：
* **我（验证 agent）从未编辑过 wt-06 下的任何被跟踪源文件**（`driver/`、`tests/`、`user/` 全部只读）。
  我对 wt-06 的唯一写入是本报告 `docs/verify/06-runtime-pm-kunit-验证报告.md`，以及把负控日志拷进
  `logs/`（`logs/*.log` 已被 `.gitignore` 忽略，不影响交付树）。
* 复核过程中观察到：13:55 我完成基线运行后，`driver/sensor_char.c` 于 **13:56:11 / 13:57:22** 被**另一个并发 agent**
  改写成 `/* NC-A: ... */`、随后又改成 `/* NC-C/NC-D: ... */`（注释掉 release 的 put / 去掉周期上界 / 改 ring_capacity）。
  这些改动**不是我所为**（我的 edit 调用因文本不匹配直接报错返回）。父 agent 已声明由其 `git checkout` 恢复；
  至 14:06 工作树已干净。为保证负控实验与交付版互不干扰，**我的三个负控实验全部在与交付树隔离的副本里完成**
  （详见第 4 节）。

---

## 2. 独立复现：六套测试（同一份产物，顺序执行）

构建命令（严格按本 lane 约定）：
```bash
limactl shell dev bash -c 'LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/13-vm-fast-cycle.sh'
LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/21-macos-sync-artifacts.sh
LC_ALL=C LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh <阶段> 300
```
> 说明：`22-macos-run-test.sh` 在 macOS 自带 bash 3.2 + `LANG=zh_CN.UTF-8` 下会因 `$f（`（多字节紧跟变量名）
> 被误解析而报 `f�: unbound variable`。加 `LC_ALL=C` 即可正常运行；这不影响脚本逻辑与测试结论。

| 测试 | 复现结果 | PASS | FAIL | panic | WARNING/Call trace | 日志文件 |
|---|---|---|---|---|---|---|
| 06-runtime-pm-kunit | ✅ 全绿 | 31 | 0 | 0 | 0 | `logs/20260914-135507-06-06-runtime-pm-kunit.log` |
| 04-sysfs-debugfs | ✅ 全绿 | 42 | 0 | 0 | 0 | `logs/20260914-135532-06-04-sysfs-debugfs.log` |
| 03-ringbuffer-mmap | ✅ 全绿 | 40 | 0 | 0 | 0 | `logs/20260914-135538-06-03-ringbuffer-mmap.log` |
| 02-i2c-driver | ✅ 全绿 | 19 | 0 | 0 | 0 | `logs/20260914-135550-06-02-i2c-driver.log` |
| 01-io-models | ✅ 全绿 | 16 | 0 | 0 | 0 | `logs/20260914-135555-06-01-io-models.log` |
| smoke | ✅ 全绿 | 15 | 0 | 0 | 0 | `logs/20260914-135607-06-smoke.log` |

合计 **163 项 PASS / 0 FAIL**，与实现记录一致。`13-vm-fast-cycle.sh` 编译输出只有 `CC [M]/LD [M]`，
无 `error`/`warning`。构建耗时 1.8s（增量）。

---

## 3. 31 项检查逐条（名称 / 预期 / 实测 / 证据行号）

证据文件：`logs/20260914-135507-06-06-runtime-pm-kunit.log`；“行号”为该日志中 `[CHECK:*]` 行。
脚本源码行号指 `tests/phases/06-runtime-pm-kunit.sh`。

| # | 检查项名称 | 预期 | 实测 | 日志行 | 脚本行 |
|---|---|---|---|---|---|
| 1 | 找到 i2c 从设备节点 | `*-0048` 存在 | `/sys/bus/i2c/devices/0-0048` | 274 | 44 |
| 2 | power/runtime_status 存在 | 文件存在 | 存在 | 275 | 45 |
| 3 | power/runtime_suspended_time 存在 | 文件存在 | 存在 | 276 | 46 |
| 4 | runtime PM 已启用 | status ∈ {active,suspended} | `active` | 279 | 55-58 |
| 5 | 无使用者时自动挂起 | 空闲 3s 后 `suspended` | `suspended` | 283 | 60 |
| 6 | 打开设备成功（fd 3 持有引用） | open 成功 | 成功 | 287 | 63 |
| 7 | 打开期间保持 active | `active` | `active` | 289 | 72 |
| 8 | 出现 runtime resume 日志 | dmesg 有 resume | 有（3.749s） | 290 | 74 |
| 9 | 关闭后自动挂起 | 关闭 3s 后 `suspended` | `suspended`（6.034s） | 295 | 82 |
| 10 | 出现 runtime suspend 日志 | dmesg 有 suspend | 有 | 296 | 84 |
| 11 | 挂起前确实产生过样本 | `seq > 0` | `seq=6` | 298 | 89 |
| 12 | 挂起期间样本序号冻结 | seq 不变 | `6 -> 6` | 300 | 94 |
| 13 | 挂起期间中断计数冻结 | irq 不变 | `6 -> 6` | 301 | 95 |
| 14 | 挂起期间写 interval_ms 不唤醒 | 仍 `suspended` | `suspended` | 304 | 101 |
| 15 | 驱动记录了被延迟的周期设置 | dmesg 有 deferred | 有（9.831s） | 305 | 104 |
| 16 | 重新打开设备成功 | open 成功 | 成功 | 309 | 108 |
| 17 | 重新打开后回到 active | `active` | `active` | 310 | 114 |
| 18 | 挂起期间设置的周期已生效 | `interval_ms=200` | `200` | 311 | 116 |
| 19 | 恢复后采样继续推进 | seq 增大 | `11 -> 16` | 313 | 121 |
| 20 | 反复开关后仍能自动挂起（引用计数配平） | 3 轮后 `suspended` | `suspended`（14.06s） | 324 | 136 |
| 21 | 累计挂起时间 > 0 | >0 | `8704ms` | 327 | 141 |
| 22 | KUnit 套件已执行 | 出现 `Subtest: sensor_calc` | 出现 | 343 | 148 |
| 23 | 零点用例通过 | `ok … sensor_raw_to_milli_boundaries` | ok 1 | 344 | 150 |
| 24 | 芯片工作区间用例通过 | `ok … _chip_range` | ok 2 | 345 | 151 |
| 25 | 高 4 位状态位用例通过 | `ok … _ignores_high_bits` | ok 3 | 346 | 152 |
| 26 | 周期边界用例通过 | `ok … sensor_interval_valid_bounds` | ok 4 | 347 | 153 |
| 27 | 环缓冲回绕用例通过 | `ok … sensor_fifo_next_wraps` | ok 5 | 348 | 154 |
| 28 | 套件汇总为 5 通过 0 失败 | `pass:5 fail:0` | `pass:5 fail:0 skip:0 total:5` | 349 | 155 |
| 29 | KUnit 没有失败用例（not ok 计数） | `0` | `0` | 350 | 157 |
| 30 | sensor_kunit 模块已加载 | lsmod 可见 | 可见 | 351 | 159 |
| 31 | dmesg 无 WARNING/Call trace | `0` | `0` | 353 | 164 |

### 3.1 恒真 / 弱检查审查

* **没有结构性恒真项**（不存在 `check_true x 1 1` 这类写死判定）；每一项在依赖文件缺失或状态不符时都会 FAIL。
  第 4 项从契约的 `control == auto` 改为 `runtime_status != unsupported`，是**加强**（`control` 默认就是 `auto`，
  原判据近乎恒真），实现记录的问题 1 对此有说明，复核认可。
* **5 项"用例名通过"（#23-27）单独看是弱检查**：`check_contains` 走 `grep` 正则，模式 `ok .* <用例名>` 会匹配到
  `not ok 1 <用例名>`（因为 `not ok` 含子串 `ok`）。见第 4 节 NC-C 实证：`sensor_raw_to_milli_boundaries` 实际
  FAIL，而 #23 仍判 PASS。真正兜住的是 #28（`pass:5 fail:0`）与 #29（`not ok` 计数）。
  **判定语义正确，但逐条用例名检查有假绿风险**，建议改为锚定行首：`grep -E '^\s*ok [0-9]+ <name>$'`。
* **KUnit 判定是否会被"只有 ok 没有 not ok"蒙混**：不会。三处独立证据——
  ① `Subtest: sensor_calc` 必须出现；② 5 条汇总 `pass:5 fail:0` 必须出现（skipped 会变成 `skip:1`，也 FAIL）；
  ③ `grep -c "not ok"` 必须为 0。若 KUnit 完全没跑（默认关闭/模块没加载），①②③中至少①②失败。

### 3.2 时序窗口是否过紧

`SENSOR_AUTOSUSPEND_DELAY_MS = 1000`（`sensor_char.c:1232`），脚本所有挂起等待均 `sleep 3`（3 倍余量）；
open 后的 active 判定 `sleep 1`（resume 在 `pm_runtime_resume_and_get()` 里是同步的，且周期 200~500ms，
1s 内必有新样本）。实测挂起发生在 closed 后 1.00~1.26s，与 autosuspend 延迟吻合，窗口不紧。

---

## 4. 负控实验（3 个，全部在隔离副本中执行）

**隔离方式**（应父 agent 指示）：把交付树复制到 `/Users/lucien/verify-06nc/`（含 `driver/user/tests/scripts/dts/docs`），
用 `LANE=06nc` 构建到 `~/lab-06nc`，用副本自带的 `scripts/22-macos-run-test.sh` 运行、日志写副本的 `logs/`。
**实验全程不触碰 `/Users/lucien/workspace/self-study/projects/wt-06` 的源码**。
隔离副本的三份被改文件在实验前/实验后都来自 `git show HEAD:`，**sha256 与交付版逐字节相同**（见第 1 节表格）。
每次实验后都把 `sensor_char.c` / `sensor_calc.h` 从 `.baseline/` 还原并重跑确认全绿（见实验 4）。

| 实验 | 改动 | 预期 | 实测 | 抓住它的检查项（日志:行号） | 日志文件 |
|---|---|---|---|---|---|
| **NC-A** | 删掉 `release()` 里的 `pm_runtime_put_autosuspend`（`sensor_char.c:550`）→ 引用计数永不归零 | 「关闭后自动挂起」FAIL | ✅ **7 项 FAIL**（关闭后自动挂起=`active`、出现 suspend 日志、样本冻结 `10→14`、中断冻结 `10→14`、挂起期间写周期不唤醒=`active`、deferred 日志、反复开关后仍能挂起=`active`） | `logs/20260914-135739-…:294,295,299,300,303,304,321` | `20260914-135739-06nc-06-runtime-pm-kunit.log` |
| **NC-B1** | 只删 `suspend()` 里的 `hrtimer_cancel`，保留 `disable_irq`（`sensor_char.c:1269`） | 「挂起期间采样冻结」FAIL | ❌ **未 FAIL，31/31 全绿**（盲点） | 无（被 `disable_irq` 掩盖） | `20260914-135811-06nc-06-runtime-pm-kunit.log` |
| **NC-B2** | 删掉 `suspend()` 的 `hrtimer_cancel` **和** `disable_irq`，只 `WRITE_ONCE(suspended,true)` —— 典型"假省电" | 「挂起期间采样冻结」FAIL | ✅ **2 项 FAIL**（样本冻结 `14→18`、中断冻结 `14→18`） | `logs/…135840…:343,344` | `20260914-135840-06nc-06-runtime-pm-kunit.log` |
| **NC-C** | 在 `sensor_calc.h:54` 去掉 12 位符号扩展（`0x0800` 不再为负） | KUnit FAIL，且能看出哪个用例挂 | ✅ **2 项 FAIL**；TAP 精确定位 `not ok 1 sensor_raw_to_milli_boundaries`（`sensor_kunit.c:43-44`：`0x0800→128000` 期望 `-128000`；`0x0FFF→255937` 期望 `-62`），套件汇总 `pass:4 fail:1` | `logs/…135909…:355,356` | `20260914-135909-06nc-06-runtime-pm-kunit.log` |
| **恢复确认** | 还原基线后重跑 | 全绿 | ✅ **31 PASS / 0 FAIL** | — | `20260914-140535-06nc-06-runtime-pm-kunit.log` |

上述 5 个负控日志已复制到 `logs/` 供复查（`logs/2026*-06nc-*.log`，该目录被 gitignore）。

### 4.1 NC-B1 揭示的真实盲点（重要）

`disable_irq()` 会让 `handle_simple_irq` 因 `IRQD_IRQ_DISABLED` 直接丢弃中断，因此**只要中断被 mask，
即使 hrtimer 仍在按周期触发，`seq`/`irq` 计数也会冻结**，「挂起期间采样冻结」判 PASS。
后果：如果实现只关中断、忘了 `hrtimer_cancel`，设备表面上 `suspended`、计数冻结、31 项全绿，
但**每秒仍在唤醒 CPU 一次**——正是 Runtime PM 最典型的"假省电"，而当前 31 项检查无法发现。
`hrtimer_cancel` 与 `disable_irq` 是两道冗余防线，检查只能证明"输出被冻住"，不能区分是哪一道在起作用。
**建议**：后续阶段可增加"挂起期间 `/proc/timer_list` 里本设备的 hrtimer 不再 armed"或统计 `timer_expired` 不再增长的检查。

### 4.2 NC-B2 顺带暴露的告警检查盲点

NC-B2 因 `resume()` 里 `enable_irq()` 没有配对的 `disable_irq()`，内核打印
`Unbalanced enable for IRQ 21` + `WARNING: ... __enable_irq` + `Call trace`（日志 286/287/305、351/352/370 行），
**但脚本第 31 项「dmesg 无 WARNING/Call trace」仍判 PASS**——因为第 10 步的 `dmesg -c` 把之前的告警清掉了，
该检查只覆盖"KUnit 段之后"的 dmesg。外部 `22-macos-run-test.sh` 的整日志 grep 打印了 `内核告警: 4`，
**但它不会因告警而判失败**（只在 FAIL_N>0 / 未跑到 END / panic 时失败）。
交付版绿跑该值为 0，故不影响本次结论；但这是一处"看起来比实际强"的检查，记录为低严重度隐患。

---

## 5. 代码审查（按严重度）

### 5.1 引用计数配平（结论：配平）

| 路径 | get | put | 结论 |
|---|---|---|---|
| `open()` / `release()` | `pm_runtime_resume_and_get` `sensor_char.c:513` | `mark_last_busy`+`put_autosuspend` `:549-550` | 1:1 配对；`resume_and_get` 失败时自动退还引用，不会留下悬挂引用 |
| `probe()` | `get_noresume` `:1482` | `put_autosuspend` `:1488` | 配对；`set_active` 在 `enable` 之前，避免多跑一次 resume |
| debugfs `regs` | `pm_runtime_resume_and_get` `:1148` | `put_autosuspend` `:1165` | 配对；只有 get 失败时提前 return（此时无需 put） |

无"漏 put 永不挂起"、无"多 put 计数下溢"的静态路径。NC-A 实证：漏 put 会被 #9/#20 抓住。

**debugfs `regs` 为什么需要 PM 引用**：该接口经 regmap→i2c 读寄存器（`sensor_char.c:1166-1177`），
真机上设备挂起时总线接口可能断电，"看现场"会读到假值甚至失败；先 resume 再读是正确做法
（代价：读 regs 会把挂起中的设备临时唤醒约 1s，属 Runtime PM 固有语义，实现记录已声明）。

### 5.2 隐患

**【中】`sensor_apply_interval()` 与 suspend/resume 之间缺少同步（`sensor_char.c:808-815` vs `:1252-1296`）**
`sysfs interval_ms` 写入不持有 PM 引用，`sensor_apply_interval()` 先读 `READ_ONCE(sd->suspended)`
（`:808`）再 `hrtimer_start()`（`:814`），两步之间没有锁、也没有 PM 引用保护。若 autosuspend 恰好在这个
窗口内完成 suspend（`hrtimer_cancel`+`disable_irq`+`suspended=true`），apply 仍会 `hrtimer_start()`，
结果是：`runtime_status=suspended`、中断已 mask、`seq/irq` 冻结（测试全绿），**但 hrtimer 仍在按周期触发**——
即 4.1 描述的假省电，且 31 项检查无法发现。
触发概率低（窗口仅数条指令，需 sysfs 写入与 autosuspend 工作项抢跑），但真机上属于电源管理缺陷。
**建议**：`sensor_apply_interval()` 在 `hrtimer_start()` 后重新检查 `suspended`（若已挂起则再 `hrtimer_cancel`），
或让 sysfs 写入路径 `pm_runtime_get_sync()` 后再改（与 open 一致）。
对称地，`resume()` 的 `enable_irq→WRITE_ONCE(suspended,false)→hrtimer_start`（`:1292-1294`）窗口内并发 apply
会让定时器在一小段时间内仍按旧周期跑，随后被下一次 apply 纠正，影响可忽略。

**【低】第 31 项告警检查范围偏窄**（同 4.2）：`dmesg -c`（脚本 :145）清掉了 step 1-9 的告警，
`:164` 只在 KUnit 段之后取样。建议把该检查改成用一份"从测试开始就持续累积"的 dmesg 快照。

**【低】5 条用例名检查可被 `not ok` 蒙混**（同 3.1）：正则未锚定行首。建议锚定 `^\s*ok [0-9]+ <name>$`。

**【信息】`remove()` 顺序**：`pm_runtime_disable()`（`:1537`）在 `hrtimer_cancel`（`:1539`）/`free_irq`（`:1540`）之前，
偏离契约原文但父 agent 已裁定接受。理由（disable 同步等待在途 PM 回调，避免 resume 把 hrtimer 重启）成立，
复核认可。注意：若 remove 时设备处于 runtime-suspended，则 irq 处于 `disable_irq` 状态就被 `free_irq`；
本项目 QEMU 实测（阶段 03 的 `rmmod/insmod` 两轮）无 `Unbalanced` 告警，但"带 disable_irq 计数的 free_irq"
在真机上属需要确认的边角，列为遗留疑点。

**【信息】`sensor_runtime_suspend` 打印 `sd->irq_count`/`sd->latest.seq` 未加 `READ_ONCE`**（`:1273-1274`）：
仅用于日志，且 suspend 前已 `hrtimer_cancel`+`disable_irq`，无实际影响。

### 5.3 `sensor_calc.h` 抽取后行为等价性

| 函数 | 与 `HEAD~1` 原实现对比 | 结论 |
|---|---|---|
| `sensor_interval_valid()` | `HEAD~1`（阶段04）已是 `1..60000` 的 `static bool`，两入口统一复用；抽到 `sensor_calc.h` 只是换位置 | **完全等价**（实现记录所说"下限 10ms"是阶段 04 之前的历史，非本次改动） |
| `sensor_fifo_next()` | 原为 `(write_idx + 1) % SENSOR_SHM_SAMPLES` 内联；新函数多一个 `cap==0` 防御分支，生产路径 `cap=64` 恒非 0 | **等价**，且新增了边界可测性 |
| `sensor_raw_to_milli()` | 原宏 `(s32)raw * 1000 / 16`（按 16 位补码解释）；新函数按 **低 12 位补码** 解释 | **语义有变**：`bit11=1` 时结果由正变负（`0x0800`：+128000 → -128000）。模拟芯片工作区间 24.0~26.0℃ → raw 0x0180~0x01A0（`bit11=0`），**结果完全一致**；且 `virt_i2c.c:34-35` 明确采用"低 12 位有效"约定，新实现才与该约定自洽。属**修正**，实现文档第 3.4 节已诚实记录 |

### 5.4 挂起中改周期"只记账"与 sysfs 回读一致性

`interval_ms_store` → `sensor_apply_interval()` 在**读 `suspended` 之前**就已把新值写入 `sd->interval_ms`
（`:804`，持 `sd->lock`），因此 sysfs `interval_ms_show`（`:982-989`）**立即回读到新值**（实测 `200`，检查 #18 PASS），
而定时器不启动；`runtime_resume()` 用 `READ_ONCE(sd->interval_ms)` 重启（`:1291-1294`，实测日志
`runtime resume: sampling restarted (interval=200ms)`）。语义一致，无"回读新值但实际用旧值"的矛盾。

---

## 6. 契约核对

| 契约项（`docs/11-阶段任务书.md` 阶段06） | 实现 | 结论 |
|---|---|---|
| `dev_pm_ops` 用 `SET_RUNTIME_PM_OPS(suspend, resume, NULL)` | `sensor_char.c:1300-1302`，挂到 `i2c_driver.driver.pm`（`:1589`） | ✅ |
| probe: `pm_runtime_enable` + `set_autosuspend_delay(1000)` + `use_autosuspend` | `:1484-1486`（另有 `get_noresume`/`set_active`/`mark_last_busy`/`put_autosuspend`） | ✅ |
| open 取引用 / release 归还 | `resume_and_get` `:513`；`mark_last_busy`+`put_autosuspend` `:549-550` | ✅ |
| remove 里 `pm_runtime_disable` | `:1537`（顺序前置，父 agent 已裁定） | ✅（偏差已批准） |
| suspend 停 hrtimer + 关中断，resume 重启，均有日志 | `:1269-1275` / `:1292-1296`，日志实测存在 | ✅ |
| KUnit 覆盖 0x0000/0x07FF/0x0800/0x0FFF、非法周期、下标回绕 | `sensor_kunit.c` 5 用例（边界/工作区间/高4位/周期边界/回绕） | ✅ |
| 用例名有意义 | `sensor_raw_to_milli_boundaries` 等 | ✅ |
| 测试脚本至少含：PM启用/打开活跃/关闭挂起/挂起恢复日志/挂起停采样/KUnit全过/无告警 | 31 项全覆盖，另加冻结计数、deferred、引用配平、suspended_time | ✅（超集） |
| 「挂起期间停采样」判定 `irq_count` 停止增长 | 实测 irq `6→6` | ✅ |

**契约文字与现实的偏差（均已裁定/记录，非缺陷）**：
1. PM sysfs 在 i2c 硬件设备 `0-0048` 上，不在 `/sys/class/sensor_char/sensor0/power/`（契约末段已修正）。
2. 契约写 KUnit 输出 `ok 1 -`，内核 6.6 实际是 KTAP v1 `ok 1 <name>`（无 `-`）；脚本按现实匹配。
3. remove 顺序（disable 在前）；阶段 04 测试脚本显式持有 fd；均由父 agent 裁定接受。

---

## 7. 遗留疑点

1. **NC-B1 盲点**（中）："挂起期间计数冻结"不能证明 hrtimer 真的停了；当前 31 项无法发现"只关中断不 Cancel 定时器"。
2. **`sensor_apply_interval` 竞态**（中）：sysfs 写周期与 autosuspend 抢跑时可造成"已挂起但定时器仍在触发"。
3. **告警检查范围**（低）：第 31 项被 `dmesg -c` 截断，只覆盖测试后段；外部 runner 打印告警数但不据此失败。
4. **负温度链路无端到端覆盖**（低）：KUnit 覆盖了 12 位补码函数，但 `virt_i2c` 只产生正温度，
   `regmap→换算→ioctl/IIO` 的负温度路径没有集成测试。实现文档已声明该边界。
5. **`remove()` 时已挂起→带 disable 计数 free_irq**（信息）：QEMU 实测无告警，真机需确认。
6. **并发写者**：复核期间 wt-06 被兄弟 agent 临时改写又恢复，`docs/kb/06-…-知识点.md` 在 14:05 仍在被写入
   （当前 82KB、untracked）。交付前请确认该文件内容完整且已纳入最终提交。

---

## 8. DoD 五条逐条判定

| # | DoD 要求 | 依据 | 判定 |
|---|---|---|---|
| 1 | `make` 无 error / 无 warning（新增代码） | `13-vm-fast-cycle.sh` 输出仅 `CC [M]/LD [M]`，无 error/warning | **通过** |
| 2 | `22 <本阶段>` 全 PASS 且跑到 `[TEST:END]` | 31 PASS / 0 FAIL，`[TEST:END] 06-runtime-pm-kunit pass=31 fail=0`（日志 356 行） | **通过** |
| 3 | `smoke` 仍全 PASS | smoke 15 PASS / 0 FAIL；另 04/03/02/01 全绿（163/163） | **通过** |
| 4 | panic 0；WARNING/Call trace 需解释 | 六套 `内核 panic: 0`、`内核告警: 0`（含最终 06 绿跑） | **通过** |
| 5 | 三份文档落盘且结论可由日志复现 | `docs/impl/06-…-实现记录.md`、`docs/kb/06-…-知识点.md`（14:05 仍在写，untracked）、本报告；本文所有结论均给出日志文件名与行号 | **通过**（提醒 kb 文档状态） |

**总判定：阶段 06 通过独立复核。**

---

## 附：本次复核产生的文件

* 本报告：`docs/verify/06-runtime-pm-kunit-验证报告.md`（唯一被跟踪的新增/修改文件）
* 负控日志：`logs/2026*-06nc-*.log`（5 份，gitignore）
* 独立复现日志：`logs/20260914-1355xx-06-*.log`、`logs/20260914-135607-06-smoke.log`（gitignore）
* 隔离实验目录（不在仓库内）：`/Users/lucien/verify-06nc/`、VM 内 `~/lab-06nc`
