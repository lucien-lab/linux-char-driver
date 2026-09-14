# 阶段 03 验证报告：kfifo 环形缓冲 + mmap 零拷贝 + 多进程并发

- **验证者**：独立验证 agent（本阶段唯一被允许做临时改动与构建的 lane）
- **验证日期**：2026-09-14
- **被验代码基线**（工作区，未提交）：
  - `driver/sensor_char.c` sha256 `845ec4b3…78c0605`
  - `driver/sensor_ioctl.h` sha256 `2b7d75ce…60b6cadabe`
  - `user/ring_mmap_test.c` sha256 `c63307de…01aedba1f0`
  - `user/concurrency_test.c` sha256 `61f982b2…a2a48822f`
  - `user/io_models_test.c` sha256 `c8471598…a2874cd97106b4`
  - `tests/phases/03-ringbuffer-mmap.sh` sha256 `9df69b1f…3c1a3cb00136`
- **独立复现日志**（本次验证者亲自构建 + 亲自运行）：
  | 用途 | 日志 | 结论 |
  |---|---|---|
  | 基线 03 | `logs/20260914-111514-03-ringbuffer-mmap.log` | pass=28 fail=0 |
  | 基线 01 | `logs/20260914-111525-01-io-models.log` | pass=16 fail=0 |
  | 基线 smoke | `logs/20260914-111538-smoke.log` | pass=15 fail=0 |
  | 最终复跑（恢复源码后重新构建） | `logs/20260914-111852-03-ringbuffer-mmap.log`（28/0）、`logs/20260914-111859-01-io-models.log`（16/0）、`logs/20260914-111909-smoke.log`（15/0） | 全绿 |
  | 负控 NC-A/B/C/E | `logs/20260914-111633` / `111708` / `111727` / `111811`·`111823`·`111830`（均 03 阶段） | 见第 4 节 |
  | 负控 NC-D | `logs/20260914-111749-01-io-models.log` | 见第 4 节 |

**总评（先说结论）**：
功能层面阶段 03 的三项测试在**当前源码下可稳定复现全绿**（03=28、01=16、smoke=15，连续多次无抖动，无 panic/WARNING）。
但**本阶段的核心卖点「mmap seqlock 一致性读」并未真正实现**：共享区里的 `seq` 字段内核从不写入，
用户态的"读 seq → 读数据 → 重读 seq"重试逻辑是**死代码**；配套的两个检查项（`mmap_invalid=0`、
负控 A/B）对该缺陷**零判别力**。同时并发测试**无法发现多读者出队锁被移除**，
且 QEMU 单核（`smp: Brought up 1 node, 1 CPU`）下"8 进程 × 200 轮"**不能**作为"并发无竞态"的证据。
叠加 `docs/kb/03-ringbuffer-mmap-知识点.md` 缺失，**DoD 判定为「不通过」**（见第 9 节）。

---

## 1. 契约核对（docs/11「阶段 03」）

| # | 契约要求 | 实现 | 判定 |
|---|---|---|---|
| 1.1 | `struct sensor_dev` 增加 `struct kfifo ring`，probe 时 `kfifo_alloc(&ring, 64*sizeof(sample), GFP_KERNEL)` | `sensor_char.c:143`（成员）、`sensor_char.c:876`（alloc，参数 `SENSOR_RING_SAMPLES=64`） | ✅ 符合 |
| 1.2 | 下半部 `kfifo_put()` 入队，队满**丢弃新样本**并累加 `kfifo_dropped` | `sensor_char.c:311-313`；用 `kfifo_in_spinlocked`（内核文档推荐的加锁版） | ✅ 符合，策略与文档一致 |
| 1.3 | `read()` 从 kfifo 取（`kfifo_get`），无数据按阶段 01 语义等待/`-EAGAIN` | `sensor_char.c:469`（`kfifo_out_spinlocked`）+ 阻塞/`-EAGAIN`/超时分支 | ✅ 符合 |
| 1.4 | `sensor_has_new_sample()` 改为 `!kfifo_is_empty(&sd->ring)` | `sensor_char.c:375-378` | ✅ 符合（但删除了 per-open 参数，见 6.2） |
| 1.5 | `struct sensor_stats` **追加** `kfifo_dropped; ring_count;` | `sensor_ioctl.h:53-65`，字段追加在尾部，未动既有偏移 | ✅ ABI 兼容符合 |
| 1.6 | 一页 `__get_free_pages(GFP_KERNEL|__GFP_ZERO,0)` + `remap_pfn_range` + `PAGE_READONLY` | `sensor_char.c:891`、`sensor_char.c:568-599` | ✅ 符合 |
| 1.7 | `sensor_shm` 布局：magic/version/count/write_idx/seq/reserved[3]/samples[32] | `sensor_ioctl.h:37-52`；magic=0x53454E53("SENS")、version=1；`static_assert(sizeof<=PAGE_SIZE)`（实测 sizeof=800B） | ✅ 布局符合 |
| 1.8 | **写入方 `write_seqlock/write_sequnlock` 保护；用户态"读 seq→读数据→重读 seq"** | 内核侧确有 `write_seqlock`（`sensor_char.c:247/252`），**但共享区的 `seq` 字段从不被写入**（`grep 'shm->seq'` 无任何赋值） | ❌ **实质不符**（见 5.S1） |
| 1.9 | `.mmap` 检查映射长度，超过 1 页返回 `-EINVAL` | `sensor_char.c:573-574` | ✅ 符合（实测 errno=22） |
| 1.10 | `__get_free_pages`/`kfifo_alloc` 失败路径、remove 时释放 | probe 错误路径 `sensor_char.c:944-953`；remove `sensor_char.c:965-982`（先 `hrtimer_cancel`+`free_irq` 再 `free_pages`/`kfifo_free`） | ✅ 符合 |
| 1.11 | 多进程并发：允许多 open，**per-open 状态（`struct sensor_file`）保证互不干扰** | 实现**删除**了 `struct sensor_file`，改为全局共享队列语义 | ❌ 契约偏差（见 6.2；实现理由成立，但需父 agent 裁定） |
| 1.12 | `ring_mmap_test` 输出 `[RING] dropped=` / `mmap_reads=100 mmap_retries= mmap_invalid=` / `OVERALL=` | `ring_mmap_test.c` 均按契约输出（另有额外键） | ✅ 符合 |
| 1.13 | `concurrency_test` 输出 `[CONC] children=8 failed= invalid_samples= i2c_errors=` / `OVERALL=` | 会输出，但分成两行（`children=/failed=` 与汇总行），非契约写的单行逐字格式 | ⚠️ 轻微格式偏差（脚本按 token 解析，不影响判定） |
| 1.14 | 阶段测试至少包含 8 项检查 | 实际 28 项，契约 8 项全含 | ✅ 超出契约 |

---

## 2. 检查项表格（阶段 03，独立复现）

证据列为最终复跑日志 `logs/20260914-111852-03-ringbuffer-mmap.log` 的行号；「脚本行」为 `tests/phases/03-ringbuffer-mmap.sh` 中的断言行。

| # | 检查项 | 预期 | 实测 | 脚本行 | 证据行 |
|---|---|---|---|---|---|
| 1 | kfifo 队满丢弃计数可见 | dropped ≥ 1 | dropped=214 | 41 | 293 |
| 2 | 缓冲里仍有未读样本 | ring_count ≥ 1 | ring_count=85 | 44 | 294 |
| 3 | mmap 映射成功 | mmap_ok=1 | 1 | 46 | 295 |
| 4 | 共享区 magic/version | 均 =1 | 1/1 | 47 | 296 |
| 5 | seqlock 一致性读轮数 | mmap_reads=100 | 100 | 50 | 297 |
| 6 | seqlock 无撕裂样本 | mmap_invalid=0 | 0 | 53 | 298 |
| 7 | 快照内样本 seq 严格递增 | =1 | 1 | 56 | 299 |
| 8 | 连读 10 个样本序号严格递增 | =1 | 1（seq_span=9） | 59 | 300 |
| 9 | 连读样本数量达标 | =10 | 10 | 62 | 301 |
| 10 | 只读映射生效 | SIGSEGV | 1 | 65 | 302 |
| 11 | 超一页映射被拒绝 | errno=22 | 22 | 68 | 303 |
| 12 | 有数据时 read 立即返回 | <100ms | 1ms（周期 2000ms） | 75 | 304 |
| 13 | 测量前周期已拉到 2s | =2000 | 2000 | 80 | 305 |
| 14 | 新 fd 读到已缓冲样本 | =1 | 1 | 83 | 306 |
| 15 | ring_mmap_test 退出码 | 0 | 0 | 85 | 307 |
| 16 | ring_mmap_test OVERALL | PASS | PASS | 86 | 308 |
| 17 | 并发无失败子进程 | failed=0 | 0 | 94 | 344 |
| 18 | 并发无非法样本 | invalid_samples=0 | 0 | 97 | 345 |
| 19 | 并发无非预期 errno | io_errors=0 | 0（仅 child=0，见 5.S3） | 100 | 346 |
| 20 | 并发无 I2C 错误 | i2c_errors=0 | 0 | 103 | 347 |
| 21 | 并发确实读到样本 | samples_read>0 | 42 | 106 | 348 |
| 22 | 并发 mmap 快照有效 | snapshots>0 | 1600 | 109 | 349 |
| 23 | concurrency_test 退出码 | 0 | 0 | 111 | 350 |
| 24 | concurrency_test OVERALL | PASS | PASS | 112 | 351 |
| 25 | sensor_test 退出码 | 0 | 0 | 117 | 357 |
| 26 | sensor_test 正常结束 | 含「测试结束」 | 是 | 118 | 358 |
| 27 | stats 含环形缓冲字段 | 含 `ring:` | 是（count/dropped） | 119 | 359 |
| 28 | dmesg 无 WARNING/Call trace | =0 | 0 | 126 | 364 |

关键原始值（同日志）：`[RING] dropped=214 ring_count=85 irq_count=300`（300 = 10ms×3s，**85+214=300 守恒**）、
`drained_samples=10 seq_span=9`、`mmap_reads=100 mmap_retries=0 mmap_invalid=0`、`buffered_read_ms=1`、
`[CONC] samples_read=42 eagain=1558 invalid_samples=0 snapshots=1600 i2c_errors=0 failed=0`（第 277–343 行）。

**恒真/自证型检查**（不构成对驱动的判别力，不算错但不应计入覆盖率）：
- 第 13 项 `buffered_read_interval_ms=2000` 是程序自身写入的常量回读，**只要程序跑到该行必过**；它的作用是保护第 12 项的判别力，不是驱动功能检查。
- 第 22 项 `snapshots>0`、第 21 项 `samples_read>0` 只验证"程序跑到了这一步"，是很弱的下界。
- 第 27 项 `ring:` 只验证程序打印了该行（ioctl 成功即可），不校验数值语义。
- **没有任何一项检查 `mmap_retries` 的数值或"重试路径确实可达"**——见 5.S1、7.2。

`tag_val` 的默认值全部是"必然导致 FAIL"的哨兵值（`mmap_invalid` 默认 99、`failed` 默认 99…），缺输出不会放行，这一点做得对。

---

## 3. 代码审查隐患（按严重度排序）

### S1【高】共享区 `seq` 从不被内核维护 → 用户态 seqlock 协议完全失效
- **位置**：`driver/sensor_char.c:240-253`（`sensor_shm_publish` 只写 `samples[]/write_idx/count`，**没有 `shm->seq` 的赋值**）；`driver/sensor_ioctl.h:45`（声明了 `seq` 并写明"偶数=一致，奇数=写入中"）；`sensor_char.c:150`（seqlock 实际作用在内核私有的 `sd->shm_lock` 上）。
- **根因**：内核 `seqlock_t` 的序号计在 `sd->shm_lock` 里（`struct sensor_dev` 的私有内存），与用户态 `mmap` 到的那一页**没有任何关系**。`write_seqlock/write_sequnlock` 只保证"内核写者之间"互斥，对用户态读者不可见。
- **实测证据**：临时探针在共享区连读 50 次 `shm->seq`（跨 1 秒、约 100 次采样），
  `[RING] shm_seq_probe_min=0 max=0`（`logs/20260914-111633-03-ringbuffer-mmap.log:283`）。
- **后果**：用户态 `shm_snapshot()`（`user/ring_mmap_test.c:118/140`、`user/concurrency_test.c:92/106`）的
  `if (s1 & 1u) retry;` 与 `if (s1 != s2) retry;` 永远不成立，`mmap_retries` 恒为 0；
  读者可能在"`count/write_idx/samples` 正被改写到一半"时取数，且**没有任何机制能发现**。
- **正确做法**：写入侧显式维护共享序号，例如
  `shm->seq++`（奇）→ 写数据 → `smp_wmb()` → `shm->seq++`（偶）；或用 `write_seqcount_begin/end(&sd->shm_lock.seqcount)`
  并把 `shm->seq` 指向的字段与 `seqcount` 同步（更稳妥是手写 seq 字段，别依赖内核结构体内布局）。

### S2【高】并发检查无法发现"多读者出队锁缺失"，且单核环境使该证据先天不足
- **位置**：`driver/sensor_char.c:469`（读侧 `kfifo_out_spinlocked` + `ring_lock`）；`user/concurrency_test.c:67-78`（`parse_temp` 解析出 seq 后**丢弃不用**）。
- **负控**：把读侧改成无锁 `kfifo_out`（NC-E），03 测试**连续 3 次 28 PASS / 0 FAIL**
  （`logs/20260914-111811`、`111823`、`111830`）。
- **原因分析（诚实说明）**：
  1) QEMU 实际只起了 **1 个 CPU**（`logs/20260914-111852-…: "smp: Brought up 1 node, 1 CPU"`），
     读侧临界区是纯内存操作、其中没有抢占点，单核上两个读者**根本不可能**在临界区内交错 —— 所以该负控在单核下"抓不到"是**结构性**的，不完全说明检查项无能；
  2) 但反过来这正说明：**"8 进程 × 200 轮无失败"无法作为"并发无竞态"的证据**，它检测不到任何只在真并行下才会暴露的问题（缺锁、SMP 内存序、cache 一致性）；
  3) 即便真多核，测试也没有能发现重复/丢失样本的断言：8 个进程合计只真正读走 ~40 个样本（`eagain=1558`），
     `parse_temp` 丢掉 seq → 即使 `out` 索引被两个读者覆盖导致**同一样本被读出两次**，温度仍在区间内、`invalid_samples` 仍为 0。
- **建议**：① 并发测试每个子进程把自己读到的 `seq` 收集起来，父进程断言**所有 seq 全局无重复**（重复 = 出队竞态）；
  ② 在 QEMU 里加 `-smp 4`（需要改 `scripts/22`，属实现侧工作）；③ 更根本的手段是 `CONFIG_KCSAN=y`。

### S3【中】"并发下无非预期 errno"实际只读了 child=0 的计数
- **位置**：`tests/phases/03-ringbuffer-mmap.sh:100`；配合 `user/concurrency_test.c:299` 先逐子进程打印 `io_errors=`，第 320 行才打印汇总。
- **根因**：`tag_val()` 取 `head -1`，而 `io_errors` 在**每个 child 行**里都出现，因此拿到的是 `child=0` 的值，不是父进程汇总值（同样问题也影响第 22 项的 `snapshots`）。
- **影响**：检查项名（"并发下无非预期 errno"）与实际断言不符；child 1–7 的错误被忽略。
  未造成假绿是因为 `concurrency_test` 自身的 `OVERALL`/退出码（第 111–112 项）用了汇总值兜底。
- **建议**：脚本改为解析汇总行的唯一键（如把键改成 `total_io_errors=`），或对 `[CONC] samples_read=` 那行做定向 grep。

### S4【中】契约偏差：删除 `struct sensor_file` 与 per-open 语义
- **位置**：`driver/sensor_char.c:159-176`（注释说明退役）、`sensor_char.c:375-378`；`tests/phases/01-io-models.sh:132`（检查项改名）。
- **说明**：阶段 01 契约要求 `struct sensor_file{last_seq}` 与 `sensor_has_new_sample(struct sensor_file *)`；
  阶段 03 契约第 3 条仍写"per-open 状态（阶段 01 的 struct sensor_file）保证互不干扰"。
  实现把一个**共享队列**当作可读性判据，理由是"per-fd last_seq 在多进程共享队列时会谎报可读"——该理由**成立且修掉了一个真实矛盾**。
  但这属于对契约的实质性改写：`poll` 的语义由"每个 fd 独立看待序列"变成"全局队列长度"，
  阶段 01 那项"per-open 独立性"检查已名不副实（改名后不误导，但断言强度下降，见 7.1）。
- **处置建议**：由父 agent 明确批准该语义变更，并在 `docs/11` 阶段 03 节把该条改成与实现一致，避免后续阶段（05 IIO）按旧契约开发。

### S5【中】kfifo 实际容量 85 ≠ 契约的 64
- **位置**：`driver/sensor_char.c:103`（`SENSOR_RING_SAMPLES 64`）、`sensor_char.c:876`；probe 日志实测 `capacity=85`。
- **说明**：`kfifo_alloc` 把 1536B 向上取整到 2048B，`2048/24=85`。实现已在文档与 probe 日志中说明，`ring_count` 也按"样本数"换算（`sensor_char.c:390-393`），
  但契约字面写"容量 64 个"，且**用户态没有任何接口能读到真实容量**（`ring_capacity` 属阶段 04）。
  本阶段测试也不校验容量上界，属"已知且被记录"的偏差。

### S6【低】`sensor_mmap` 不校验映射偏移 `vm_pgoff`
- **位置**：`driver/sensor_char.c:568-599`。`mmap(..., fd, 4096)` 会再次映射同一页并成功（`remap_pfn_range` 直接用 `virt_to_pfn(sd->shm)`，忽略 `vma->vm_pgoff`）。
- **影响**：不会越界（同一页），但语义上"偏移不生效"容易误用；建议 `if (vma->vm_pgoff) return -EINVAL;`。

### S7【低】无锁读 kfifo 索引（当前语义无害，但 KCSAN 会报竞争）
- **位置**：`sensor_char.c:377`（`kfifo_is_empty`）、`sensor_char.c:392`（`kfifo_len`）。
- **说明**：`in/out` 是整字量，读到的值可能瞬时不一致；对 `wait_event` 条件/统计展示是良性的（条件会在唤醒后重新求值），
  但注释里"免锁读不会读到未写入的数据"的表述容易被误读为"没有数据竞争"。属可接受设计，建议注释里点明"这是有意的、良性的数据竞争（KCSAN 会报）"。

### S8【低】`read()` 在 `copy_to_user` 失败时样本已出队，样本被静默丢弃
- **位置**：`sensor_char.c:498-499`。`kfifo_out` 成功后 `copy_to_user` 失败直接返回 `-EFAULT`，样本不再回队，`kfifo_dropped` 也不增长（统计与实际不一致）。

### S9【低】实现文档的证据引用与实际日志不符
- **位置**：`docs/impl/03-ringbuffer-mmap-实现记录.md` 第七节，代码块写 `[TEST:END] 03-ringbuffer-mmap pass=28 fail=0`，
  但引用的日志 `logs/20260914-111043-03-ringbuffer-mmap.log` 实际是 **pass=25 fail=0**（该次运行的脚本是缺 3 项的旧版，
  缺"连读 10 个样本"两条与"周期已拉到 2s"一条）；表中 `dropped=214/irq=299` 的数值其实来自 `logs/20260914-111209-…`。
  数值本身正确、可复现（本次复跑 dropped=214、irq=300），但**引用错配**，不符合 DoD 第 5 条"结论必须能被日志复现"的严谨要求。

---

## 4. 负控实验（全部由验证者亲自改码 → 构建 → 跑 QEMU → 恢复）

所有临时改动均已用备份还原，并对每个文件做过 sha256 比对（还原后与基线逐字节一致），改动前后 `git status --short` 未增加任何新文件/新修改。

| ID | 改了什么 | 期望 | 实测 | 日志 / 关键行 | 判定 |
|---|---|---|---|---|---|
| **NC-A** | `user/ring_mmap_test.c` 读侧 seqlock 失效：`s1=0`、`s2=0`（重试与比较永真） | 应 FAIL 或 invalid>0 | **28 PASS / 0 FAIL**（`mmap_invalid=0`） | `logs/20260914-111633-03-ringbuffer-mmap.log`（`:283 shm_seq_probe_min=0 max=0`） | ❌ **检查无效**（预期外） |
| **NC-B** | `driver/sensor_char.c:247/252` 删除写入侧 `write_seqlock/write_sequnlock` | 应 FAIL | **28 PASS / 0 FAIL** | `logs/20260914-111708-03-ringbuffer-mmap.log` | ❌ **检查无效**（预期外） |
| **NC-C** | 丢弃策略改为"覆盖最旧样本"（`sensor_char.c:311`，dropped 恒 0） | dropped 检查应 FAIL | **25 PASS / 3 FAIL** | `logs/20260914-111727-03-ringbuffer-mmap.log:293`（dropped 检查 FAIL）、`:307`、`:308` | ✅ **检查有效**（确认） |
| **NC-D** | 去掉 read() 空判定（空则返回旧样本）+ `sensor_has_new_sample()` 恒 true | 01 的 poll/read 检查应 FAIL | **10 PASS / 6 FAIL** | `logs/20260914-111749-01-io-models.log:299`（blocking_wait=0）、`:300`（eagain=0）、`:305`（唤醒 108969 次 >12）、`:306`（消费后仍可读）、`:302`/`:310` | ✅ **检查有效**（确认） |
| **NC-E** | `sensor_char.c:469` 读侧改为无锁 `kfifo_out`（多读者出队竞态） | 并发检查应 FAIL | **28 PASS / 0 FAIL ×3 次** | `logs/20260914-111811`/`111823`/`111830-03-ringbuffer-mmap.log` | ⚠️ **未捕获**；单核下属结构性不可捕获（见 5.S2） |

**负控结论**：
- **有效**：`dropped ≥ 1`（NC-C）、01 的 poll/read 语义组（NC-D）——这两组检查确有判别力，且 NC-D 一次性触发 6 项 FAIL，符合 docs/11 对 stage 01「破坏性负控必须 ≥5 项 FAIL」的记录。
- **无效（本阶段必须整改）**：所有与 seqlock 相关的检查（`mmap_invalid=0`、`mmap_seq_monotonic`、`mmap_reads=100`）在 NC-A/NC-B 下毫无反应 ——
  因为内核根本没在维护用户可见的 `seq`（S1）。**当前 `mmap_invalid=0` 只证明了"窗口内样本的温度在区间内、样本 seq 递增"，没有证明"seqlock 不撕裂"。**

---

## 5. 独立复现结果

1. **构建**：`limactl shell dev bash -c 'TEST=03-ringbuffer-mmap bash scripts/13-vm-fast-cycle.sh'`
   → 4 次 `CC [M]` / 2 次 `LD [M]`，**无 error、无 warning**（全量输出 `/tmp/verify03-final-build.txt`）。
   用户态 5 个程序全部编译成功（`ring_mmap_test`/`concurrency_test`/`io_models_test`/`sensor_test`/`sensor_stat`）。
2. **03 阶段**：`bash scripts/22-macos-run-test.sh 03-ringbuffer-mmap 240` → 28 PASS / 0 FAIL / `[TEST:END]`，panic 0，告警 0。
3. **01 回归**：16 PASS / 0 FAIL / `[TEST:END]`。
4. **smoke 回归**：15 PASS / 0 FAIL / `[TEST:END]`。
5. **稳定性**：恢复源码后重新构建并复跑，三项均全绿；03 在前述多次运行中（基线 + NC-C 之外的运行）无抖动。
6. **产物一致性**：`Image`、`initramfs.cpio.gz`、`virt-sensor.dtb` 由 `scripts/13 → 21 → 22` 标准链路产出与运行，未绕过脚本自行拼 QEMU 命令。

---

## 6. `tests/phases/03-ringbuffer-mmap.sh` 专项审查

### 6.1 恒真/弱检查
- 见第 2 节末尾：第 13 项是"程序自证常量"，第 21/22/27 项是弱下界。**没有发现"写死 PASS"或"必然通过"的驱动检查**，
  但整体上**缺少对"丢弃守恒"的不变量断言**：实现文档最有力的证据 `ring_count + dropped == irq_count`（85+214=299）
  只写在文档里，脚本并未断言。建议补一条：在 `ring_mmap_test` 首次 `GET_STATS` 时输出 `irq_count`，
  脚本断言 `ring_count + dropped == irq_count`（该点在"无消费者"阶段成立，是能抓"样本无声蒸发"的强不变量）。
- 第 4/5/6 项（magic、mmap_reads、mmap_invalid）三者的取值都用 `tag_val` 从 `ring_mmap_test` 输出解析，默认值 fail-safe，**解析逻辑本身没问题**。

### 6.2 "无竞态"结论的证据充分性（如实评估）
- **能证明的**：多进程各自 `open` 不互相破坏；共享队列下 `read` 不崩、不返回越界温度；
  `mmap` 单写多读在**本机观察窗口内**未出现越界/明显错位；`I2C` 事务在并发下未报错（`i2c_errors=0`）。
- **不能证明的**：
  1. 单核 TCG（`1 CPU`）下不存在真正的并行执行，**所有** SMP 竞态（多读者 `out` 索引、内存序、cache 一致性）都不被触发；
  2. 负控 NC-E 已证实：即使**真的删掉读侧锁**，测试仍 3/3 全绿；
  3. `invalid_samples` 的判据（温度区间 + mmap 窗口内样本 seq 递增）**不看 read() 的 seq**，
     因此"同一样本被读两次 / 样本丢失"这类最典型的出队竞态症状**完全不可见**；
  4. "seqlock 重试"路径从未被触发（`retries=0` 属于低概率，但结合 S1，即使触发也永远为 0），
     所以"一致性读"这个词在本阶段是**名不符实**的。
- **结论**：`tests/phases/03` 里"8 进程 200 轮"应被表述为"**冒烟级并发回归**"，
  不能支撑"多进程并发无竞态"这一简历级结论；需要按 5.S2 的建议补强（seq 全局唯一性断言 + `-smp` + KCSAN）后才可升级表述。

---

## 7. `logs/` 交叉检查（关键实测值）

| 指标 | 本次实测 | 与实现文档/历史日志是否一致 |
|---|---|---|
| `kfifo_dropped`（10ms×3s 不读） | 214 / 215（多次运行） | 与 impl 文档 214 一致（`logs/20260914-110\*` 时为 214） |
| `ring_count` | 85 | 与 probe 日志 `ring ready: capacity=85 samples` 一致 |
| `irq_count` | 300 | 与 `ring_count+dropped=85+214` 守恒一致（文档写 299/214 对应另一次运行） |
| `mmap_reads / mmap_retries / mmap_invalid` | 100 / **0** / 0 | retries 恒 0，与 S1 相符（不是"运气好"，是机制失效） |
| 并发 `failed / invalid_samples / io_errors / i2c_errors` | 0 / 0 / 0 / 0 | 与文档一致；`samples_read=41~42`、`eagain≈1559` 与文档一致 |
| `buffered_read_ms`（周期 2s） | 0~1ms | 与文档 0ms 一致 |
| `i2c_err`（03 阶段全部日志） | 0（44 处 `i2c_err=0`） | 一致；另有 `i2c_err=3/6` 仅出现在阶段 02 的注入实验日志（`logs/20260914-040826-02b-inject-probe.log`、`…-040851-zz-kb-probe.log`），属预期 |
| 历史 bug 证据 | `logs/20260914-111013-03-ringbuffer-mmap.log` 的 `invalid_samples=1600`（fail=3） | 证实 impl 文档所述"RESET 未清共享区导致 seq 回退"确曾被 `invalid_samples` 抓住——**该检查项对"样本 seq 回退"这一类错误确有判别力**（但它抓的是样本自身 seq，不是 seqlock） |
| 引用错配 | `docs/impl/...:七` 引用的 `logs/20260914-111043` 实际 `pass=25` | ❌ 与文档正文 `pass=28` 不符（见 5.S9） |

---

## 8. 遗留疑点

1. **`shm->seq` 修好之后，重试概率依然极低**（写窗口 ~1µs / 20ms 周期 ≈ 5×10⁻⁵），
   即使修好也难以在 QEMU 里"自然"观测到重试。建议在验证中引入**可控放大器**：
   临时在写侧插入 `udelay(200)`（仅验证用），确认 `mmap_retries>0` 且 `mmap_invalid=0`；否则"seqlock 有效"仍无正向证据。
2. **`remap_pfn_range` + `PAGE_READONLY` 在真机（带 cache 属性/DMA）上的行为未验证**，文档已声明本项目不涉及真实 DMA。
3. **模块卸载与已建立 mmap 映射的并存**：当前依赖 VMA→file→module 引用链阻止 `rmmod`，未做显式保护；
   若将来允许 `rmmod` 时仍有映射，`free_pages`（`sensor_char.c:980`）会造成 UAF。未做实验验证，列为风险。
4. **`buffered_read_ms<100` 的阈值**依赖"测试前把周期拉到 2s"这一前置；若将来有人复用该程序而不拉周期，判别力会退化。
5. **`ring_mmap_test` 与 `concurrency_test` 各自重复实现了一份 `shm_snapshot`**（有意的），
   若修 S1 需要同时改两处，容易漏改其一；建议抽公共头文件（仍属实现侧工作）。
6. 阶段 05/06 会复用 `sensor_char.c`，**S1/S2 若不在阶段 03 收口，会被后续阶段继承**。

---

## 9. 逐条回答 docs/10 的 Definition of Done

| # | DoD 条目 | 判定 | 依据 |
|---|---|---|---|
| 1 | `make` 无 error、无 warning（新增代码部分） | **通过** | 恢复源码后重新构建：4×`CC [M]`、2×`LD [M]`，无任何 error/warning（`/tmp/verify03-final-build.txt`）。唯一一次 warning 出现在验证者负控 NC-C 的临时代码（`__kfifo_uint_must_check_helper` 未用返回值），与提交代码无关。 |
| 2 | `22-macos-run-test.sh 03-ringbuffer-mmap` 全 PASS 且到 `[TEST:END]` | **通过** | `logs/20260914-111852-03-ringbuffer-mmap.log`：pass=28 fail=0，`[TEST:END]` 在第 367 行，重复多次稳定。 |
| 3 | `smoke` 仍然全 PASS | **通过** | `logs/20260914-111909-smoke.log`：pass=15 fail=0。 |
| 4 | panic 0；WARNING/BUG/Call trace 需解释 | **通过** | 所有本次运行的 QEMU 解析结论均为「内核 panic: 0 / 内核告警: 0」；`[CHECK] dmesg 无 WARNING/Call trace` 在 03/01/smoke 均 PASS。 |
| 5 | 三份文档落盘，且结论能被日志复现 | **不通过** | ①`docs/kb/03-ringbuffer-mmap-知识点.md` **缺失**（`docs/kb/` 下只有 01、02）——违反 docs/10 第六节"每阶段三份文档"；②`docs/impl/03-…实现记录.md` 第七节引用的日志 `logs/20260914-111043-…` 实际是 25 项版（正文写 28），且表格数值取自另一份日志（见 5.S9）；③文档中"seqlock 保护共享区"的结论**与代码不符**（S1），不能从日志复现。 |

**阶段 03 总体判定：不通过。**

必须整改项：
1. **修 S1**（`shm->seq` 由内核维护），并把 seqlock 检查项改成"能被负控抓住"的形式；给出 `mmap_retries>0` 的正向证据（可用受控 `udelay` 放大窗口）。
2. **补 `docs/kb/03-…知识点.md`**；修正 `docs/impl` 的日志引用错配；把"并发无竞态"的表述降级为"冒烟级并发回归 + 单核限制说明"。
3. **修 S3**（`io_errors`/`snapshots` 取值取到 child=0）与补"丢弃守恒"不变量断言。
4. **处理 S4**（`struct sensor_file` 退役）——由父 agent 裁定后在 `docs/11` 同步契约，或恢复 per-open 语义。

非阻塞但建议一并处理：S2（并发 seq 唯一性断言 / `-smp` / KCSAN）、S6（mmap offset 校验）、S7（注释标注良性数据竞争）、S8（`copy_to_user` 失败回队）。

---

## 附：验证者操作合规声明

- 本验证期间**只临时修改**了 `driver/sensor_char.c` 与 `user/ring_mmap_test.c`（负控用），
  每次实验后立即用备份还原，并以 sha256 逐字节比对 + `git diff -q` 证明与基线一致；
- 最终又用**还原后的源码**重新构建并复跑 03/01/smoke，三项全绿；
- `git status --short` 在验证前后完全一致（6 个 modified、4 个 untracked，均为本次阶段既有改动，无新增）；
- 本次唯一写入的项目文件是本报告 `docs/verify/03-ringbuffer-mmap-验证报告.md`；
- 构建全部通过 `scripts/13-vm-fast-cycle.sh`，运行全部通过 `scripts/22-macos-run-test.sh`，未绕过标准链路。
