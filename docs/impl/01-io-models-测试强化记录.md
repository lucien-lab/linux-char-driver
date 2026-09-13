# 阶段 01 测试强化记录（按独立验证反馈修复）

- 背景：`docs/verify/01-io-models-验证报告.md` 的独立验证者用**破坏性变体**证明
  上一版测试存在中危「假通过」缺口（P1），另发现两个低级问题（P2/P3）。
- 本文记录：把这三条反馈落地做了什么、**怎么证明补丁真的有效**、以及给后续阶段的建议。
- 完成时间：2026-09-14 03:00 ~ 03:06

---

## 0. 一句话结论

| 状态 | 阶段测试 `01-io-models` | 回归 `smoke` |
|---|---|---|
| 修复前（旧套件 + 坏驱动） | **9 PASS / 0 FAIL（假通过）** | — |
| 修复后（新套件 + 健康驱动） | **16 PASS / 0 FAIL** | **12 PASS / 0 FAIL** |
| 修复后（新套件 + 故意漏写 `poll_wait` 的坏驱动） | **11 PASS / 5 FAIL（成功抓住）** | — |

**「漏写 `poll_wait`」从"无法发现"变成"5 项检查同时失败"。**

---

## 1. 问题回顾（验证者的原始发现）

| # | 严重度 | 问题 | 上一版的缺陷本质 |
|---|---|---|---|
| P1 | 中 | 删掉 `sensor_poll()` 里的 `poll_wait()` 后，套件仍 **9 项全 PASS** | 判定是「epoll_wait 至少返回过一次可读」——而 `epoll_ctl(ADD)` 时的首轮 `->poll` 就满足了它。**上界检查（唤醒数 ≤ 12）抓不到"唤醒太少"** |
| P2 | 低 | `tests/runner/init.sh` 用 `cut -d' ' -f2-` 解析模块参数，行内无空格时把整行当参数返回 | 模块名被当参数传给 insmod → 每次启动 dmesg 出现 `unknown parameter 'sensor_char.ko' ignored` |
| P3 | 低 | 「阻塞 read」检查有时序依赖 | 新 fd 若恰好在样本已就绪时进入 read，会立即返回而不走等待路径，检查仍 PASS |

P1 的真正危害不是"驱动写错了"，而是**回归保护是假的**：阶段 02~06 都会改动
`read`/`poll` 的数据源（kfifo、IIO 缓冲），任何一次重构删掉 `poll_wait` 都会静默全绿。

---

## 2. 新增/修改的检查项（每项证明什么）

修复后 `01-io-models` 的检查项从 9 项增加到 **16 项**。下表只列新增/改动项。

| # | 检查项 | 判定 | 它专门证明什么 | 阈值怎么定的 |
|---|---|---|---|---|
| 1 | 阻塞 read 真的等了约一个采样周期 | `blocking_wait_ms ≥ 500` | **P3 修复**：证明 read 真的睡在 waitqueue 上，而不是"缓存里恰有样本所以立刻返回" | 测试前用 `ioctl(SET_INTERVAL, 1000)` 把周期拉长；先做一次 read 与"样本到达沿"对齐，再测第二次 read 的耗时 → 理论值≈1000ms。阈值取半个周期 500ms，实测 997 / 1000ms |
| 2 | 首次唤醒延迟 ≤ 1000ms | `epoll_first_wakeup_ms ≤ 1000` | **P1 核心修复**：这是发现「漏写 `poll_wait`」的**唯一可靠观测** | 正确实现注册了等待队列 → 新样本到达即唤醒 → 延迟 ≤ 一个周期(500ms)，实测 502ms；漏写时 `epoll_wait` 睡满超时(2500ms)才靠最后一次 `vfs_poll` "假装可读"，实测 2503ms。取 1000ms 有 2.5 倍分辨力 |
| 3 | 唤醒次数下界 | `epoll_wakeups ≥ 3` | **P1 核心修复**：同一次运行里的下界，直接掐死"只在 `epoll_ctl(ADD)` 时可读一次"的假通过 | 3 秒窗口 / 500ms 周期 ≈ 6~7 次，取 3 留足调度抖动余量。坏驱动实测 0 次 |
| 4 | 唤醒次数上界（**保留**） | `epoll_wakeups ≤ 12` | 抓"消费后仍上报 EPOLLIN"的忙轮询（实测坏实现可达 121642 次） | 原样保留——它对"唤醒过多"仍然有效，只是不能单独作为电平触发的证据 |
| 5 | 消费后 poll 立刻不再可读 | `poll_after_consume == 0` | 电平触发的**确定下界**（比"唤醒次数 ≤12"更直接：不依赖采样周期） | 抽干到 EAGAIN 后 `poll(fd,0)` 必须为 0；允许重试 3 次以排除"微秒级窗口内恰好来了新样本"的假失败 |
| 6 | 未消费时连续 5 次 poll 都可读 | `poll_unread_hits == 5` | 电平触发的**另一半边**：可读状态必须**持续存在**（one-shot/边沿式实现只能通过 1 次） | 上一版完全没有覆盖这半边语义 |
| 7 | 新开的 fd 立刻可读 | `new_fd_immediately_readable == 1` | per-open 独立性：`last_seq` 必须按 fd 记录，若实现成全局"已消费"标志，新 fd 会不可读 | 依赖"设备已产生过样本"（前置的阻塞测试已保证） |
| 8 | 该内核支持对 `kill_fasync` 做 ftrace 函数过滤 | `FT_OK == 1` | 消除"静默减少检查项"：上一版 ftrace 不可用时直接跳过该项，检查数从 9 悄悄变 8 仍显示全绿 | 当前内核支持（实测过滤到 10~14 次调用）；能力缺失要体现在结论里，而不是消失 |

> 前 7 项都改为「取不到值就用必然失败的默认值」（`val_or` 辅助函数）：
> 例如 `epoll_first_wakeup_ms` 缺失时取 99999、`poll_after_consume` 缺失时取 1（=失败）、
> `poll_unread_hits` 缺失时取 0。避免"删掉输出就能过"。

`user/io_models_test.c` 同步新增输出键：
`blocking_wait_ms`、`epoll_first_wakeup_ms`、`poll_after_consume`、`poll_unread_hits`、
`new_fd_immediately_readable`。另外 epoll 段新增**前置抽干**（`read` 到 `EAGAIN` 再 `epoll_ctl(ADD)`），
这是让下界检查生效的前提——否则 ADD 的首轮 `->poll` 又会造成假通过。

---

## 3. 负控实验（本次交付的核心证据）

方法：用**真实内核树**编出一个「故意漏写 `poll_wait()`」的坏驱动 `.ko`，替换进 initramfs
后跑同一套件；随后 `git checkout` 恢复。脚本、产物路径、日志全部保留。

```bash
# 注入缺陷（仅注释掉一行）
sed -i'' -e 's/^\tpoll_wait(file, &sf->sd->wq, wait);/\t\/* 负控实验（临时） *\/\n\t\/* poll_wait(file, \&sf->sd->wq, wait); *\//' driver/sensor_char.c
limactl shell dev bash -c 'TEST=01-io-models bash .../scripts/13-vm-fast-cycle.sh'
bash scripts/21-macos-sync-artifacts.sh
bash scripts/22-macos-run-test.sh 01-io-models 180     # → 11 PASS / 5 FAIL

# 恢复
git checkout -- driver/sensor_char.c
limactl shell dev bash -c 'TEST=01-io-models bash .../scripts/13-vm-fast-cycle.sh'
bash scripts/21-macos-sync-artifacts.sh && bash scripts/22-macos-run-test.sh 01-io-models 180   # → 16 PASS
bash scripts/22-macos-run-test.sh smoke 180            # → 12 PASS
```

### 3.1 对照结果

| 实验 | 驱动 | 日志文件 | 结论 |
|---|---|---|---|
| A 修复后·健康 | 正常 | `logs/20260914-030351-01-io-models.log`（16 PASS/0 FAIL）<br>`logs/20260914-030405-smoke.log`（12 PASS/0 FAIL） | 全部通过 |
| B **负控·坏驱动** | 漏写 `poll_wait()` | `logs/20260914-030424-01-io-models.log` | **11 PASS / 5 FAIL** |
| C 恢复后·健康 | 正常（`git checkout` 后与备份逐字节一致） | `logs/20260914-030446-01-io-models.log`（16 PASS/0 FAIL）<br>`logs/20260914-030455-smoke.log`（12 PASS/0 FAIL） | 全部通过 |

### 3.2 坏驱动到底被哪几条抓住（实验 B 证据）

| 失败检查项 | 日志行 | 原始实测值（日志行） |
|---|---|---|
| 首次唤醒延迟 ≤ 1000ms —— **实测 2503ms** | `:301` | `[IO] epoll_first_wakeup_ms=2503`（`:286`） |
| poll 唤醒次数下界 —— **实测 0 次** | `:302` | `[IO] epoll_wakeups=0`（`:287`） |
| epoll 在 3 秒窗口内报告可读 | `:299` | `[IO] epoll_wait_ok=0`（`:289`） |
| io_models_test 退出码为 0 —— 实际 1 | `:300` | `[IO] checks ... epoll=0 first_wakeup=0 wakeup_min=0`（`:294`） |
| 程序整体结论为 PASS | `:308` | `[IO] OVERALL=FAIL`（`:295`） |

两条新增检查独立抓住同一缺陷（延迟 2503ms + 唤醒数 0），且都是**原始实测数值**，
不是程序自述的字符串。作为对照：实验 B 里「唤醒次数上界 ≤12」**仍然是 PASS**
（0 ≤ 12）——这正好说明为什么上界检查必须与下界/延迟检查配合使用，
也解释了上一版为什么抓不到。

---

## 4. P2 修复：模块参数解析

`tests/runner/init.sh` 改动：

```sh
# 旧（错）：行内没有空格时，cut 会把整行原样返回 → 模块名被当成模块参数
args=$(echo "$line" | cut -d' ' -f2-)

# 新：先摘掉第一个字段，再删掉残留的前导空白；无参数时得到空串
args=$(echo "$line" | awk '{$1=""; sub(/^[ \t]+/, ""); print}')
```

顺带把日志行改成无参数时打印 `--- insmod sensor_char.ko ---`（旧版会打印多余空格）。

**验证**（`logs/20260914-030455-smoke.log`）：

```
--- insmod i2c-stub.ko chip_addr=0x48 ---      ← 有参数：正常
--- insmod sensor_char.ko ---                  ← 无参数：不再重复模块名
unknown parameter 出现次数: 0                   ← 修复前每份日志 2 次
```

---

## 5. P3 修复：阻塞 read 的时序依赖

原检查只要求"阻塞 read 成功返回"，而新 fd 若恰好赶上样本已就绪就会立即返回（没走等待路径）。
修复方法（写进 `user/io_models_test.c` 的 `test_blocking_read_and_wait()`）：

1. `ioctl(SENSOR_IOC_SET_INTERVAL, 1000)` 把周期拉长到 1000ms（放大"等待"与"没等"的耗时差异）；
2. 抽干到 `EAGAIN`，切回阻塞模式；
3. **第一次** read：只用来与"样本到达沿"对齐（耗时不确定，不断言）；
4. **第二次** read：此刻刚消费过样本，下一次样本要等约一个完整周期 → 断言耗时 ≥ 500ms；
5. 恢复默认周期 500ms（避免影响后面的 epoll 窗口统计）。

实测：修复前日志为 `blocking_wait_ms` 不存在（旧版没这个输出）；
修复后稳定在 **997ms / 1000ms**（两次复现），与理论值 1000ms 吻合。

---

## 6. 遗留风险与对后续阶段的建议

### 6.1 需要父级（项目负责人）决策的事项

1. **建议把新增检查项写进 `docs/11-阶段任务书.md` 的「阶段 01」检查表**。
   目前任务书写的是"至少包含"这些检查项，所以本强化不违反契约；但阶段 03 会重构
   `read` 的数据源（kfifo）、阶段 05 会另写 IIO 驱动，若契约里不固化这几条，
   后续实现者可能"简化"掉它们，P1 会重新出现。
2. **建议阶段 03/05 复用同一套判定思路**（下界 + 首次唤醒延迟 + 电平双向断言），
   而不是只断言"能读到数据"。

### 6.2 仍未覆盖（本次不做）

1. `io_models_test` 的总运行时长增加到约 8~10 秒（阻塞测试 2s + epoll 3s + SIGIO ≤3s）。
   仍在 180s 超时窗口内，可接受。
2. per-open 结构体的**内存泄漏**只做了"300 次 open/close 无告警"的间接验证
   （验证者已指出 slab 计数噪声会掩盖泄漏），本次未增加 `slabinfo` 差量测量。
3. `epoll_first_wakeup_ms` 的阈值 1000ms 依赖"默认采样周期 500ms"这一前提
   （测试结束时会把周期恢复为 500ms）。若阶段 03/04 改默认周期，需要同步调整该阈值。
4. 验证者登记的接口级遗留风险（`SENSOR_IOC_RESET` 把 `latest.seq` 归零后，
   已读过样本的 fd 会误判"有新样本"）本次**未修**——它属于契约定义内的行为，
   建议在阶段 03 引入单调 `sample_gen` 时一并处理。

---

## 7. 变更文件清单

| 文件 | 改动 |
|---|---|
| `user/io_models_test.c` | 新增 5 个输出键；epoll 段前置抽干；阻塞测试改为"先对齐边沿再计时"；新增电平三段断言与 per-open 断言；`checks` 汇总行拆出 `block_wait` |
| `tests/phases/01-io-models.sh` | 检查项 9 → 16；新增 `val_or`（缺值即失败）；新增下界/延迟/电平/per-open/ftrace 能力检查；注释说明每条判定针对哪个假通过 |
| `tests/runner/init.sh` | 修复 `cut -d' ' -f2-` 的模块参数解析（P2） |
| `docs/impl/01-io-models-测试强化记录.md` | 本文 |
