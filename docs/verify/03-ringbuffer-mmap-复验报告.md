# 阶段 03 复验报告：kfifo 环形缓冲 + mmap 零拷贝 + 多进程并发（整改后）

- **复验者**：独立复验者（工作树 `03v`，构建目录 `~/lab-03v`，日志写入本工作树 `logs/*-03v-*.log`，与其他工作树隔离）
- **复验日期**：2026-09-14
- **复验基线**：本工作树 `wt-03v` HEAD = `4a5165e`（工作区 `git status` 干净，未提交任何东西）
  - `driver/sensor_char.c` sha256 `7ab08c6d…77c4a0`
  - `user/ring_mmap_test.c` sha256 `6fdf67b4…ffb2bd`
  - `user/concurrency_test.c` sha256 `493a8073…e41d`
  - `tests/phases/03-ringbuffer-mmap.sh` sha256 `fc8f153d…b518b`
- **复验方式**：亲自 `scripts/13 → 21 → 22` 构建与运行，未绕过标准链路；负控临时改码后一律 `git checkout` 还原并复核 `git status`/`git diff`。
- **被比较的原报告**：`docs/verify/03-ringbuffer-mmap-验证报告.md`（初判「不通过」，问题 S1–S9）。

---

## 0. 复验结论（先说结论）

| 结论项 | 判定 |
|---|---|
| **S1（假 seqlock）是否真修好** | **实质修好**：内核亲手写 `shm->seq`（奇/偶协议 + 双侧 `smp_wmb`），用户态重试路径**不是死代码**（受控放大器下观测到 `mmap_retries≈1.3×10⁸`）。负控确认可证伪。 |
| **S2（单核不可证并发）是否解决** | **环境已改对**：QEMU 已用 `-smp 2` + `-accel tcg,thread=multi`，日志实测 `smp: Brought up 1 node, 2 CPUs`。但「并发无竞态」仍**不能写**（见第 4、5 节）。 |
| **整改是否可复现** | **不可稳定复现（阻塞项）**：同一份代码、同一条命令，我在本工作树连续 3 次干净运行 03 阶段分别是 **37/3、40/0、37/3**；失败的全部落在「放大器验证」3 项（`shm_seq_odd_seen=0` / `mmap_retries=0`）。集成负责人报告的「40/40 稳定」**不成立**。 |
| **DoD 五条** | 1/3/4 **通过**；2 **不通过（间歇性失败）**；5 **部分不符**（详见第 7 节）。 |
| **阶段 03 总判定** | **不通过**（技术实现本身正确，但验收测试的放大器配置不稳定、文档与代码/日志不一致，无法满足 docs/10 的 DoD 第 2、5 条）。 |

---

## 1. S1／S2 逐条复验表

### 1.1 S1——假 seqlock（初版：内核从不写 `shm->seq`，用户态重试是死代码）

| # | 复验点 | 代码/日志证据 | 判定 |
|---|---|---|---|
| S1.1 | 内核亲手写共享区 `seq`，实现奇/偶协议 | `driver/sensor_char.c:308-325`：`seq = READ_ONCE(shm->seq)+1; WRITE_ONCE(shm->seq, seq)`（→奇）… 写数据 … `WRITE_ONCE(shm->seq, seq+1)`（→偶） | ✅ 符合 |
| S1.2 | `smp_wmb` 位置正确（与 `raw_write_seqcount_begin/end` 一致） | `:310` 置奇**后**屏障、写数据；`:324` 写数据**后**屏障、置偶前。顺序 = 「先置奇再屏障」+「先屏障再置偶」，与 `include/linux/seqlock.h` 的 begin/end 语义一致 | ✅ 符合 |
| S1.3 | 加锁串行化多个内核写者 | `spin_lock(&sd->shm_lock)`/`spin_unlock` 包住整个发布过程（`:305`/`:330` 附近）；`shm_lock` 已从 `seqlock_t` 换成 `spinlock_t` | ✅ 符合 |
| S1.4 | 用户态重试逻辑**真的会被执行**（不是死代码） | 受控放大器（`shm_publish_delay_us`）下实测 `mmap_retries=130484077`、`shm_seq_odd_seen=162`（`logs/20260914-134522-03v-03-ringbuffer-mmap.log:293/296` 区段） | ✅ 符合 |
| S1.5 | 存在「抓假 seqlock」的确定性探针 | 用户程序先等一次 seq 变化（最多 500ms），脚本断言 `shm_seq_advances=1`；负控 NC-a 下探针 `0 → 0` 并被抓住（见第 2 节） | ✅ 符合 |
| S1.6 | 集成负责人的整数溢出修复（`retries` 用 `long long`） | `user/ring_mmap_test.c`：`long long retries_total`、`printf("%lld")`；对照历史日志 `logs/20260914-125014-03*.log` 曾出现 `mmap_retries=-712206102`（int 溢出），现大值正常打印 | ✅ 修好 |
| S1.7 | 放大器参数是否引入「读者长期占满窗口」问题 | `AMPLIFY_MS=2000`（紧凑循环 2s）、`shm_publish_delay_us=1000`（1ms 写窗口）。读者用 `retries` 上限 100000 保护（超限返回 -1 记 invalid），不会死循环；正常模式窗口 ~1µs，无副作用 | ⚠️ 无死循环风险，但**参数偏弱导致不稳定**（见第 3.5、6.1 节） |

**S1 结论**：假 seqlock 的根因（`seqcount` 在私有内存、共享区字段从不写）**已被正确修复**，且策略与 docs/11 阶段 03 给出的参考写法完全吻合。代之以「确定性探针 + 可证伪负控」的检查设计，方向正确。

### 1.2 S2——单核不可证并发

| # | 复验点 | 证据 | 判定 |
|---|---|---|---|
| S2.1 | QEMU 使用 `-smp 2` | `scripts/22-macos-run-test.sh:45` `-smp 2`；`scripts/20-macos-gen-dtb.sh:28` `-smp 2`（dtb 生成侧同步） | ✅ 符合 |
| S2.2 | QEMU 使用 MTTCG | `scripts/22-macos-run-test.sh:45` `-accel tcg,thread=multi`，并附注释说明「轮转 TCG 下同一时刻只有一个 vCPU 在跑」 | ✅ 符合 |
| S2.3 | 日志证明两 CPU 真的在跑 | 我本次全部 03/01/smoke 日志均有 `smp: Brought up 1 node, 2 CPUs`（如 `logs/20260914-134842-03v-03-ringbuffer-mmap.log:79`） | ✅ 符合 |
| S2.4 | 出队全局唯一性断言已加入 | `user/concurrency_test.c` 每子进程收集 `read()` 的 `seq`（`__sync_fetch_and_add` 追加），父进程排序查重；脚本断言 `dup_seqs=0` | ✅ 已实现 |
| S2.5 | 该断言**能被负控触发** | ❌ **否**：NC-c（把 `kfifo_out_spinlocked` 换成无锁 `kfifo_out`）实测 **40 PASS / 0 FAIL**（`logs/20260914-134658-03v-03-ringbuffer-mmap.log`，`dup_seqs=0`） | ❌ **未确认** |

**S2 结论**：环境侧整改到位（双核 + MTTCG），但「出队竞态可被检测」这一点**仍未取得证据**——删掉读侧锁后测试依旧全绿。这与原报告 S2 的结论一致（当时单核不可捕获）；现在虽已双核，负控仍抓不到。

---

## 2. 负控实验（本复验者亲自改码 → 构建 → 跑 QEMU → 还原）

所有临时改动均已 `git checkout` 还原；还原后 `git diff --stat` 为空，`git status --short` 相对起点无新增（唯一 untracked 是集成负责人并发写入的 `docs/kb/03-…知识点.md`，非我产生）。

| ID | 改了什么 | 期望 | 实测 | 日志（本工作树）/ 关键行 | 判定 |
|---|---|---|---|---|---|
| **NC-a** | `driver/sensor_char.c:309/325` 删掉写侧两处 `WRITE_ONCE(shm->seq, …)` | 「seq 推进」与放大器相关检查 FAIL | **33 PASS / 7 FAIL**，含 `共享区 seq 真的在内核侧推进（探针：0 → 0）`、`shm_seq_changes=0` | `logs/20260914-134626-03v-03-ringbuffer-mmap.log`，FAIL 行 315、407–411 | ✅ **有效** |
| **NC-b（标准配置）** | `user/ring_mmap_test.c` 去掉「奇数重试」+「s1==s2 校验」（重试/比较恒不成立） | 重试路径检查 FAIL、撕裂样本应 >0 | **37 PASS / 3 FAIL**（`retries=0`、`odd_seen=0`），但本轮**恰好没撞上写入窗口**，`invalid=0`，无法判定撕裂检查 | `logs/20260914-134737-03v-03-ringbuffer-mmap.log`，FAIL 行 408、409、411 | ⚠️ **配置太弱，结论被环境掩盖** |
| **NC-b（20ms 强放大器）** | 同上，另把窗口临时放大到 `shm_publish_delay_us=20000`、`AMPLIFY_MS=12000` | `mmap_invalid>0` 触发 FAIL | **37 PASS / 3 FAIL**，含 `重试后数据始终一致（放大窗口下 mmap_invalid=0）`→实测 **invalid=39**，`odd_seen=8588707` | `logs/20260914-134801-03v-03-ringbuffer-mmap.log`，FAIL 行 410、411、412 | ✅ **有效（但仅在强放大配置下）** |
| **NC-c** | `driver/sensor_char.c:555` 读侧改无锁 `kfifo_out`（去掉 `ring_lock`） | 「出队全局无重复」FAIL（`dup_seqs>0`） | **40 PASS / 0 FAIL**，`dup_seqs=0` | `logs/20260914-134658-03v-03-ringbuffer-mmap.log` | ❌ **未捕获（检查无判别力/环境不给机会）** |
| **NC-d** | 入队满时改为「覆盖最旧样本」（`dropped` 不再增长） | 「丢弃守恒」/`dropped≥1` FAIL | **33 PASS / 7 FAIL**，含 `dropped=0`、`样本守恒 … 85 + 0 vs irq 300，差 215` | `logs/20260914-134712-03v-03-ringbuffer-mmap.log`，FAIL 行 305、308 | ✅ **有效** |

**负控小结**：
- 初版报告里「零判别力」的两条核心检查（`seq` 推进、重试/invalid）**现在都能被负控证伪**（NC-a 确定性；NC-b 在强放大窗口下 `invalid=39`）。S1 的整改**在检查层面也落地了**。
- **但**：NC-b 在**当前提交的标准配置（1ms/2s）下无法稳定验证**——本轮 `odd_seen=0`，与「不改代码但环境没给机会」的失败形态完全一样（见第 3 节）。这说明「正/负证据」都依赖运气。
- **NC-c 仍抓不到**：删掉读侧锁后 40/0。「出队全局唯一性」在 2 vCPU TCG + 当前负载下**未取得判别力证据**，S2 的整改目标只完成了一半。

---

## 3. 独立复现结果

| 用途 | 日志（本工作树 `logs/`） | 结果 |
|---|---|---|
| 03 干净复跑 #1 | `20260914-134433-03v-03-ringbuffer-mmap.log` | **37 PASS / 3 FAIL**（放大器 3 项），`[TEST:END]` 行 438 |
| 03 干净复跑 #2 | `20260914-134522-03v-03-ringbuffer-mmap.log` | **40 PASS / 0 FAIL**（`retries=1.30e8, odd_seen=162`） |
| 03 干净复跑 #3（还原后最终复跑） | `20260914-134842-03v-03-ringbuffer-mmap.log` | **37 PASS / 3 FAIL**（`odd_seen=0`） |
| 01 回归 | `20260914-134535-03v-01-io-models.log`、`20260914-134854-03v-01-io-models.log` | 均 **16 PASS / 0 FAIL** |
| smoke 回归 | `20260914-134546-03v-smoke.log`、`20260914-134905-03v-smoke.log` | 均 **15 PASS / 0 FAIL** |
| 构建 | `scripts/13`（`WORKTREE=03v`） | 无 C error / 无 C warning；仅有 kbuild 的 `Clock skew detected`（宿主/虚拟机时间戳告警，非代码告警） |

**关键结论**：03 阶段在同一份源码、同一命令下 **3 次里 2 次 37/3**，失败点固定为：

```
[CHECK:FAIL] 读者确实撞上过写入窗口（shm_seq_odd_seen=0 > 0）
[CHECK:FAIL] seqlock 重试路径真的被执行（mmap_retries=0 > 0）
[CHECK:FAIL] 放大器模式程序退出码为 0
```

三条同源：紧凑循环跑了 510 万次快照、观察到 200 次 `seq` 变化，**一次都没落在 1ms 的「写入中」窗口**——只有两个 vCPU 在 TCG 下被有效串行化时才会出现这种组合。也就是说，**这 3 项检查的通过与否取决于宿主调度，而不是被测代码**。

01/smoke 稳定全绿。

---

## 4. KCSAN 证据核对（不采信转述，逐文件核对）

| 集成负责人的说法 | 我在日志中核对到的事实 | 判定 |
|---|---|---|
| 普通内核下 03 阶段 40/40 | `logs/20260914-130201-03-ringbuffer-mmap.log`（严格模式内核）：`[TEST:END] … pass=40 fail=0` | ✅ 一致 |
| KCSAN_STRICT 内核下 03 阶段零 data-race | 同日志 `:71 kcsan: strict mode configured`、`:99 selftest 3/3 passed`、全文 `BUG: KCSAN` **0 处**、`:81 smp: Brought up 1 node, 2 CPUs` | ✅ 一致 |
| 01 阶段唯一报告在 mm/rmap，报告块内无驱动符号 | `logs/20260914-130220-01-io-models.log:315` 起：`BUG: KCSAN: data-race in folio_add_file_rmap_range / page_remove_rmap`，调用栈为 `filemap_map_pages → __handle_mm_fault`、`exit_mmap → begin_new_exec` 等，**报告块（315–356 行）内无 `sensor_*`/`virt_i2c*` 符号**；同日志 01 阶段 16 PASS / 0 FAIL | ✅ 一致 |
| smoke 在 KCSAN 内核下 | `logs/20260914-130238-smoke.log` 严格模式，读完未见 `BUG: KCSAN` | ✅ 一致 |

**限制（必须写明）**：这些 KCSAN 日志来自 2026-09-14 13:02，**早于**本工作树 HEAD 提交时间 13:27；当前内核树已重建为普通配置（无 `CONFIG_KCSAN`），本次复验**未重跑 KCSAN**。因此 KCSAN 证据的「内核代码版本 = 提交版本」只能通过日志内 40/40 与放大器数值间接佐证，未被本次独立重跑覆盖。

---

## 5. 「无竞态」证据等级评估与建议措辞

### 5.1 现在**能**写什么（有日志证据）

1. **seqlock 协议真实成立**：内核亲手维护用户可见 `seq`（奇/偶 + 双侧屏障），受控放大器下读者重试路径被真实执行（`mmap_retries≈1.3×10⁸`）且 `mmap_invalid=0`；**负控可证伪**（删写侧 `seq` → 探针 0→0 等 7 项 FAIL；去读侧重试 + 强放大 → `invalid=39`）。
2. **出队/丢弃语义正确**：`ring_count + kfifo_dropped == irq_count`（差 0~1）在无消费者窗口成立；改为「覆盖最旧」立即被抓住（差 215）。
3. **冒烟级并发回归**：`-smp 2` + MTTCG 下 8 进程 × 200 轮，`failed=0 / invalid_samples=0 / dup_seqs=0 / i2c_errors=0`，无 panic/WARNING。
4. **动态检测**：KCSAN_STRICT 核下 03 阶段零 data-race 报告（日志真实存在，见第 4 节）。

### 5.2 现在**不能**写什么

1. **「多进程并发无竞态」的结论性表述**（无正向证明；NC-c 证明即使删掉读侧锁也测不出来）。
2. **「测试 40/40 稳定可复现」**（3 次干净运行 2 次 37/3）。
3. 把「2 vCPU TCG 下的通过」等同于「真机 SMP 无竞态」（TCG 与硬件内存模型、调度、cache 行为不同）。

### 5.3 建议的确切措辞（可直接抄进文档/简历）

> 阶段 03 的并发正确性采用**三重证据**：① QEMU MTTCG 双核（`-smp 2 -accel tcg,thread=multi`）下 8 进程 × 200 轮并发回归全绿，并新增「出队全局唯一性（`dup_seqs=0`）」与「样本守恒（`ring_count+dropped==irq_count`）」不变量断言；② 针对 mmap 共享区实现用户可见的 seqlock（内核维护奇/偶 `seq` + 双侧 `smp_wmb`），并用**受控放大器**（`shm_publish_delay_us`）拿到「重试路径被真实执行且无撕裂样本」的正向证据，配套负控可证伪；③ `CONFIG_KCSAN_STRICT` 内核下同一负载零 data-race 报告（唯一报告位于 `mm/rmap` 进程 exec/exit 路径，与驱动无关）。
> **边界**：以上为「冒烟级并发回归 + 静态/动态检测」的高置信度证据，**不等于数学证明**；TCG 硬件的并行度受宿主调度影响，且「出队全局唯一性」断言在负控（去读侧锁）下未能触发，真正的并发保证仍应以 KCSAN/lockdep 与真机 SMP 压测为准。

> ⚠️ 在上述①修好之前（放大器检查稳定可复现），措辞里**不要**出现「40/40 全绿」这类数字承诺。

---

## 6. 新发现问题

| ID | 级别 | 问题 | 证据 |
|---|---|---|---|
| **N1** | **高** | **放大器检查不稳定**：标准配置 `shm_publish_delay_us=1000` + `AMPLIFY_MS=2000` 下，约 1/3~2/3 的运行完全撞不上 1ms 写窗口，导致 3 项 FAIL。集成负责人的「40/40 稳定」不成立。 | 本复验 3 次干净运行：`…134433`(37/3)、`…134522`(40/0)、`…134842`(37/3)；失败形态固定，`iters≈5.1e6, seq_changes≈200, odd_seen=0, retries=0` |
| **N2** | 中 | **实现文档与测试脚本参数不符**：`docs/impl/03…实现记录.md` 第 3.2.3 节表格写「窗口 20ms、观测 15s、retries 6~8 亿」，但已提交脚本用的是 1ms / 2s。文档描述的是更稳的旧配置，实际交付的是更弱（且不稳定）的配置。 | `docs/impl/…:205-219` vs `user/ring_mmap_test.c:89`(AMPLIFY_MS=2000) + `tests/phases/03…sh:162`(delay=1000) |
| **N3** | 中 | **出队全局唯一性仍无判别力证据**：NC-c（无锁 `kfifo_out`）40/0。该断言目前只是「没抓到也算过」。 | `logs/20260914-134658-03v-03-ringbuffer-mmap.log` |
| **N4** | 低 | **实现记录头部仍引用 28 项旧日志**（`logs/20260914-111209…` pass=28），与当前 40 项脚本/日志不一致，违反 DoD 第 5 条「结论能被日志复现」。 | `docs/impl/03…实现记录.md:6-9,407` |
| **N5** | 低 | 放大器模式下写者在 `spin_lock` 内 `udelay(20000)`，会把 `sd->lock`(mutex) 一起持 20ms；仅测试参数使用，生产默认 0，无实际风险，但文档应说明「放大参数会显著拉长中断线程临界区」。 | `driver/sensor_char.c` `sensor_shm_publish` |
| **N6** | 低 | `rmmod` 与活跃 mmap 并存的 UAF 风险（原报告遗留疑点 3）**未处理**：仍依赖 VMA→file→module 引用链阻止卸载；测试脚本 `rmmod/insmod` 时恰好无映射，未覆盖该场景。 | `tests/phases/03…sh:159-166` |

**续验项**：`shm_publish_delay_us` 生产默认值确认为 **0**（`module_param` 默认 0；普通日志 `shm publish delay = 0 us`；脚本放大器实验后 `rmmod`+无参 `insmod` 复位，`logs/20260914-134842…:267/379/418`）——**符合要求**。

其他原报告问题复查：S3（`io_errors`/`snapshots` 取到 child=0）已修（子进程键加 `child_` 前缀，汇总行键唯一）；S5（容量 85）已在脚本新增 `ring_capacity≥64` 与守恒断言；S6（mmap offset 校验）**未修**（仍不校验 `vm_pgoff`）；S8（`copy_to_user` 失败回队）已在 read 路径处理并有 `dropped_at_capacity` 断言覆盖。

---

## 7. 逐条回答 docs/10 的 DoD 五条

| # | DoD 条目 | 判定 | 依据 |
|---|---|---|---|
| 1 | `make` 无 error、无 warning（新增代码部分） | **通过** | `scripts/13`（WORKTREE=03v）无 C error/warning；唯一输出是 kbuild 的 `Clock skew detected`（文件系统时间戳告警，非代码告警）。 |
| 2 | `scripts/22 … 03-ringbuffer-mmap` 全 PASS 且到 `[TEST:END]` | **不通过** | 本复验 3 次干净运行：2 次 37/3、1 次 40/0（失败为放大器 3 项）。**不可稳定全 PASS**。 |
| 3 | `smoke` 仍全 PASS | **通过** | `logs/20260914-134905-03v-smoke.log`：15 PASS / 0 FAIL。 |
| 4 | panic 0；WARNING/BUG/Call trace 需解释 | **通过** | 本次全部运行「内核 panic: 0 / 内核告警: 0」；KCSAN_STRICT 下 01 阶段唯一 `data-race` 报告位于 `mm/rmap`（与驱动无关，已在第 4 节解释）。 |
| 5 | 三份文档落盘且结论能被日志复现 | **部分不符** | ①`docs/kb/03-ringbuffer-mmap-知识点.md` 在本复验进行中（13:46–13:47）才由集成负责人写入，当前为 **untracked**（尚未提交）——存在性满足、入库性未定；②`docs/impl/03…实现记录.md` 头部仍引用 28 项旧日志（N4），放大器参数与实际脚本不符（N2）。 |

**阶段 03 总体判定：不通过。**

阻塞整改项（按优先级）：
1. **修 N1**：把放大器配置改成能稳定命中窗口的参数（例如回到文档所述的 20ms 窗口 / 15s 观测，或按宿主并行度自适应：窗口不足时重试/延长），使 03 阶段可稳定 40/40；或把该 3 项改为「统计量 > 0 时才算证据、==0 时打印 WARN 不计 FAIL」的可辩驳设计（但需在文档里明说其概率性质）。
2. **补 N3**：为「出队全局唯一性」设计能真正触发重复的负控（提高读者并发/样本产出率、或注入窄窗口），否则该断言不得作为并发证据计入。
3. **修 N2/N4**：让实现记录的参数、日志引用与交付脚本/日志一致；确认 `docs/kb/03` 已提交。

---

## 附：复验者操作合规声明

- 本复验只**临时修改**了 `driver/sensor_char.c`、`user/ring_mmap_test.c`、`tests/phases/03-ringbuffer-mmap.sh`（负控用），每次实验后 `git checkout` 还原；还原后 `git diff --stat` 为空、`git status --short` 与起点一致（唯一 untracked 文件 `docs/kb/03-…知识点.md` 为集成负责人并行写入，非本次复验产生）。
- 最终用**还原后的源码**重新构建并复跑 03/01/smoke（`…134842` / `…134854` / `…134905`）。
- 本复验唯一写入的项目文件是 `docs/verify/03-ringbuffer-mmap-复验报告.md`；**未** `git commit`、**未** 修改 `~/kernel-build`、**未**修改 `kernel-src/`。
- 构建一律经 `scripts/13-vm-fast-cycle.sh`（`WORKTREE=03v`），运行一律经 `scripts/21-macos-sync-artifacts.sh` + `scripts/22-macos-run-test.sh`；dtb 由 `scripts/20-macos-gen-dtb.sh` 重新生成（本工作树原本无 `artifacts/`）。
- 日志全部落在本工作树 `logs/*-03v-*.log`，与主检出/其他工作树隔离。
