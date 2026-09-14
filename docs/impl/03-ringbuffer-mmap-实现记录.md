# 阶段 03 实现记录：kfifo 环形缓冲 + mmap 零拷贝 + 多进程并发

- **代码提交**：待父 agent 统一提交（本阶段工作区状态：`driver/sensor_char.c`、`driver/sensor_ioctl.h`、
  `user/{ring_mmap_test,concurrency_test,io_models_test,sensor_test}.c`、`tests/userspace/sensor_stat.c`、
  `tests/phases/{03-ringbuffer-mmap.sh,01-io-models.sh}`）
- **验证日志**：`logs/20260914-111209-03-ringbuffer-mmap.log`（03 阶段，pass=28 fail=0；
  稳定性复跑 `...-111258-*.log` / `...-111305-*.log` / `...-111312-*.log` 三次均 pass=28 fail=0）、
  `logs/20260914-111221-01-io-models.log`（回归 pass=16 fail=0）、
  `logs/20260914-111232-smoke.log`（回归 pass=15 fail=0）
- **测试结论**：03-ringbuffer-mmap **28 PASS / 0 FAIL**；01-io-models **16 PASS / 0 FAIL**；
  smoke **15 PASS / 0 FAIL**（三项日志均无 `WARNING:`/`Call trace:`）

---

## 一、这一阶段到底解决了什么问题

阶段 02 结束时，驱动的数据通路是"**单样本覆盖**"：

```
线程化中断下半部 → sd->latest（只有一个样本） → read()/ioctl()
```

只要读方的节奏比采样慢，中间产生的样本就被**静默覆盖**：用户态永远看不到丢了多少。
阶段 03 把它改成"**一个生产者、三个出口**"：

```
                        ┌── kfifo 环形缓冲（85 个样本）──► read() / poll / epoll / SIGIO
传感器 --I2C--> 中断下半部 ┼── mmap 共享页（seqlock）──────► 用户态零拷贝直接读
   （唯一生产者）          └── latest（最新一个）────────────► ioctl(GET_SAMPLE)
```

三个出口各有存在理由：`read/poll` 是标准的 VFS 接口；`mmap` 解决高频取值时的拷贝开销；
`latest` 提供"立刻拿到当前值"的语义（不需要排队）。**它们共享同一个数据源，
所以不存在"读出来不一致"的问题**。

---

## 二、kfifo 通道

### 2.1 数据结构与容量

```c
#define SENSOR_RING_SAMPLES	64
struct kfifo	ring;		/* kfifo_alloc(&ring, 64 * sizeof(struct sensor_sample), GFP_KERNEL) */
spinlock_t	ring_lock;	/* 保护 kfifo 读写的自旋锁 */
u32		kfifo_dropped;	/* 队满丢弃的新样本数 */
```

**kfifo 容量会被向上取整**，这是必须先搞清楚的细节：
`64 × sizeof(struct sensor_sample)(24B) = 1536` 字节，而 `kfifo_alloc` 要求 2 的幂，
于是实际分配 2048 字节 → `mask + 1 = 2048` → 实际可放 `2048/24 = 85` 个样本。
驱动在 probe 时把真实容量打出来（实测日志：`ring ready: capacity=85 samples (24 bytes each)`），
并提供 `sensor_ring_capacity()` 做换算；`GET_STATS` 返回的 `ring_count` 也是
**样本数**（`kfifo_len()/sizeof(sample)`），不是字节数 —— 否则用户态会拿到一个有歧义的量纲。

### 2.2 为什么必须显式加锁（而不是直接用 `kfifo_put/kfifo_get`）

内核文档（`include/linux/kfifo.h:21`）明确写了：

> Replace the use of `kfifo_put` into `kfifo_in_spinlocked` and `kfifo_get` into `kfifo_out_spinlocked`

原因是 kfifo 的**免锁性质只覆盖"单读者 + 单写者"**：`__kfifo_in/__kfifo_out` 通过
`smp_wmb()` 保证数据先于索引可见，但 `in`/`out` 两个索引各自的读改写并不是原子的。
本设备允许**多个进程各自 open 后同时 read()**（阶段 03 的并发测试就是这么干的），
多读者必须串行化，否则 `out` 索引互相覆盖 → 样本错位。
所以：生产者与消费者统一走 `kfifo_in_spinlocked()` / `kfifo_out_spinlocked()`，
共用一把 `ring_lock`（这两个宏内部是 `spin_lock_irqsave`，临界区只有内存拷贝，不会睡眠）。

### 2.3 丢弃策略：丢新不丢旧

`kfifo_in_spinlocked()` 返回值 = 实际写入字节数，队列满时为 0：

```c
if (kfifo_in_spinlocked(&sd->ring, &sample, sizeof(sample),
			&sd->ring_lock) != sizeof(sample))
	sd->kfifo_dropped++;
```

**为什么丢"新"而不是覆盖"旧"**：

| 方案 | 行为 | 优点 | 缺点 |
|---|---|---|---|
| 丢新（本项目） | 队满时丢弃刚产生的样本，计数 | 历史数据不被改写；丢了多少可观测；实现简单 | 最新样本可能进不了队列 |
| 覆盖旧 | 队满时挤掉最旧的样本 | 队列里永远是最新的 N 个 | 静默改写历史；"丢了多少"不可知；需要额外逻辑 |
| 阻塞生产者 | 队满时让中断线程等待 | 不丢数据 | **中断上下文不允许**；且采样节奏由硬件决定，不该被读方拖住 |

选择"丢新 + 计数"的三个理由：
1. 已排队的数据是"用户态还欠着没读的历史"，**静默改写历史**比丢新更难排查；
2. 计数让"读方跟不上采样"变成**可观测量**（`kfifo_dropped` 持续增长就是告警信号）；
3. 最新值这条路径不受影响：即使队列满，`latest` 与 mmap 共享区仍然会更新，
   `ioctl(GET_SAMPLE)` 永远能拿到最新样本。

**实测一致性校验**（本次日志）：周期 10ms、静置 3 秒不读 →
`irq_count=299`（≈ 3s/10ms，样本全部产生）、`ring_count=85`（队列被填满）、
`kfifo_dropped=214`。**85 + 214 = 299**：每一个样本要么在队列里、要么被计数丢弃，
没有"无声蒸发"的样本。这条等式是本阶段最有说服力的证据。

### 2.4 read/poll 语义的变化：per-open 状态退役

阶段 01 为了让 `poll` 有正确的电平语义，引入了 `struct sensor_file { u32 last_seq; }`
（"本 fd 消费到哪个 seq"）。阶段 03 把它**删除**了，原因是一个真实的语义矛盾：

> 可读性若按 per-fd 的 `last_seq` 判断，多进程共享同一队列时会谎报可读：
> A 进程把样本读走了，B 进程的 `last_seq` 不会跟着变 → B 的 `poll` 报 EPOLLIN，
> 但 B 的 `read()` 却拿不到数据（队列已空）→ 违反 `EPOLLIN` 的契约（"read 不会阻塞"）。

现在可读性只有一个判据：**共享队列是否非空**（`sensor_has_new_sample()`），
它与 `read()` 的成功条件完全一致，因此不可能出现上面那种矛盾。

**代价（必须说清楚）**：一个刚打开的 fd 只有在"缓冲里确实还有未读样本"时才立刻可读，
而不是"设备曾经产生过样本就永远立即可读"。阶段 01 测试里对应的检查
（`new_fd_immediately_readable`）因此需要建立确定性前提，见第六节。

---

## 三、mmap 零拷贝通道

### 3.1 布局与"为什么不直接暴露 kfifo"

```c
/* driver/sensor_ioctl.h —— 内核与用户态共用同一份定义 */
#define SENSOR_SHM_SAMPLES 32
#define SENSOR_SHM_MAGIC   0x53454E53u   /* "SENS" */
#define SENSOR_SHM_VERSION 1u
struct sensor_shm {
	__u32 magic, version, count, write_idx, seq;
	__u32 reserved[3];
	struct sensor_sample samples[32];
};
```

**不直接 mmap kfifo 的原因**：kfifo 的内部结构（`in`/`out` 索引、环形数组、字节容量）
是内核实现细节，跨版本可能变化；把它暴露给用户态等于把内核内部结构变成 ABI。
这里换成稳定的自定义布局（magic/version 开头，字段语义写死在文档里，
`reserved[3]` 给后续阶段留位不移动 `samples` 偏移）。

布局定义放在**与用户态共用的头文件**里（`driver/sensor_ioctl.h`），
和 ioctl 结构体同样的道理：两边布局不一致会导致"看起来正常其实错位"的读数，
让编译器保证一致是最省事的做法。

### 3.2 用户态可见的 seqlock：协议、屏障与"假实现"教训

> 本节在阶段 03 整改时**整节重写**。初版实现只在内核侧用了 `seqlock_t`，
> 共享区里的 `seq` 字段从未被写入，用户态的重试逻辑是**死代码**——
> 这是本阶段最大的问题（验证报告 S1），必须完整记录下来。

#### 3.2.1 协议：序号必须由驱动亲手维护在共享区里

```c
/* 写入侧（线程化中断下半部） */
spin_lock(&sd->shm_lock);                 /* 序列化写者，且禁止 RESET 与发布交错 */
seq = READ_ONCE(shm->seq) + 1;
WRITE_ONCE(shm->seq, seq);                /* → 奇数：写入中 */
smp_wmb();                                /* 与 raw_write_seqcount_begin() 一致 */
if (sd->shm_publish_delay_us)
        udelay(sd->shm_publish_delay_us); /* 测试放大器，默认 0，见 3.2.3 */
shm->samples[shm->write_idx] = *s;
shm->write_idx = (shm->write_idx + 1) % SENSOR_SHM_SAMPLES;
if (shm->count < SENSOR_SHM_SAMPLES)
        shm->count++;
smp_wmb();                                /* 与 raw_write_seqcount_end() 一致 */
WRITE_ONCE(shm->seq, seq + 1);            /* → 偶数：一致 */
spin_unlock(&sd->shm_lock);
```

两侧屏障的顺序照抄内核 `raw_write_seqcount_begin/end()`
（`include/linux/seqlock.h`）：begin 是"先置奇、再屏障"，end 是"先屏障、再置偶"。
少任何一个屏障，读者都可能拿到"序号检查通过、数据却是旧的"的结果。

读取侧（用户态，`user/ring_mmap_test.c:shm_snapshot`）：

```c
s1 = shm->seq;                 /* 1. 先读序号 */
if (s1 & 1u) retry;            /*    奇数 = 写方正在更新 */
__sync_synchronize();          /* 2. 全屏障（用户态没有 smp_rmb） */
... 抄 samples[]/count/write_idx ...
__sync_synchronize();          /* 3. 全屏障 */
s2 = shm->seq;
if (s1 != s2) retry;           /*    期间写过 → 重试 */
```

#### 3.2.2 教训：为什么"内核用了 seqlock_t"对用户态毫无意义

- **初版写法**：`seqlock_t shm_lock` + `write_seqlock/write_sequnlock`，
  共享区里的 `seq` 字段只在 `sensor_ioctl.h` 里声明、注释写着"偶数=一致"，**却从没有被赋值**。
- **根因**：`seqlock_t` 的序号记在自己的 `seqcount` 里（`struct sensor_dev` 的私有内存），
  用户态 `mmap` 到的那一页**看不到它**。内核侧的锁只保证"内核写者之间互斥"，
  对用户态读者没有任何可观测的一致性信号。
- **为什么没被测试抓住**：配套检查项只断言"`mmap_invalid == 0`"，而
  "不写 seq" 并不会让数据变坏（写者的临界区极短），所以断言恒真。
  验证者用负控实验证明了这一点：删掉写侧 `write_seqlock`（NC-B）、
  让用户态重试条件永假（NC-A），测试**仍然 28 PASS / 0 FAIL**。
- **修法**：把 `seqlock_t` 换成普通 `spinlock_t`，在共享区里亲手维护用户可见的 `seq`；
  并用**确定性探针**（等待 seq 变化，最多 500ms）替代"循环里 min/max"的弱检查——
  注意 100 轮一致性读只花几毫秒，跨不过一个 10ms 采样周期，
  用 min/max 会把"窗口太短"误判成"seq 没在动"（整改过程中真的这么误判过一次）。

#### 3.2.3 受控放大器：怎么把"seqlock 生效"变成可观测的正向证据

普通运行时写窗口只有 ~1µs，而采样周期是 10ms：读者撞上"写入中"的概率约 10⁻⁴，
意味着**正向证据天然拿不到**（这也是初版"假 seqlock"能长期潜伏的原因）。
为此加了模块参数 `shm_publish_delay_us`（默认 0，仅在测试时由脚本 insmod 打开），
把写入窗口放大到 20ms（占空比约 67%），于是：

| 观测项 | 正常模式 | 放大器模式（20ms 窗口，15s 观测） |
|---|---|---|
| 快照次数 | 100 | 180 万 ~ 790 万 |
| 撞上写入窗口次数（`shm_seq_odd_seen`） | 0 | 约 500 |
| 重试次数（`mmap_retries`） | 0 | 6 亿 ~ 8 亿 |
| 撕裂样本（`mmap_invalid`） | 0 | **0** |
| 共享区 `seq` 是否推进（确定性探针） | 是（600→602，5ms） | 是（64→66，10ms） |

反向对照（NC-1：把写入侧的两处 `WRITE_ONCE(shm->seq, ...)` 去掉，
即复现整改前的假 seqlock）：**同一窗口下 `mmap_invalid=24`、8 项检查 FAIL**。
"好实现 invalid=0 / 坏实现 invalid>0，用的是同一套断言"——这才叫检查项有判别力。

**环境依赖要如实说明**：撞上窗口的概率取决于 QEMU 两个 vCPU 的真实并行度。
实测 3 秒窗口时约 1/3 的运行完全撞不上（现象是 `shm_seq_changes=300` 但
`shm_seq_odd_seen=0`——读者读到了 300 次序号变化却从未落在窗口内），
因此窗口取到 15 秒，并在脚本里打印每次的实测数字，便于区分"协议问题"与"环境没给机会"。

### 3.3 mmap 实现与约束

```c
static int sensor_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sensor_dev *sd = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > PAGE_SIZE) return -EINVAL;        /* 只分配了一页，多映射会读到无关内存 */
	if (vma->vm_flags & VM_WRITE) return -EPERM; /* 内核单方写入，用户态写会破坏一致性假设 */

	vma->vm_page_prot = PAGE_READONLY;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	return remap_pfn_range(vma, vma->vm_start, virt_to_pfn(sd->shm),
			       size, vma->vm_page_prot);
}
```

几个必须讲清楚的约束：

1. **内存必须来自线性映射区**。共享页用 `__get_free_pages(GFP_KERNEL | __GFP_ZERO, 0)`
   分配，属于内核直接映射（linear map）区，`virt_to_pfn()` 可直接换算成物理页帧号，
   `remap_pfn_range()` 就能把它填进用户页表。
   **如果改用 `vmalloc()` 分配**，虚拟地址与物理页不再一对一，必须
   `vmalloc_to_pfn()` 逐页换算，或者改成实现 `vm_ops->fault` + `vm_insert_page()`
   让缺页时逐页插入 —— 这也是为什么"随手把 vmalloc 内存丢给 remap_pfn_range"
   是常见错误。
2. **`PAGE_READONLY` 要显式设置**。`vma->vm_page_prot` 默认来自用户请求的 `PROT_*`，
   让内核侧的只读语义依赖调用方参数很脆弱；显式覆盖后，即使调用方要求 PROT_WRITE
   也会被 `VM_WRITE` 检查拒绝（实测：写入映射区触发 SIGSEGV）。
3. **`vm_flags` 在 6.3+ 是只读成员**（`struct vm_area_struct` 里是 union 中的
   `const vm_flags_t`），必须用 `vm_flags_set()` 修改 —— 直接 `|=` 编译报错
   （`error: assignment of read-only member 'vm_flags'`），这是本阶段实际踩到的编译错误。
4. **真机上的额外问题（QEMU 里不存在，但面试会问）**：
   - **cache 一致性**：如果这块内存要交给 DMA，在非一致性 cache 的 ARM 平台上，
     设备写入的数据可能停留在 cache 里，CPU 读到旧值 —— 那时需要 `dma_alloc_coherent()`
     分配 + `dma_sync_single_for_{cpu,device}()` 做同步。本项目的"设备"是另一个软件模块，
     不涉及真实 DMA，所以用普通内存即可。
   - **映射权限与 cache 属性**：真机上如果要映射设备的寄存器空间（MMIO），
     不能用普通内存的 `remap_pfn_range` 语义，通常要 `ioremap` +
     `pgprot_noncached()/pgprot_writecombine()`。
   - **安全性**：把内核内存映射给用户态后，内核无法再单方面保证那块内存不被窥探，
     所以只映射"该给用户看的数据"，不要把整个设备结构体映射出去（本项目只映射
     `struct sensor_shm` 这一页）。

### 3.4 零拷贝的收益量化

| | 每次取值的开销 |
|---|---|
| `read()` | 一次系统调用 + 一次 `copy_to_user`（≈24~64 字节拷贝）+ 内核态格式化成文本 |
| `mmap()` | **零系统调用、零拷贝**：纯用户态读页表里的同一块物理内存 |

**实测（队列已满、且把采样周期拉到 2 秒时）：`buffered_read_ms=0`**。
为什么要先把周期拉大到 2 秒再测：如果周期还是 10ms，"等下一个样本"的实现最多只等 10ms，
用 100ms 阈值区分不出两种实现 —— 那是弱检查。周期 2s 时两者相差三个数量级
（实测 0ms vs 理论 ~2000ms），这条断言才真正具备判别力。
（这是阶段 01 那次教训的延续：**每一项检查都要问"什么样的错误实现会让它失败"**。）

真正的价值在**高频取值**：`read` 的 cost 随取值次数线性增长，`mmap` 则是一次映射、
之后每次取值只有一次内存读。代价是接口复杂度移到用户态（要自己解析布局 + 处理 seqlock 重试）。

---

## 四、多进程并发

### 4.1 共享状态清单（并发测试针对的就是这三处）

| 共享状态 | 保护方式 | 若不做会怎样 |
|---|---|---|
| kfifo 环形缓冲 | `ring_lock`（spinlock） | 多读者互相覆盖 `out` 索引 → 样本错位/重复 |
| `latest`、统计计数 | `sd->lock`（mutex） | 统计错乱；`latest` 撕裂 |
| mmap 共享区 | seqlock（内核写 / 用户态重试读） | 用户态读到写了一半的窗口 |

### 4.2 测试设计

`user/concurrency_test.c`：8 个子进程，每个独立 `open("/dev/sensor0")`，
循环 200 轮做三件事：`read()`（非阻塞）/ `ioctl(GET_SAMPLE)` / mmap seqlock 快照校验。
每轮 `usleep(1000)` 让 8×200 轮真正跨越多个采样周期（否则整轮测试几百微秒就跑完，
采样与读取得不到重叠，"并发"名不副实）。

子进程结果通过 **`MAP_SHARED | MAP_ANONYMOUS` 共享内存**回传（父进程 fork 前映射、
子进程继承），父进程用 `waitpid()` **逐个精确回收**并汇总 —— 这也是并发编程里
"结果怎么回传"的一个标准做法（比让子进程往 stdout 打印再解析更可靠，不会交错）。

判定项（任一不满足即为 FAIL）：
- `failed=0`：没有子进程异常退出（退出码非 0 说明打不开设备或撞上非预期错误）
- `invalid_samples=0`：读到的样本温度都在区间内、mmap 快照内 `seq` 严格递增
- `io_errors=0`：没有非预期 errno（**EAGAIN 不算错误**，它是"队列暂时为空"的正常返回）
- `i2c_errors=0`：并发没有破坏总线访问（有的话说明锁没保护住 I2C 事务）
- `samples_read > 0`、`snapshots > 0`：确实读到了东西，不是"空跑一遍全 EAGAIN"

实测：`samples_read=41 eagain=1559 invalid_samples=0 snapshots=1600 i2c_errors=0 failed=0`。
41 个样本被 8 个进程瓜分、1559 次 EAGAIN 正常返回 —— 说明队列确实是共享且被竞争的。

### 4.3 怎么证明"无竞态"而不是"没撞上"（诚实结论）

必须承认：**200 轮 × 8 进程跑通 ≠ 数学上证明无竞态**。它只能提高置信度，而且 QEMU
单核 + TCG 还会削弱并发交错的机会（真正的多核交错在 TCG 下被串行化执行）。
本测试的价值在于"**一旦实现有明显错误就会高概率暴露**"，三点依据：

1. 本轮它**确实抓到了一个真实 bug**：`RESET` 没清共享区导致窗口内 `seq` 回退，
   被 `invalid_samples` 检出（1600 次全部命中）—— 说明这项检查有判别力，不是摆设。
2. 多进程同时出队会高频触发 kfifo 的 `out` 索引争用；如果去掉 `ring_lock`，
   在 8 进程 × 200 轮下几乎必然出现重复/错位样本。
3. 边界条件（队列满 + 消费者并发）在测试里被主动构造（预热 300ms + 20ms 周期）。

**要真正做竞态检测，正确的工具是**：
- **KCSAN**（`CONFIG_KCSAN=y`）：编译期内核的动态数据竞争检测器，
  给每个内存访问插桩并记录 happens-before，能报出真实的数据竞争；
- **lockdep**（`CONFIG_PROVE_LOCKING`）：检测锁的使用错误（递归加锁、锁顺序反转）——
  本阶段那个"中断下半部重复 `mutex_lock` 导致死锁"的 bug，如果开着 lockdep 就会
  在第一次触发时直接报 `possible recursive locking detected`，而不是等到系统卡死；
- 内存检测：`CONFIG_DEBUG_KMEMLEAK`、`CONFIG_SLUB_DEBUG` 查泄漏/越界。
本项目当前内核配置里没开这些（它们会显著变慢 QEMU 启动），
但把"下一步可以怎么做得更严格"写清楚，比含糊地说"验证过了"更有价值。

---

## 五、本阶段踩到的坑（都已在代码注释里标注）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 测试跑到第一次 read 就卡死，`dmesg` 出现 `sched: RT throttling activated` | 我在中断下半部里**重复了一次 `mutex_lock(&sd->lock)`**（原代码在函数入口已加锁）→ mutex 不可重入 → 直接死锁；持有锁的 RT 中断线程卡住，系统失去响应 | 删掉重复加锁，并在代码里写明"本函数入口已持有 sd->lock，不要再加锁" |
| 2 | `concurrency_test` 每次迭代都报 1 个非法样本（1600/1600），mmap 快照内 `seq` 回退 | `SENSOR_IOC_RESET` 把 `latest.seq` 归零、清了 kfifo，但**没清 mmap 共享区** → 窗口里还留着旧样本（seq=300+），新样本从 seq=1 重新开始 → 同一个窗口内序号先大后小 | RESET 时用 `write_seqlock` 一起清 `count/write_idx`；并在注释里说明这个"重置不彻底"的坑 |
| 3 | 编译报 `error: assignment of read-only member 'vm_flags'` | Linux 6.3 起 `vma->vm_flags` 是 union 中的 `const vm_flags_t`，禁止直接赋值 | 改用 `vm_flags_set(vma, ...)` |
| 4 | `kfifo_dropped` 与预期容量对不上（64 vs 85） | `kfifo_alloc` 把字节容量向上取整到 2 的幂：1536 → 2048 字节 → 85 个样本 | 不改行为，而是把真实容量换算并打印（`sensor_ring_capacity()`），文档里说明 |
| 5 | 01-io-models 的"新 fd 立刻可读"在队列语义下会时序性失败 | per-open `last_seq` 退役后，可读性取决于"此刻队列是否非空"，而上一个测试结束时队列恰好空不空是随机的 | 在 `test_poll_level` 里先静置两个采样周期（生产者投递、无人消费），把前提变成确定性 |

---

## 六、对既有测试的改动（必须由验证者复核）

**改动 1：`user/io_models_test.c` —— 新增 `sleep_ms()` 并给"新 fd 立刻可读"补前提**

```diff
-	/* (a) per-open 独立性：全新 fd 立即应报可读（前提是设备已有过样本） */
+	/* (a) 全新 fd 应立刻报可读 —— 前提是"缓冲里此刻确实还有未读样本"。
+	 *     阶段 03 起可读性由共享环形缓冲是否非空决定；这里先静置两个采样周期，
+	 *     让生产者投递且无人消费，把前提变成确定性的而不是时序碰运气。 */
+	sleep_ms(2 * DEFAULT_INTERVAL_MS);
 	if (poll_readable_now(fd) == 1)
```

**改动 2：`tests/phases/01-io-models.sh` —— 只改这一个检查项的**描述文字**，断言完全不变**

```diff
-check_eq "新开的 fd 立刻可读（last_seq 按 fd 独立记录）" "1" "$NEWFD"
+check_eq "新开的 fd 立刻可读到已缓冲样本（队列非空即可读）" "1" "$NEWFD"
```

**自查结论**：01-io-models 的**检查项数量与断言强度均未降低**（改动前后都是 16 项全 PASS，
数值阈值一个都没动）。描述文字必须改的原因：旧文字写的是"last_seq 按 fd 独立记录"，
引入 kfifo 后 `last_seq` 已退役，留着这句会让检查项**名不副实**
（阶段 02 的验证者恰好因为"检查项名与断言不符"提过一次问题）。
补偿措施：阶段 03 自己新增了更严格的一项
`新开的 fd 立刻读到已缓冲样本（队列语义）`（`[RING] new_fd_sees_buffered=1`）。
**请验证者重点复核这两处改动是否构成"放宽"**，如认为不妥，可改为在 03 脚本中独立断言。

---

## 七、验证证据（可复现）

**阶段测试**：`bash scripts/22-macos-run-test.sh 03-ringbuffer-mmap 240`

```
[CHECK:PASS] × 28（含下列关键项）
[CHECK] kfifo 队满丢弃计数可见（实测 dropped=214 ≥ 1）
[CHECK] 缓冲里仍有未读样本（实测 ring_count=85 ≥ 1）
[CHECK] 连读 10 个样本序号严格递增（历史被保留，不是只有最新一个）
[CHECK] 连读样本数量达标（drained_samples=10）
[CHECK] 共享区 magic/version 正确
[CHECK] seqlock 一致性读轮数（mmap_reads=100）
[CHECK] seqlock 一致性读无撕裂样本（mmap_invalid=0）
[CHECK] 快照内样本 seq 严格递增（无错位/回退）
[CHECK] 共享区只读映射生效（写入触发 SIGSEGV）
[CHECK] 超一页的映射被拒绝（errno=EINVAL=22）
[CHECK] 测量前已把采样周期拉大到 2s（保证该项有判别力）
[CHECK] 队列有数据时 read 立即返回（周期 2000ms 下实测 0ms < 100ms）
[CHECK] 新开的 fd 立刻读到已缓冲样本（队列语义）
[CHECK] 8 进程 × 200 轮无失败子进程（failed=0）
[CHECK] 并发下无非法样本（invalid_samples=0）
[CHECK] 并发下无非预期 errno（io_errors=0）
[CHECK] 并发下无 I2C 传输错误（i2c_errors=0）
[CHECK] dmesg 无 WARNING/Call trace（取样覆盖整段测试）
[TEST:END] 03-ringbuffer-mmap pass=28 fail=0
```

**关键实测值**（`logs/20260914-111043-03-ringbuffer-mmap.log`）：

| 指标 | 实测 | 说明 |
|---|---|---|
| `irq_count` | 299 | ≈ 10ms 周期 × 3 秒，样本全部产生 |
| `ring_count` | 85 | 队列被填满 = 驱动打印的实际容量 |
| `kfifo_dropped` | 214 | **85 + 214 = 299**：每个样本要么在队列、要么被计数丢弃，无静默丢失 |
| `drained_samples / seq_span` | 10 / 9 | 连读 10 次拿到 10 个不同样本，序号跨度为 9 → **历史样本被保留**（旧实现会重复返回同一条） |
| `mmap_reads / retries / invalid` | 100 / 0 / 0 | 100 轮一致性读无撕裂样本 |
| `mmap_ro_sigsegv` | 1 | 只读映射写入触发 SIGSEGV |
| `mmap_oversize_errno` | 22 (EINVAL) | 超一页映射被拒绝 |
| `buffered_read_interval_ms` | 2000 | 测量前把周期拉大到 2s，使该项有 3 个数量级的判别力 |
| `buffered_read_ms` | 0 | 周期 2s 下 read 仍立即返回（若实现是"等下一个样本"，这里会是 ~2000ms） |
| 并发 | `samples_read=41 eagain=1559 invalid_samples=0 snapshots=1600 i2c_errors=0 failed=0` | 1600 次快照全部有效 |

**回归**：`01-io-models` 16 PASS / `smoke` 15 PASS（日志见文件头）。

**稳定性**：03 阶段测试连续复跑 3 次均为 28 PASS / 0 FAIL（无抖动），
日志 `logs/20260914-111258/111305/111312-03-ringbuffer-mmap.log`。
这一项很重要：带时序断言的测试如果偶发失败，等于没有保护作用。

---

## 八、遗留风险与已知限制

1. **并发正确性只做到"高置信度"，不是证明**：见 4.3。下一步可开 KCSAN + lockdep
   （需要改内核 `.config` 并重建，约 10 分钟）来把"可能的数据竞争"变成"检测器直接报错"。
2. **seqlock 的重试路径在实测中没有被触发**（`retries=0`，概率约 5×10⁻⁵/次）。
   需要用负控实验证明其有效性（去掉读侧屏障/重试后测试必须失败），
   这一点已写入给验证者的建议。
3. **mmap 共享区容量固定 32 个样本**（`reserved[3]` 预留了扩展位）。
   用户态若读取慢于 32 个周期，只能看到最近的 32 个；更长的历史要靠 `read()` 的 kfifo。
4. **`read()` 与 `mmap` 是两条独立的数据通道**：`read` 出队会消耗 kfifo，
   而 mmap 窗口不受影响（内核始终保留最近 32 个）。这不是 bug，但用户态需要知道
   "读走的样本不会从共享区消失"。已写进 `struct sensor_shm` 的注释。
5. **`buffered_read_ms < 100` 的阈值**依赖采样周期（测试里 10ms）。
   如果将来把周期改到比 100ms 更大，这条断言的判别力会下降 —— 但那时
   "立即返回"与"等一个周期"的差异反而更大，阈值仍然成立。
