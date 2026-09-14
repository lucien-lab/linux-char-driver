# 阶段 06 实现记录：Runtime PM + KUnit 内核单元测试

> 目标：补上嵌入式高频考点「Linux 电源管理」，并用内核单元测试覆盖纯逻辑函数。
> 契约见 `docs/11-阶段任务书.md`「阶段 06」节。本文记录**实际实现、踩坑过程与验证证据**。

## 一、改动清单

| 文件 | 改动 | 说明 |
|---|---|---|
| `driver/sensor_calc.h` | **新增**（约 110 行） | 纯逻辑函数：`sensor_raw_to_milli()` / `sensor_interval_valid()` / `sensor_fifo_next()` |
| `driver/sensor_kunit.c` | **新增**（约 120 行） | KUnit 套件 `sensor_calc`，5 个用例 |
| `driver/sensor_char.c` | 修改 | 引入 Runtime PM（`dev_pm_ops` + open/release/probe/remove 的引用计数）、换算与周期判定改为调用 `sensor_calc.h`、共享区下标回绕复用 `sensor_fifo_next()`、debugfs `regs` 读加 PM 引用 |
| `driver/Makefile` | 修改 | 增加 `obj-m += sensor_kunit.o` |
| `tests/phases/06-runtime-pm-kunit.sh` | **新增** | 31 项检查 |
| `tests/phases/04-sysfs-debugfs.sh` | 修改 | 故障注入段显式持有设备引用（阶段 06 的行为变化导致的必要前提补充，见第五节） |
| `tests/phases/03-ringbuffer-mmap.sh` | 修改 | 放大器窗口占空比 10% → 50%（消除并发负载下的假失败，见第五节） |

---

## 二、Runtime PM：状态机与 get/put 时机

### 2.1 为什么需要它

驱动在 probe 里启动了一个周期性 hrtimer，它会不断触发中断 → 线程化下半部 → I2C 事务。
**没有任何进程打开设备时这套动作也在跑**：在电池设备上就是纯浪费（真实传感器上还意味着
ADC 一直在转换、总线一直被占用）。Runtime PM 让设备在"没人用"时自动停下来。

### 2.2 引用计数的时间线（实测）

```
[    0.642255] runtime PM enabled (control=auto autosuspend=1000ms)   ← probe 交还引用
[    1.646894] runtime suspend: sampling stopped (irq_count=2 seq=2)  ← +1.005s，无人使用 → 自动挂起
[    3.697208] runtime resume: sampling restarted (interval=500ms)    ← open() 取引用 → 恢复
[    3.697578] opened (count=1)
[    4.735020] closed                                                 ← 归还引用，1s 后挂起
[    5.756469] runtime suspend: sampling stopped (irq_count=6 seq=6)
[    9.835857] interval -> 200 ms (deferred: device runtime-suspended) ← 挂起期间改参数只记账
[   10.892665] runtime resume: sampling restarted (interval=200ms)    ← 重新 open：按新周期重启
[   12.955493] closed
[   14.098034] runtime suspend: sampling stopped (irq_count=22 seq=22) ← 反复开关后仍能挂起
```

对照的检查项实测值（日志 `logs/20260914-135029-main-06-runtime-pm-kunit.log`）：

| 时刻 | runtime_status | 触发动作 |
|---|---|---|
| 空闲 3 秒 | `suspended` | probe 交还引用 + autosuspend 到期 |
| 持有 fd | `active` | `open()` → `pm_runtime_resume_and_get()` |
| 关闭 3 秒后 | `suspended` | `release()` → `mark_last_busy()` + `put_autosuspend()` |
| 挂起期间 | `suspended`（写 `interval_ms` 不会唤醒） | `sensor_apply_interval()` 检测到挂起 → 只记账 |
| 开关 3 轮后 | `suspended` | 引用计数已配平 |
| 累计挂起时间 | `runtime_suspended_time = 9076ms` | 核心记录了真实的低功耗时长 |

### 2.3 引用计数为什么必须配平

`probe` 结束时用一对 `pm_runtime_get_noresume()` + `pm_runtime_put_autosuspend()`：

* `get_noresume()` 声明"probe 期间设备是活的"，但**不**触发一次 resume 回调；
* `set_active()` 把 PM 状态标成 active（此时定时器确实在跑）；
* `put_autosuspend()` 交还引用 → 计数降到 0 → 1 秒后核心回调 `runtime_suspend`。

如果只 `get` 不 `put`（或干脆什么都不做），引用计数永远不为 0，**设备永远不会挂起，
而表面上一切正常**——没有报错、没有告警，属于最难发现的一类错误。
测试项"反复开关后仍能自动挂起"就是专门抓这一类不平衡。

### 2.4 suspend 里为什么必须真的停采样

"假省电"是电源管理最典型的假通过：只把状态标成 suspended，硬件/采样照跑。
本驱动的做法是**停数据源**：

```c
hrtimer_cancel(&sd->timer);   /* 同步等待正在执行的定时器回调结束 */
disable_irq(sd->virq);        /* 关中断线：真机上传感器可能自己报 DRDY，光停定时器不够 */
WRITE_ONCE(sd->suspended, true);
```

顺序是刻意的：先停定时器（它可能正在触发中断），再关中断线；反过来会残留一个已排队的中断。
对应检查项：挂起 2 秒内 `seq` 与 `irq` 计数必须**完全冻结**（实测 `seq 6 -> 6`、`irq 6 -> 6`）。

### 2.5 remove 里的顺序（对契约做了一处有意偏离）

契约写的是"先停 hrtimer/中断，再 `pm_runtime_disable()`"。实现里把
`pm_runtime_disable()` 放在了**最前面**，理由：

* `pm_runtime_disable()` 会同步等待正在执行的 suspend/resume 回调结束，并让此后的 PM 请求失败退出；
* 若放在最后，`hrtimer_cancel()` 与 `pm_runtime_disable()` 之间存在一个窗口——
  内核仍可能回调 `sensor_runtime_resume()`，而 resume 里会 `hrtimer_start()`，
  把采样在"正在拆卸设备"的过程中原地重新启动。

两种顺序都能保证"拆卸期间不采样"，但 disable 在前是**结构性**地排除竞态，
而不是靠时序侥幸。此偏离已在本节记录，供 review 与后续维护参考。

### 2.6 顺带修正的一处真实缺陷：调试接口也要遵守电源管理

`debugfs/regs` 会发起 I2C 事务读寄存器。真机上设备挂起时总线接口可能是关的
（读不到或读到无意义值），因此该接口现在先 `pm_runtime_resume_and_get()`、读完再归还。
**否则"看现场"这个动作本身就会破坏现场**（读到假数据）。

---

## 三、KUnit：覆盖边界，以及"能测什么、不能测什么"

### 3.1 为什么把纯逻辑抽成 `static inline` 头文件

驱动里有三类代码：① 内核交互（regmap/中断/文件操作）；② 纯计算（原始值换算、参数合法性、下标推进）；
③ 策略（何时挂起）。第 ② 类最适合单元测试——没有副作用、边界清晰，且恰恰是**最容易写错、
错了又最难在真机上发现**的地方（负温度补码、下标回绕）。

拆成 `static inline` 而不是单独一个 `.c`：

* 对驱动零额外开销（编译器内联，不引入跨编译单元调用）；
* 不必为一个函数多编一个模块，也就不需要维护 `EXPORT_SYMBOL` 与模块依赖；
* **驱动与 `sensor_kunit.ko` include 同一份实现** —— 测试测的就是驱动实际执行的代码，
  不存在"测试一份、跑另一份"的漂移（这是单元测试有意义的前提）。

约束（写在文件头注释里）：只能放**不依赖内核子系统、无副作用**的函数。
一旦某个函数需要睡眠、需要锁或需要访问寄存器，它就不属于这个头文件。

### 3.2 KUnit 的覆盖边界（诚实说明）

| 能覆盖 | 不能覆盖 |
|---|---|
| 12 位补码的正/负边界（`0x0000/0x07FF/0x0800/0x0FFF`） | regmap/I2C 传输是否正确（需要真实总线） |
| 换算系数（24.0 ℃→24000 m℃、26.0 ℃→26000 m℃） | 中断上下文约束、能否睡眠 |
| 高 4 位状态位不参与算术（`0x8190` 与 `0x0190` 必须同值） | Runtime PM 时序（挂起/恢复的时序只能在 QEMU 集成测试里验） |
| 周期合法性的两个端点与越界、`ULONG_MAX` 不误判 | 并发正确性（KCSAN 才有意义） |
| 环形下标推进与回绕、容量为 0 的防御分支 | 真实硬件时序（传感器转换时间、总线毛刺） |

### 3.3 用例清单与实测输出

```
    # Subtest: sensor_calc
    # module: sensor_kunit
    1..5
    ok 1 sensor_raw_to_milli_boundaries
    ok 2 sensor_raw_to_milli_chip_range
    ok 3 sensor_raw_to_milli_ignores_high_bits
    ok 4 sensor_interval_valid_bounds
    ok 5 sensor_fifo_next_wraps
# sensor_calc: pass:5 fail:0 skip:0 total:5
ok 1 sensor_calc
```

判定方式（两条一起看，缺一不可）：

1. 用例名逐条出现（证明**这一版**用例真的跑了，而不是 dmesg 里恰好有 `ok` 字样）；
2. 套件汇总行 `sensor_calc: pass:5 fail:0` 与 `not ok` 计数为 0（防止"只看有 ok"被蒙混过关）。

### 3.4 换算函数的一个语义修正（顺带记录）

原实现用宏 `RAW_TO_MILLI(raw) = (s32)raw * 1000 / 16`（按 16 位补码解释），
现在按芯片约定实现为**低 12 位补码**（`sensor_raw_to_milli()`）。
对这颗芯片的实际输出（24.0~26.0 ℃ → raw 384~416）两者结果完全一致，
但 12 位版本对 `0x0FFF` 这类"高位状态位为 0、低 12 位为负"的值给出的才是正确结果
（`-62 m℃` 而不是 `+255937 m℃`），并且把"高 4 位不参与算术"这条约定显式测了出来。

---

## 四、踩到的问题与解决（按发现顺序）

### 问题 1：`power/runtime_status` 读出来是 `unsupported`

**现象**：第一版测试读 `/sys/class/sensor_char/sensor0/power/runtime_status`，得到 `unsupported`，
11 项检查失败，看起来像"Runtime PM 没启用"。

**根因**：Runtime PM 状态挂在**硬件设备**（i2c 从设备 `0-0048`）上，而
`/sys/class/sensor_char/sensor0` 是 `device_create()` 出来的**纯软件接口**节点，
它自己的 `power/` 目录永远是 `unsupported`（`disable_depth = 1`）。
驱动只对 i2c client 设备调了 `pm_runtime_enable()`。

**解决**：测试改为读 `/sys/bus/i2c/devices/*-0048/power/`；并在脚本头部把这个概念写成注释。
同时把"PM 已启用"的判定从 `control == auto` 升级为
**`runtime_status` 不是 `unsupported`**——因为 `control` 对所有设备默认就是 `auto`，
作为"驱动启用了 PM"的判据几乎是恒真的（弱检查）。

### 问题 2：KUnit 用例判定写成了 `ok 1 - <用例名>`

**现象**：KUnit 实际输出是 `ok 1 sensor_raw_to_milli_boundaries`（没有 `-`），
5 条用例检查全部假失败。

**根因**：内核 6.6 的 KUnit 使用 KTAP v1 格式（`ok <序号> <用例名>`），
而 `-` 分隔是 TAP 13 的写法。

**解决**：改用 `ok .* <用例名>` 模式匹配，并额外断言套件汇总行 `pass:5 fail:0`。

### 问题 3：复用旧的 dmesg 快照导致假失败

**现象**：`runtime resume 日志` 检查失败，但日志里明明有这条记录。

**根因**：该检查用的是步骤 4 采样的 dmesg 快照，而 resume 发生在步骤 7（更晚）。

**解决**：每条与"事件是否发生"相关的检查都**当场重新取样**。
这条教训与阶段 02 的"告警检查必须在测试体之后取样"是同一类问题。

### 问题 4（重要）：Runtime PM 改变了"后台采样"这个隐含前提

**现象**：阶段 04 的"注入结束后采样仍在推进"检查失败（`seq 57 → 57`）。

**根因**：阶段 06 之前，设备**永远在采样**，所以阶段 04 的故障注入步骤（不打开设备、
靠周期性 I2C 读消耗注入次数）可以直接工作。阶段 06 之后，无人持有时设备会挂起并停止采样，
该步骤测到的是"挂起后设备不采样"，与它想验证的错误处理路径无关。

**解决**：在阶段 04 的故障注入段显式打开设备持有引用（`exec 3<>/dev/sensor0`），
段末归还。**这不是放宽检查**，而是把一个隐含前提写出来。
受影响范围的核查结论：`smoke`（ftrace 步骤）、`01`、`03` 都不受影响
（分别在 close 后 1 秒内 / 全程持有 fd），实测七套测试均全绿。

### 问题 5：放大器检查在宿主负载高时假失败（阶段 03）

**现象**：`03-ringbuffer-mmap` 偶发 `shm_seq_odd_seen=0`（同版本 13:46 通过、13:48 失败），
而 `shm_seq_changes=200` 说明写者确实在发布。

**根因**：该检查依赖"读者与写者真的并行执行"。放大器窗口占空比是 1ms/10ms = 10%，
当宿主 CPU 被其它任务（并行 lane、内核构建）占满、读者线程被调度走时，
一次运行可能整段都撞不上窗口。

**解决**：把注入窗口提到 5ms（占空比 50%）并写进注释；
"假 seqlock"（内核不写 `seq`）在这种条件下依然是 0 次，判别力不受影响。
修正后连跑 3 次均 40/40。

---

## 五、验证证据

### 5.1 最终六套测试（同一份产物、顺序执行）

| 测试 | 结果 | 日志 |
|---|---|---|
| 06-runtime-pm-kunit | **31 PASS / 0 FAIL** | `logs/20260914-135029-main-06-runtime-pm-kunit.log` |
| 04-sysfs-debugfs | 42 PASS / 0 FAIL | `logs/20260914-135048-main-04-sysfs-debugfs.log` |
| 03-ringbuffer-mmap | 40 PASS / 0 FAIL | `logs/20260914-135054-main-03-ringbuffer-mmap.log` |
| 02-i2c-driver | 19 PASS / 0 FAIL | `logs/20260914-135106-main-02-i2c-driver.log` |
| 01-io-models | 16 PASS / 0 FAIL | `logs/20260914-135111-main-01-io-models.log` |
| smoke（基线回归） | 15 PASS / 0 FAIL | `logs/20260914-135122-main-smoke.log` |

合计 **163 项检查全绿**，无 panic、无 WARNING/Call trace。

### 5.2 稳定性

* `03-ringbuffer-mmap` 在占空比修正后**连跑 3 次**均 40/40；
* `06-runtime-pm-kunit` 在修正问题 1/2/3 后连跑 2 次均 31/31；
* 时序类检查（autosuspend 1s）统一留 3 秒余量，避免用紧窗口凑"刚好通过"。

### 5.3 复现方式

```bash
cd /Users/lucien/workspace/self-study/projects/linux-char-driver
limactl shell dev bash -c 'TEST=06-runtime-pm-kunit bash '"$PWD"'/scripts/13-vm-fast-cycle.sh'
bash scripts/21-macos-sync-artifacts.sh
bash scripts/22-macos-run-test.sh 06-runtime-pm-kunit 300
bash scripts/22-macos-run-test.sh smoke 180
```

---

## 六、遗留风险与后续建议

1. **`disable_irq()` 的假设**：本驱动的中断是自建虚拟中断域（实验环境）。
   真机上是传感器 DRDY 接 GIC 的普通 IRQ，用法相同；
   但若某天改成共享中断线（`IRQF_SHARED`），`disable_irq/enable_irq` 的语义需要重新评估。
2. **系统级 suspend/resume 未实现**：本阶段只做了 Runtime PM（`SET_RUNTIME_PM_OPS`）。
   若要支持系统睡眠，还需要 `SET_SYSTEM_SLEEP_PM_OPS` 与设备树 `wakeup-source`，
   属于可选扩展（当前不做，避免与 runtime PM 的交互引入未验证路径）。
3. **KUnit 未覆盖 `sensor_shm_publish()` 的序号协议**：该函数有副作用（写共享内存、`udelay`、自旋锁），
   不属于纯逻辑，因此留在 QEMU 集成测试（阶段 03 的放大器与 KCSAN）覆盖。
4. **建议验证 lane 关注**：
   * 删掉 `release()` 里的 `put_autosuspend` → "反复开关后仍能自动挂起"必须 FAIL；
   * 让 `suspend` 不停 `hrtimer` → "挂起期间采样冻结"必须 FAIL；
   * 在 `sensor_calc.h` 里把 `0x0800` 的符号处理写错 → KUnit 必须 FAIL；
   * `probe` 里去掉 `put_autosuspend` → "无使用者时自动挂起"必须 FAIL。
