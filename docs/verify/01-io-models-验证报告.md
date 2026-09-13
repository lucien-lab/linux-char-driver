# 阶段 01（四种 IO 模型）独立验证报告

- 验证对象：`driver/sensor_char.c` + `user/io_models_test.c` + `tests/phases/01-io-models.sh`
- 契约：`docs/11-阶段任务书.md`「阶段 01」
- 验证者立场：**独立复核，任务是找反例**。本文所有数值均为本人重新执行得到，可复现。
- 验证时间：2026-09-14 02:52 ~ 03:00（日志时间戳可核对）
- 结论：**通过**（实现对契约无偏差；但测试体系存在 1 个中危「假通过」缺口，见 §5.3）

---

## 0. 结论（先给答案）

| 问题 | 结论 |
|---|---|
| 阶段 01 实现是否符合契约 | **符合**，逐条见 §2 |
| 是否发现问题 | **发现 3 个真实问题**（1 中 2 低），按严重度见 §0.1 |
| 是否有内核告警/panic | 无。两次独立复现 `内核 panic: 0 / 内核告警: 0` |
| 复现结果是否与实现者声称一致 | **一致**（9/9、12/12） |

### 0.1 发现的真实问题（按严重度排序）

| # | 严重度 | 问题 | 证据 | 影响 |
|---|---|---|---|---|
| P1 | **中** | 阶段测试无法发现「`.poll` 漏写 `poll_wait()`」这一类最常见的 poll 实现错误：删掉 `poll_wait` 后 **9 项检查全 PASS**（假通过） | 负控实验 B：`/tmp/neg-B.log`；见 §5.3 | 测试给了虚假覆盖度。驱动本身写对了，但**回归保护缺失**：将来任何人删掉 `poll_wait`，CI 会仍然全绿 |
| P2 | **低** | `tests/runner/init.sh:69` 的 `cut -d' ' -f2-` 在「行内无空格」时会把整行原样返回，导致**模块文件名被当成模块参数**传入：每次启动 dmesg 都出现 `sensor_char: unknown parameter 'sensor_char.ko' ignored`，日志里也是 `--- insmod sensor_char.ko sensor_char.ko ---` | 我方复现日志 `logs/20260914-025219-01-io-models.log:258,260`；`logs/20260914-025229-smoke.log:258,260`；`git show 53f1498:tests/runner/init.sh` 证明是**基线遗留**、非本阶段引入 | 当前无害（内核忽略未知参数、不为 0 退出）。但会掩盖真正写错的模块参数，且所有日志都带一条噪声告警；`docs/10` §DoD4 要求异常要解释，实现文档未提 |
| P3 | **低** | 「阻塞 read」检查项不保证真的走了 waitqueue 等待路径：新 fd 的 `last_seq=0`，若进入 read 时缓存里已有样本（`latest.seq != 0`），read 会立即返回，检查照样 PASS | 本次复现 `logs/20260914-025219-01-io-models.log:262-265`：open→close 仅 15ms；样本周期 500ms，说明**恰好**卡在等待窗口内，属时序巧合 | 检查项存在时序依赖；真正证明「阻塞等待路径」的是我的独立探针 `poll_first_sample` 等 501ms，以及 `epoll`/`SIGIO` 链路 |

另有一项**不属于阶段 01 交付、但被我在本次验证中实测确认**的边界（低）：`read()` 在 `count==0` 或 `copy_to_user` 失败时会**先把样本标记为已消费**再返回 0 / `-EFAULT`（`driver/sensor_char.c:348-358`），即零长度读会「吃掉」一个样本。契约未定义该场景，不构成偏差，登记为遗留项。

---

## 1. 独立复现（本人执行，非引用实现者日志）

严格按任务要求执行三条命令，`qemu-system-aarch64` + 项目脚本，未自拼 QEMU 命令：

```bash
cd /Users/lucien/workspace/self-study/projects/linux-char-driver
bash scripts/21-macos-sync-artifacts.sh
bash scripts/22-macos-run-test.sh 01-io-models 180
bash scripts/22-macos-run-test.sh smoke 180
```

| 步骤 | 实测结果 | 日志文件 |
|---|---|---|
| 21 同步产物 | 成功：`initramfs.cpio.gz 1.6M`、`Image 39M`、`i2c-stub.ko 64K` | — |
| 22 阶段测试 | **9 PASS / 0 FAIL**，`[TEST:END] 01-io-models pass=9 fail=0`（日志 299 行），跑到结束=是，panic 0，告警 0 | `logs/20260914-025219-01-io-models.log` |
| 22 回归 smoke | **12 PASS / 0 FAIL**，`[TEST:END] smoke pass=12 fail=0`（日志 289 行），跑到结束=是，panic 0，告警 0 | `logs/20260914-025229-smoke.log` |

**与实现者声称对比**：实现文档 `docs/impl/01-io-models-实现记录.md` 声称「阶段测试 9/9 PASS，回归 smoke 12/12 PASS」，引用的日志为 `logs/20260914-025014-01-io-models.log` / `logs/20260914-025021-smoke.log`。我逐行核对了这两份日志，**检查项名称、实测数值、PASS 数与我独立复现完全一致**，无夸大。

顺带核对了产物与日志的对应关系（避免「日志来自旧构建」）：VM 内 `~/lab/out/initramfs.cpio.gz` mtime `02:50:14.169`，宿主 `artifacts/initramfs.cpio.gz` mtime `02:50:14`，最后一次源码改动为 `02:50:08`（`tests/runner/init.sh`），`02:50:14` / `02:50:21` 两次运行确实晚于该构建 → **被引用的两份日志出自当前源码的产物**，不存在「跑的是旧 initramfs」问题。

---

## 2. 契约逐条核对（代码级证据）

行号均为当前 `driver/sensor_char.c`（HEAD `c96e4df`）。

| 契约条目（docs/11 §阶段01） | 判定 | 证据 |
|---|---|---|
| 1. `struct sensor_file { sd; last_seq; }` 存在 | 符合 | `sensor_char.c:117-120` |
| 1. `sensor_open()` 用 `kzalloc` 分配、`file->private_data = sf` | 符合 | `:275-281`（失败 `return -ENOMEM`） |
| 1. `sensor_release()` 调 `sensor_fasync(-1, filp, 0)` 且 `kfree` | 符合（顺序正确） | `:300`（摘除）早于 `:302`（kfree），`:303` 清空 `private_data` |
| 1. `struct sensor_dev` 增加 `fasync` 字段 | 符合 | `:97` |
| 2. `sensor_has_new_sample(sf)` 且 read/poll 共用 | 符合 | 定义 `:262-265`；read 用 `:328/339`；poll 用 `:381` —— 两条路径**同一函数** |
| 3. 有新样本 → 返回并更新 `sf->last_seq` | 符合 | `:348-349`：在 `sd->lock` 内取快照并写 `last_seq = snap.seq` |
| 3. 无新样本 + `O_NONBLOCK` → `-EAGAIN` | 符合 | `:329-330` |
| 3. 无新样本 + 阻塞 → 睡 `sd->wq`，2 秒超时 → `-ETIMEDOUT` | 符合 | `:338-344`（`SENSOR_READ_TIMEOUT_MS=2000`，`:72`；超时 `0`→`-ETIMEDOUT`，信号 `<0`→`-ERESTARTSYS`） |
| 3. 判定基准必须是 `sf->last_seq`（不能是「进入 read 时的 seq」） | 符合 | `git diff 53f1498 HEAD`：旧的 `unsigned long seq = READ_ONCE(sd->latest.seq);` 与 `latest.seq != seq` 已被删除 |
| 4. `sensor_poll(filp, wait)` 先 `poll_wait` 再判断 | 符合 | `:379`（poll_wait）在 `:381`（判定）之前，且未持 `sd->lock` |
| 4. 有样本 → `EPOLLIN|EPOLLRDNORM`，否则 0 | 符合 | `:381-384` |
| 4. 电平触发：消费后不再报 EPOLLIN | 符合 | 消费路径 `:349` 与判定 `:263` 构成闭环；独立探针实测「消费后 `poll(fd,0)`=0、未消费连续 5 次 `poll(fd,0)`=POLLIN」，见 §5.4 |
| 5. `sensor_fasync()` 用 `fasync_helper` | 符合 | `:395-400` |
| 5. `kill_fasync` 在「更新 latest + `wake_up_interruptible` 之后」 | 符合 | `:216`（`mutex_unlock`，`latest` 已在锁内写完）→ `:222`（`wake_up_interruptible`）→ `:231`（`kill_fasync`） |
| 5. `kill_fasync` 上下文安全 | 符合 | 位于**线程化下半部**（内核线程上下文，可睡眠/可持锁场景），`kill_fasync` 内部仅链表遍历 + `send_sigio`，不睡眠；全文件仅此一处于 `:231`（`grep kill_fasync` 命中 1 处调用） |
| 5. `#include <linux/poll.h>` | 符合 | `:51` |
| 6. fops 含 `.open .release .read .write .unlocked_ioctl .poll .fasync .llseek` | 符合 | `:496-506`，8 项齐全（`.owner` 另计） |
| 错误路径：`kzalloc` 失败 | 符合 | `:275-277` → `-ENOMEM` |
| 错误路径：`copy_to_user` 失败 | 符合 | read `:357-358` → `-EFAULT`；ioctl `:448/454/477` → `-EFAULT` |
| 用户态程序输出键名/格式逐字一致 | 符合 | `user/io_models_test.c` 实测输出见下（脚本 grep 全部命中） |
| 用户态程序退出码 0/非 0 | 符合 | `[IO] OVERALL=PASS` → `return 0`；负控 A/C/D 中均为 1，脚本捕获到 `实际='1'` |

**结论：契约 6 条 + 验收要求，无一条不符合。**

---

## 3. 阶段测试检查项核对表（预期 / 实测 / 证据）

「实测」列为**我本次独立复现**的数值（`logs/20260914-025219-01-io-models.log`），括号内为实现者日志同一项数值，用于交叉核对。

| # | 检查项名称 | 预期 | 实测 | 证据（文件:行号） |
|---|---|---|---|---|
| 1 | 阻塞 read 成功返回样本 | `[IO] blocking_read=ok` | ok（样本 `seq=1 temp=25.100C irq=1`） | 复现日志 `:279-280`；`[CHECK:PASS]` `:288`；实现者 `logs/20260914-025014-...log:279,288` |
| 2 | 非阻塞 read 返回 EAGAIN | `nonblock_eagain ≥ 1` | **1**（一致） | `:281`（原始值）、`:289`（判定） |
| 3 | epoll 在超时内报告可读 | `epoll_wait_ok=1` | 1（`epoll_reads=7 elapsed_ms=3000`） | `:283-284`、`:290` |
| 4 | 程序退出码 0 | 期望 `0` | 0 | `:291` |
| 5 | poll 电平触发 / 无忙轮询 | `epoll_wakeups ≤ 12` | **7**（一致） | `:282`、`:292` |
| 6 | SIGIO 异步通知到达 | `sigio_count ≥ 1` | **1**（一致） | `:285`、`:293` |
| 7 | 程序整体结论 | `[IO] OVERALL=PASS` | PASS | `:287`、`:294` |
| 8 | ftrace 捕获 `kill_fasync` | 次数 > 0 | **8**（一致） | `:295` |
| 9 | dmesg 无 WARNING/Call trace | 计数 = 0 | 0 | `:296` |
| — | 回归 smoke 12 项 | 全 PASS | 12/12 | `logs/20260914-025229-smoke.log:267-289` |
| — | 内核 panic | 0 | 0 | 22 脚本输出「内核 panic: 0」；两次运行均如此 |
| — | 内核 WARNING/BUG/Call trace | 0 | 0 | 22 脚本输出「内核告警: 0」；`grep -cE 'WARNING:\|Call trace:\|BUG:'` = 0 |

> 日志交叉核对结论：`logs/` 下与本阶段相关的 6 份日志我逐份 grep，**没有** `WARNING:` / `Call trace:` / `BUG:` / `Kernel panic`；唯一的异常行是 P2 的 `unknown parameter`（不含上述关键字，因此不影响检查项 9）。

---

## 4. 对抗性实验（本次验证的核心手段）

目的：**证明这些检查项不是恒真的**。做法：在 `/tmp` 里做驱动源码的「破坏性变体」，用真实内核树编成 `.ko`，替换进 initramfs 的**临时副本**后启动 QEMU。**项目源码/测试脚本/产物未被修改**（`git status` 全程干净；变体与临时 initramfs 全部在 `/tmp`）。

| 实验 | 变体（故意写错） | 预期 | 实测 | 结论 |
|---|---|---|---|---|
| **A** | `sensor_poll()` 恒返回 `EPOLLIN\|EPOLLRDNORM`（等价于契约警告的「用全局 seq 判断」） | 检查项 5 FAIL | `epoll_wakeups=**121642**`，`OVERALL=FAIL`，套件 `pass=6 fail=3` | ✅ 检查项 5 **不是恒真**；阈值 12 有 4 个数量级的分辨力 |
| **B** | 删掉 `sensor_poll()` 里的 `poll_wait()`（**经典漏写**） | 应 FAIL | **9 PASS / 0 FAIL，全绿假通过** | ❌ **测试缺口 P1** |
| **C** | `sensor_read()` 忽略 `O_NONBLOCK`（当阻塞处理） | 检查项 2 FAIL | `nonblock_eagain=0`，套件 `pass=6 fail=3` | ✅ 检查项 2 有效 |
| **D** | 删掉 `kill_fasync()` | 检查项 6/8 FAIL | `sigio_count=0`、`ftrace 0 次`，套件 `pass=5 fail=4` | ✅ 检查项 6/8 有效（双重证据） |

实验 A/C/D 的失败形态与预期完全一致，说明编写者在这些方向上的判定是**真判定**；B 暴露了唯一的方向性盲区。

---

## 5. 对「epoll_wakeups ≤ 12 能否证明电平触发语义」的回答

### 5.1 它能证明什么（不是废话）

`≤12` 是**上界**检查，它精确地排除「消费后仍上报 EPOLLIN」这一状态泄漏：实验 A 实测 121642 次 vs 正确实现 7 次。因此它**确实**验证了「消费后不再报可读」这半边语义，且与 `epoll_wait_ok=1`（下界：有数据时必须报可读）合起来构成「可读 ⇄ 有新样本」的双向约束。这一点实现文档与知识点文档 §6.4 的论证是站得住的。

### 5.2 它不能证明什么

1. **上界无法发现「唤醒次数过少」**。删掉 `poll_wait` 后唤醒数从 7 掉到 1，**仍在 ≤12 内**。
2. **`epoll_wait_ok=1` 只要求「至少被唤醒 1 次」**，而 `epoll_ctl(ADD)` 时的首次 `->poll` 就足够满足它（新 fd 的 `last_seq=0`，`latest.seq` 已 ≥1 → 立刻 POLLIN）。因此「poll 以后再也不唤醒 epoll」这种**功能性致命错误**被判定为 PASS —— 这正是实验 B 的假通过机理。
3. 数值 12 与 DT 里的 `poll-interval-ms = 500` 强耦合（本身已被实现者在遗留风险中登记），换周期即失效。
4. 「电平触发」的另一半性质（**未消费时可读状态持续存在**，即区分 level 与 one-shot）完全没被检查。

### 5.3 更可信的替代验证（我已实现并实测过）

我另写了一个独立探针（`/tmp/adhoc/fdtest.c`，静态编译进临时 initramfs 运行，**未进入项目**），在**同一份正确驱动**与**实验 B 的坏驱动**上分别跑：

| 探针断言 | 正确驱动 | 坏驱动 B | 证明力 |
|---|---|---|---|
| `poll(fd,0)` 消费后 = 0 | 0 ✅ | 0 | 电平触发下界（比 ≤12 更直接） |
| `poll(fd,0)` 未消费连续 5 次都 = POLLIN | 5/5 ✅ | 5/5 | **电平触发上界**（one-shot 实现会 1/5 失败；现有套件完全没覆盖） |
| 新开 fd 立即 `read()` 成功（41 字节） | ✅ | ✅ | **per-open 语义独立**（全局「已消费」实现会 EAGAIN） |
| `poll(1500ms)` 被下一个样本唤醒且 **延迟 ≤1000ms** | **486ms** ✅ | **1503ms** ❌ | **`poll_wait` 是否真登记**——注意只看返回值不管用：没有 `poll_wait` 时 `poll()` 会睡满超时，退出前最后一次 `vfs_poll` 仍能看到新样本（实测 1503/2003ms 才返回 1） |
| 300 次 open/close 后 dmesg 无 WARNING/BUG | ✅ | ✅ | release 路径（kfree + fasync 摘除）无 UAF 迹象 |

**具体修改建议（对 `user/io_models_test.c` + `tests/phases/01-io-models.sh`）**：

1. epoll 段**先抽干**（`read` 到 `EAGAIN`）再 `epoll_ctl(ADD)`，并把判定从 `wakeups ≥ 1` 提高为 **`wakeups ≥ 3`**（500ms×3s 期望 6~7，取 3 留足容差）——直接掐死实验 B 的假通过；
2. 增加两条断言并把实测值打进 `[IO]` 输出：消费后 `poll(fd,0)==0`、`epoll_wait` 返回后不读的情况下再 `poll(fd,0)` 仍为 POLLIN（level 语义）；
3. 记录并断言**首次唤醒延迟**（应 ≈ 采样周期，判 `≤1000ms`），这是发现「漏写 `poll_wait`」唯一可靠的观测；
4. 把 `≤12` 保留为**上界**（它已经有效），但必须与上面 1/3 配合使用，不能单独作为「电平触发」的证据。

### 5.4 对实现者「反例实验」的复核

`docs/kb/01-io-models-知识点.md` §6.4 给出了同样的反例方法，但**明确标注「未验证：本节只给出实验方法与预期」**。我把它真正跑了出来（实验 A：121642 次，脚本 `[CHECK:FAIL]`，`OVERALL=FAIL`），结论与其预期一致——**该文档在这一点上是诚实的，没有把推测写成实测**。这是加分项。

---

## 6. 测试脚本/用户程序的「恒真风险」审查

| 风险类型 | 是否存在 | 说明 |
|---|---|---|
| 写死结论 | 否 | 9 个检查项无一硬编码 `pass`；`grep -n 'pass "' tests/phases/01-io-models.sh` 仅 1 处（第 4 项内联判定，条件 `[ "$WAKES" -le 12 ]`，实测会被实验 A 判 FAIL） |
| 条件写反 | 否 | `check_gt ... "$EAGAIN_N" 0`、`check_gt "$SIGIO_N" 0`、`check_gt "$KF" 0` 方向均正确；实验 C/D 反证它们会 FAIL |
| 把 PASS 打印当结论 | 部分 | `check_contains ... "\[IO\] OVERALL=PASS"`（`:123`）确实是「信任程序自述」；但脚本同时**独立解析**了 `nonblock_eagain`、`epoll_wakeups`、`sigio_count` 三个原始数值，并用**退出码**做交叉约束，所以整体不是「只看自述」。实验 A/C/D 证明这三条独立解析都会 FAIL |
| 缺失输出即放行 | 否 | `[ -n "$EAGAIN_N" ] \|\| EAGAIN_N=0`、`WAKES` 缺省 9999 → 缺失一律判 FAIL |
| 程序压根没跑 | 否 | 若二进制缺失，`/tmp/io.txt` 为空，`check_contains` 全部 FAIL、`RC=127` → 套件 FAIL |
| ftrace 可选项被静默跳过 | **是（低）** | `FT_OK=0` 时第 8 项**不执行**，检查项总数从 9 降为 8，`[TEST:END]` 仍显示全 PASS。当前内核 `FT_OK=1`（`:295` 实测 8 次），不影响本次结论；但换内核时「9 项全绿」会静默变成「8 项全绿」，建议加一条 `pass "该内核支持 kill_fasync 函数过滤"` 之类的显式说明项 |
| 上界检查掩盖下界错误 | **是（中，即 P1）** | 实验 B 全绿假通过，见 §5 |
| 判定数值与 DT 强耦合 | 是（低） | `≤12` 依赖 500ms 周期，见 §5.2-3 |

---

## 7. 与契约/文档的逐条偏差

| 项 | 偏差 | 判定 |
|---|---|---|
| 契约 1~6（per-open / 判定函数 / read / poll / fasync / fops） | 无偏差 | 符合 |
| 契约「错误路径处理」 | 无偏差 | 符合 |
| 用户态输出格式与退出码 | 无偏差（额外多打印了 `blocking_sample`、`epoll_reads/elapsed_ms`、`checks` 三行，属允许的增量） | 符合 |
| 阶段测试脚本检查项 | 契约表 7 项**全部存在** + 1 项 ftrace 直接证据 | 符合 |
| 验收「文档给出 ftrace/dmesg 证据说明 kill_fasync 被调用」 | `docs/impl` §二、`docs/kb` §6.2 均给出 ftrace 8 次的证据，我复现为 8 次 | 符合 |
| `docs/10` §七.2「新增内核 API 使用处必须有一行注释说明作用与上下文约束」 | `poll_wait`/`kill_fasync`/`fasync_helper`/`wait_event_interruptible_timeout`/`READ_ONCE` 均有上下文说明 | 符合 |
| `docs/10` §八.4「禁止把 `[CHECK:PASS]` 直接写死在脚本」 | 未发现 | 符合 |
| `docs/10` §八.5「禁止绕过 13/21/22 自拼 QEMU」 | 实现者的正式结论均来自 22 脚本；我自己的负控实验在 `/tmp` 自拼 QEMU，属于验证手段，已在本文完整披露命令与做法 | 符合（本人实验已披露） |
| `docs/10` §DoD4「出现异常必须在文档里解释」 | **小偏差**：`unknown parameter 'sensor_char.ko' ignored` 出现在每一份日志里（P2），`docs/impl/01-io-models-实现记录.md` 未提及；严格说它不是 WARNING/BUG，故不违反 DoD 字面要求 | 低 |

---

## 8. 遗留疑点与风险

1. **P1（中）测试缺口**：`poll_wait` 漏写无法被发现。不是本阶段实现的缺陷，但是本阶段**验证体系**的缺陷，且会随阶段 02~06 继承（后续阶段的 `poll` 若无回归保护，删掉会全绿）。
2. **P2（低）测试框架参数解析 bug**：`tests/runner/init.sh:69`。修法（一行）：`args=$(echo "$line" | awk '{$1=""; sub(/^ /,""); print}')` 或先判断行内是否有空格。基线遗留、非本阶段引入，但本阶段改动了同一函数体（新增 `$PHASE` 选择逻辑）却没有顺手修。
3. **P3（低）「阻塞 read」检查项的时序依赖**：新 fd + 已有缓存样本时不会进入等待路径。建议在程序里先 `ioctl(GET_STATS)` 或等一个已知样本后再做阻塞读，或断言「本次 read 的耗时 ≥ 300ms」来证明它真的等了。
4. **实现者已登记、我确认成立的遗留项**（均与契约自洽）：
   - `SENSOR_IOC_RESET` 把 `latest.seq` 归零后，已读过样本的 fd 会误判「有新样本」并读到 `seq=0`。契约判定式就是 `latest.seq != sf->last_seq`，实现符合契约；现有测试不调用 RESET。**这是真正的接口级遗留风险，建议在后续阶段引入单调 `sample_gen`。**
   - `sigio_count` 恒为 1：标准信号不排队，判定用 ≥1 正确（我用 `sigtimedwait` 实测确为 1，且 ftrace 证明 `kill_fasync` 实际被调用 8 次）。
   - `≤12` 阈值与 500ms 周期绑定。
5. **未覆盖（我有意不做的）**：per-open 状态的**内存泄漏**只做了 300 次 open/close + dmesg 无告警的间接验证，没有做 slab 计数差量测量（`kmalloc-32` 的噪声可能掩盖 300×24B 的泄漏）。代码上是 `kzalloc`/`kfree` 严格配对（`:275` / `:302`），泄漏概率极低，但**不是**本文的实测结论。
6. **文档时间线提示**：`docs/kb/01-io-models-知识点.md` 的 mtime 为 `02:53`，即在我开始验证之后才落盘。我复核了它的引用数值（`epoll_wakeups=7`、ftrace 8 次、`sigio_count=1`）与日志一致，并已指出其 §6.4 标注为「未验证」——**该标注是诚实的**，我已替它跑出结果。

---

## 9. Definition of Done（`docs/10` 第五节）逐条回答

| # | 条款 | 结论 | 证据 |
|---|---|---|---|
| 1 | `make` 无 error、无 warning（新增代码部分） | **满足** | 我在 VM 里把当前 `sensor_char.c` 复制到 `/tmp/warnchk` 全新编译：`CC [M] / LD [M]` 共 4 行、`grep -inE "warning\|error"` 无命中；`user/io_models_test.c` 用 `gcc -static -O2 -Wall -Wextra` 编译 `exit=0` 无告警 |
| 2 | `22-macos-run-test.sh 01-io-models` 全 PASS 且跑到 `[TEST:END]` | **满足** | 本人复现 `logs/20260914-025219-01-io-models.log:288-296` 全 `[CHECK:PASS]`，`:299` `[TEST:END] 01-io-models pass=9 fail=0`；22 脚本输出「跑到结束: 是」 |
| 3 | `22-macos-run-test.sh smoke` 全 PASS | **满足** | 本人复现 `logs/20260914-025229-smoke.log:267-286` 12 项全 PASS，`:289` `pass=12 fail=0` |
| 4 | 日志 `内核 panic: 0`；`WARNING/BUG/Call trace` 必须在文档解释 | **满足（附说明）** | 两次运行 22 脚本均输出「内核 panic: 0 / 内核告警: 0」。日志中唯一异常是 `unknown parameter 'sensor_char.ko' ignored`（P2，基线遗留、非告警），实现文档未解释——建议补一句说明 |
| 5 | 三份文档已落盘（`docs/impl`、`docs/kb`、`docs/verify`），且文档结论能被日志复现 | **满足** | `docs/impl/01-io-models-实现记录.md`（294 行，引用两份日志、数值可复现）；`docs/kb/01-io-models-知识点.md`（1047 行，引用同一批日志，数值一致，反例章节如实标注「未验证」）；`docs/verify/01-io-models-验证报告.md` = 本文 |

**阶段 01 整体结论：通过（DoD 5/5 满足）。** 契约实现无偏差；唯一需要跟进的是 §0.1 P1（测试缺口）与 P2（框架小 bug），两者都不影响「驱动本身正确」这一结论，但 P1 会影响后续阶段的回归可信度。
