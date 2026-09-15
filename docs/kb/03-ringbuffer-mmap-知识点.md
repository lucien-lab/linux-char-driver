# 阶段 03 知识点：kfifo 环形缓冲 + mmap 零拷贝 + seqlock + 并发正确性

> 面向"嵌入式 / Linux 驱动岗面试与被追问"。本文所有内核源码引用格式为 `文件:行号`，
> 内核基线为 `linux-6.6.156`（源码树 `/Users/lucien/workspace/self-study/projects/linux-char-driver/kernel-src/linux-6.6.156`）。
> 本项目实测值全部来自仓库 `logs/` 目录下的 QEMU 串口日志，文件名在正文与第 7 节注明。
> **本工作树（wt-03v）里 `logs/` 目录未落盘**（`.gitignore` 忽略 `logs/*.log`），
> 实测值抄自主检出 `/Users/lucien/workspace/self-study/projects/linux-char-driver/logs/` 下的同名日志文件。
> 凡未亲自运行/未在日志中找到证据的结论，一律标「未验证」。

---

## 0. 本阶段的坐标系（先建立问题，再讲机制）

阶段 02 结束时的数据通路是"**单样本覆盖**"：

```
线程化中断下半部 → sd->latest（只有一个样本） → read()/ioctl()
```

只要读方节奏慢于采样，中间样本被**静默覆盖**，用户态无法知道丢了多少。
阶段 03 把它改成"一个生产者、三个出口"：

```
                          ┌── kfifo 环形缓冲（实际 85 个样本）─► read()/poll/epoll/SIGIO
传感器 --I2C--> 中断下半部 ─┼── mmap 共享页（用户可见 seqlock）──► 用户态零拷贝直读
   （唯一生产者）           └── latest（最新一个）───────────────► ioctl(GET_SAMPLE)
```

三个出口对应三类需求，**必须能讲清"为什么不是只留一个"**：

| 出口 | 语义 | 适用 |
|---|---|---|
| `read/poll/epoll/SIGIO` | 有队列、有阻塞/唤醒语义、VFS 标准接口 | 常规流式读取、sleep 等数据 |
| `mmap` 共享页 | 零系统调用、零拷贝，但接口是无锁共享内存 | 高频轮询最新值、低延迟采集 |
| `latest` + `ioctl(GET_SAMPLE)` | 立刻拿到"当前值"，不排队 | 控制回路要当前值、不要历史 |

代码落点（本项目）：
- 生产者：`driver/sensor_char.c:336` `sensor_irq_thread()` → `:388` kfifo 入队 → `:393` `sensor_shm_publish()`；
- 消费者：`driver/sensor_char.c:545` `sensor_read()` 出队；`driver/sensor_char.c:663` `sensor_mmap()`。

---

## 1. ring buffer 选型

### 1.1 为什么速率失配必须用缓冲

生产速率由硬件/中断驱动（本项目 10ms 周期），消费速率由调度、用户态处理时间决定，
两者**没有任何同步关系**。没有缓冲时，状态只能存"一个最新值"，任何消费慢于生产的时刻都会丢数据，
且丢失量不可观测。缓冲把"生产者与消费者直接耦合"改成"通过队列解耦"：

- 生产者只在队满时受惩罚（丢新或覆盖旧），不必等待消费者；
- 消费者可以事后补读队列里的历史；
- "丢了多少"可以变成一个计数器（本项目的 `kfifo_dropped`），从不可观测变成可观测量。

这是阻塞队列（`waitqueue`）与环形缓冲的分工：**缓冲负责容纳速率差，waitqueue 负责在队空/队满时改变调度状态**。

### 1.2 kfifo 的数据结构与满/空判定

kfifo 的内部结构（`include/linux/kfifo.h:40`）：

```c
struct __kfifo {
	unsigned int	in;	/* 写入位置（单调递增，不对 mask 取模！） */
	unsigned int	out;	/* 读出位置（单调递增） */
	unsigned int	mask;	/* size - 1，size 必须是 2 的幂 */
	unsigned int	esize;	/* 元素大小 */
	void		*data;
};
```

满/空判定完全靠两个**单调递增**计数器之差，不做加减模运算（这是 kfifo 的关键设计）：

- 长度：`#define kfifo_len(fifo) ((fifo)->kfifo.in - (fifo)->kfifo.out)`（`include/linux/kfifo.h:233`）；
- 空：`kfifo_len == 0`（`include/linux/kfifo.h:243`）；
- 满：`kfifo_len > mask`（`include/linux/kfifo.h:287`，即 `len >= size`）；
- 槽位下标：`in & mask` / `out & mask`。

因为 `in`/`out` 是无符号整型，自然回绕（wrap）后 `in - out` 的结果仍然正确，
所以"满"与"空"不会混淆——这正好解决了 `head == tail` 既是空又是满的经典歧义。
对比 `circ_buf` 用 `size - 1` 个槽位来回避该歧义（见 1.4）。

**容量向上取整到 2 的幂**：`lib/kfifo.c:24` `__kfifo_alloc()` 在 `lib/kfifo.c:31` 调用
`size = roundup_pow_of_two(size)`。本项目请求 `64 × sizeof(struct sensor_sample)(24B) = 1536B`，
向上取整到 2048B → `mask+1 = 2048` → 实际能放 `2048/24 = 85` 个样本。
驱动在 probe 时把真实容量打印出来（`driver/sensor_char.c:999` 的 `dev_info`），
并通过 `sensor_ring_capacity()`（`driver/sensor_char.c:471`）用 `kfifo_size()/sizeof(sample)` 换算。
实测日志：

```
[    0.xxx] sensor_char 0-0048: ring ready: capacity=85 samples (24 bytes each)
（日志 logs/20260914-133755-main-03-ringbuffer-mmap.log）
```

**面试陷阱**：`kfifo_len()` 返回的是**字节数**，不是元素个数。用户态统计若直接用它，
量纲会错（85 vs 2048）。本项目 `GET_STATS` 返回的 `ring_count`/`ring_capacity`
都是"样本数"（`driver/sensor_ioctl.h` 注释与 `driver/sensor_char.c:478/471`）。

### 1.3 `kfifo_in_spinlocked/out_spinlocked` 与免锁版的区别、内存序

内核文档说得很直白（`include/linux/kfifo.h:29-33`）：

> There is no locking required until only one reader and one writer is using the fifo
> and no kfifo_reset() will be called. ...
> For multiple writer and one reader there is only a need to lock the writer.
> And vice versa for only one writer and multiple reader there is only a need to lock the reader.

也就是说：**免锁性质只覆盖"单读者 + 单写者"**。`kfifo_put/kfifo_get`
（`include/linux/kfifo.h:390` / `:437`）、`kfifo_in/kfifo_out`（`:519` / `:587`）
都是免锁版本，它们只保证"数据先于索引可见"的内存序，不保证索引读改写的原子性。

内存序由谁提供？看无锁路径的源码：

- 写路径 `kfifo_copy_in()`：`memcpy` 完数据后 `smp_wmb()`（`lib/kfifo.c:110`），
  再 `fifo->in += len`（`lib/kfifo.c:125`）。这是"**数据在索引之前可见**"的标准 release 语义。
  `kfifo_put` 宏里同样是"写槽位 → `smp_wmb()` → `in++`"（`include/linux/kfifo.h:423`）。
- 读路径 `kfifo_copy_out()`：`memcpy` 出数据后 `smp_wmb()`（`lib/kfifo.c:149`），
  再 `fifo->out += len`（`__kfifo_out`，`lib/kfifo.c:166-172`）。`kfifo_get` 宏同理（`:461`）。

为什么读侧也是 `smp_wmb()` 而不是 `smp_rmb()`？因为读侧的 `out++` 也是**写**，
必须保证"已经把数据读出来"发生在"推进 out 索引"之前，否则生产者可能认为该槽位已空、
写进新数据，覆盖读者尚未拷走的内容。

`kfifo_in_spinlocked/out_spinlocked`（`include/linux/kfifo.h:541` / `:611`）
是**加锁 + 关闭中断**的包装：

```c
#define kfifo_in_spinlocked(fifo, buf, n, lock) \
({ \
	unsigned long __flags; \
	unsigned int __ret; \
	spin_lock_irqsave(lock, __flags); \
	__ret = kfifo_in(fifo, buf, n); \
	spin_unlock_irqrestore(lock, __flags); \
	__ret; \
})
```

**为什么本项目必须用加锁版**：设备允许多个进程各自 `open()` 后同时 `read()`
（并发测试就是 8 个进程同时读），即"单生产者 + 多消费者"。
多消费者会并发执行 `out++`（读改写非原子），互相覆盖 → 同一样本被读两次或槽位错乱。
所以生产与消费统一走 `kfifo_in_spinlocked`/`kfifo_out_spinlocked`，共用 `sd->ring_lock`
（`driver/sensor_char.c:388` / `:555`）。
反之，如果**确定**只有一个读者，可以只用 `kfifo_out`（免锁）——但那是把正确性押在调用者纪律上。

还有一点：`kfifo_reset()` 会同时改写 `in` 和 `out`，**任何情况下都必须独占**
（`include/linux/kfifo.h:205-209` 的 Note 明确警告）。本项目 `SENSOR_IOC_RESET` 里用
`spin_lock_irqsave(&sd->ring_lock, ...)` 包住 `kfifo_reset()`（`driver/sensor_char.c:805-807`）。

### 1.4 `circ_buf` 与 kfifo 的关系

`circ_buf`（`include/linux/circ_buf.h:9`）是最朴素的环形缓冲，只有两个计数器：

```c
struct circ_buf {
	char *buf;
	int head;
	int tail;
};
```

配套 `CIRC_CNT/CIRC_SPACE`（`include/linux/circ_buf.h:16/21`）：

```c
#define CIRC_CNT(head, tail, size)    (((head) - (tail)) & ((size) - 1))
#define CIRC_SPACE(head, tail, size)  CIRC_CNT((tail), ((head) + 1), (size))
```

注意 `CIRC_SPACE` 用 `(head)+1` 来规避"`head == tail` 既是空又是满"的歧义，
**代价是永远浪费一个槽位**。`circ_buf` 不提供任何原子性/内存序保证，
使用者必须自己处理并发（通常仍是单生产者单消费者 + 屏障）。

| | `circ_buf` | `kfifo` |
|---|---|---|
| 容量 | 任意 size，浪费 1 槽 | 必须是 2 的幂，无浪费 |
| 满/空 | `CIRC_CNT/SPACE` 宏 | `in - out` 与 `mask` 比较 |
| 内存序 | 无，需自己加屏障 | 内置 `smp_wmb()` |
| 加锁封装 | 无 | `kfifo_*_spinlocked` |
| 典型用途 | 串口 `tty` 环形缓冲、自造协议 | 通用内核 FIFO、驱动数据通路 |

面试常见追问"为什么不自己写一个环？"回答要点：`circ_buf` 适合单生产者单消费者且你完全掌控屏障；
只要出现多读者、跨越中断上下文、需要统计丢弃，`kfifo` 的内置内存序 + 加锁封装能避免一大类错误。

### 1.5 DMA 环形缓冲的关系

DMA 环形缓冲（典型是 `dmaengine` 的 cyclic transfer，以及网卡的 RX/TX ring）
与 kfifo 是**不同层次**的东西：

- kfifo/circ_buf 解决的是 **CPU 视角**的数据排队（生产者与消费者都是 CPU 上下文的软件路径）；
- DMA ring 的"生产者"是**设备**，它按自己的节奏把数据写进内存，通过 **descriptor ring + 完成中断/状态位**通知 CPU。
  并发控制靠的不是自旋锁，而是"**所有权（ownership）交接**"：descriptor 的 `OWN` 位或
  生产者/消费者指针交接，加上 `dma_wmb()/dma_rmb()`（保证 descriptor 内容先于状态位可见）。

共同点：都是"环形 + 索引/指针单调推进 + 满则丢或阻塞"。
不同点：kfifo 的同步原语是 `smp_wmb` + 自旋锁，DMA ring 是 cache 一致性协议 + DMA 屏障 + 中断。

因此面试若问"你的 kfifo 能直接拿来做 DMA 环形缓冲吗？"——**不能直接套**：
真实 DMA 缓冲要求内存在设备可见的 DMA 域（`dma_alloc_coherent` 或 `dma_map_single` + `dma_sync_*`），
见 2.5。本项目"设备"是 `virt_i2c.ko` 模拟的软件芯片，`regmap` 读走 I2C 路径，
不涉及真实 DMA。

### 1.6 三种溢出策略与本项目选择

| 策略 | 行为 | 适用场景 | 代价 |
|---|---|---|---|
| **丢新（丢刚产生的）** | 队满时丢弃新样本，计数 | 历史比最新更重要：日志采集、审计、需要事后回溯；且"丢了多少"必须可观测 | 最新值可能进不了队列（但本项目另有 `latest`/`mmap` 兜底） |
| **覆盖旧（丢最旧的）** | 队满时挤掉队头再写新样本 | "永远保留最近 N 个"的示波器/滑动窗口型采集 | 静默改写历史；"丢了多少"不可知（除非额外维护计数，而覆盖本身已丢失信息） |
| **阻塞生产者** | 队满时让生产者等待 | 生产速率可控、且**不允许丢任何数据**的可靠性场景（如协议栈） | 生产者在中断/中断线程上下文时不可用；把读方的慢反馈给采样节奏 |

本项目的选择是**丢新 + 计数**（`driver/sensor_char.c:388-390`）：

```c
if (kfifo_in_spinlocked(&sd->ring, &sample, sizeof(sample),
			&sd->ring_lock) != sizeof(sample))
	sd->kfifo_dropped++;
```

理由（能背下来）：

1. 生产者运行在**线程化中断下半部**，绝不能因队列满而阻塞（阻塞会同时拖慢采样与 I2C 事务）；
2. 已排队的数据是"用户态还欠着没读的历史"，静默改写它比丢新更难排查；
3. 计数让"读方跟不上采样"变成**可观测量**：`kfifo_dropped` 持续增长就是告警信号；
4. 最新值这条路径不受影响——即使队满，`latest` 与 mmap 共享区仍会更新，
   `ioctl(GET_SAMPLE)` 永远能拿到最新样本。

**丢新的代价必须说清楚**：对"需要每一个样本"的应用（例如振动 FFT），丢新会造成频谱混叠归因困难，
此时应改覆盖旧或加大容量并降低采样率；本项目通过 `kfifo_dropped` 让这个代价可见而不是隐藏。

**负控证据（策略选择不是空话）**：
验证者把策略改成"覆盖最旧"后，`dropped` 恒为 0，`dropped ≥ 1` 检查立刻 FAIL
（`logs/20260914-111727-03-ringbuffer-mmap.log`，25 PASS / 3 FAIL）。
说明该检查项确实能区分丢弃策略。

---

## 2. mmap 零拷贝

### 2.1 `read()` 拷贝与 `mmap` 映射的成本对比

| | `read()` | `mmap()` |
|---|---|---|
| 每次取值 | 一次系统调用 + 一次 `copy_to_user` | **零系统调用、零拷贝**，直接读页表里的同一块物理内存 |
| 内核侧动作 | 出队 + 格式化（本项目格式化成一行文本）+ 拷贝 | 无（数据由生产者直接写进共享页） |
| 用户侧动作 | 解析返回的文本 | 按固定布局解析结构体 + 处理 seqlock 重试 |
| 拷贝次数 | 数据从内核缓冲 → 用户缓冲 1 次 | 0 次 |
| 页表 | 每次 syscall 的常规地址翻译 | 首次缺页/建映射后，用户页表项直接指向内核分配物理页 |

页表层面发生了什么（能讲清是关键）：

1. `mmap()` 时 VFS 调驱动 `.mmap`，驱动调 `remap_pfn_range()`：
   内核把**指定物理页帧号（PFN）**写进用户进程页表的对应 PTE，并设置权限位。
   之后用户态访问该地址时，MMU 直接命中这条 PTE，**不再进内核、不再拷贝**。
2. `read()` 则每次都要 `copy_to_user`：内核态地址 → 用户态地址，走 `uaccess` 路径，
   在弱内存序架构上是真正的内存搬运（可能还有 cache 影响）。
3. `remap_pfn_range` 的 PTE 是**预先建立**的（非 demand paging）：本项目不实现 `.fault`，
   所以映射建立后立即有效，不会在首次访问时再进内核。

**实测（本项目）**：把采样周期拉到 2s 后测量，`buffered_read_ms=0`
（`logs/20260914-133755-main-03-ringbuffer-mmap.log`）。为什么要先拉大周期：
若周期仍是 10ms，"等下一个样本"的实现最多只等 10ms，用 100ms 阈值区分不出两种实现；
2s 周期下"从队列取"（~0ms）与"等下一个样本"（~2000ms）相差三个数量级，断言才有判别力。

**mmap 的代价**（面试必问"零拷贝是不是永远更好"）：接口复杂度被推到用户态——
用户态必须自己解析固定布局、自己处理 seqlock 重试、自己定义"读到一半的窗口怎么办"。
`read()` 的语义由内核保证（要么拿到完整一行，要么出错），mmap 的语义由**协议**保证。
所以低频场景用 `read`，高频轮询用 `mmap`。

### 2.2 `__get_free_pages` + `remap_pfn_range` 的正确用法与限制

本项目分配与映射（`driver/sensor_char.c:1007`、`:663-702`）：

```c
/* probe：一页，__GFP_ZERO 保证第一个样本到达前用户态读到的是"空状态"而不是随机数据 */
sd->shm_addr = __get_free_pages(GFP_KERNEL | __GFP_ZERO, 0);
sd->shm = (struct sensor_shm *)sd->shm_addr;
sd->shm->magic = SENSOR_SHM_MAGIC;
sd->shm->version = SENSOR_SHM_VERSION;
```

```c
static int sensor_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > PAGE_SIZE)           /* :668 只分配了一页，多映射会读到无关内存 */
		return -EINVAL;
	if (vma->vm_pgoff)              /* :676 remap_pfn_range 不看 pgoff，不检查会静默映射同一页 */
		return -EINVAL;
	if (vma->vm_flags & VM_WRITE)   /* :679 内核单方写入，用户态写会破坏一致性假设 */
		return -EPERM;

	vma->vm_page_prot = PAGE_READONLY;                 /* :687 显式只读 */
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);    /* :693 6.3+ 必须用 helper */

	return remap_pfn_range(vma, vma->vm_start, virt_to_pfn(sd->shm),
			       size, vma->vm_page_prot);        /* :701 */
}
```

**四条限制，必须逐条讲得出原因**：

1. **内存必须来自"物理页与虚拟地址一对一"的区域，即线性映射区（linear map / lowmem）**。
   `__get_free_pages`/`kmalloc` 属于该区域，`virt_to_pfn()` 可直接换算成 PFN
   （内核里 `virt_to_pfn` 就是 `__pa(x) >> PAGE_SHIFT`）。
2. **`remap_pfn_range` 只接受 PFN，不做任何页表逐页映射的语义检查**，
   所以绝不能用它映射"非连续物理页"的虚拟地址块。
3. **必须自己做边界与权限校验**：映射长度、偏移、写权限（上面前三个 `if`）。
   少任何一个都会产生"调用者以为成功、实际语义不对"的静默错误。
4. **安全边界**：映射出去的内存，内核无法再单方面保证不被窥探/篡改，
   所以只映射"该给用户看的数据"。本项目只映射 `struct sensor_shm` 这一页，
   且用 `static_assert(sizeof(struct sensor_shm) <= PAGE_SIZE, ...)`
   （`driver/sensor_char.c:110`）在编译期钉死"放得进一页"。

**6.6 的 API 细节（本项目踩过的编译错误）**：Linux 6.3 起
`struct vm_area_struct` 里的 `vm_flags` 是 union 中的 `const vm_flags_t`，
直接 `vma->vm_flags |= VM_DONTDUMP;` 会 `error: assignment of read-only member 'vm_flags'`，
必须用 `vm_flags_set(vma, ...)`（`driver/sensor_char.c:693` 上方注释记录了这次编译错误）。

### 2.3 为什么 `vmalloc` 内存不能直接 `remap_pfn_range`

`vmalloc` 返回的是**虚拟地址连续、物理页不连续**的区域（页表另建，与 linear map 分离）。
对同一个虚拟地址区间，每 4KB 都可能是不同的物理页，所以：

- 不能用 `virt_to_pfn(vmalloc_addr)`——它算出的"物理地址"是伪造的，
  实际会把无关物理内存映射给用户态（**这是安全漏洞级别的错误**）；
- 正确做法一：`vmalloc_to_pfn()` **逐页**换算（`mm/vmalloc.c:757`），
  再对每一页单独调用 `remap_pfn_range`；
- 正确做法二：实现 `vm_ops->fault`，在缺页时用 `vm_insert_page()`
  （`mm/memory.c:2009`，配套 `vm_insert_pages` `:1963`）把某一个具体 `struct page` 插进用户页表。

本项目为什么用 `__get_free_pages` 而不是 `vmalloc`：只需要 1 页，
linear map 的 `virt_to_pfn` 一次换算即可，`remap_pfn_range` 一条调用搞定，
不需要实现 `.fault`。如果共享区需要很多页（> `MAX_ORDER`），才考虑 vmalloc + 逐页方案。

参考实现语义：`remap_pfn_range`（`mm/memory.c:2511`）内部走
`remap_pfn_range_notrack()`（`mm/memory.c:2482`），按 PFN 连续递增建立 PTE。

### 2.4 `PAGE_READONLY` 语义

`PAGE_READONLY` 是内核的**页保护模板**（`pgprot_t`），不是用户态的 `PROT_READ` 宏，
把它赋给 `vma->vm_page_prot` 后，建立的 PTE 不含可写位。要点：

1. **为什么要显式覆盖**：`vma->vm_page_prot` 默认由调用者的 `PROT_*` 推导而来。
   若沿用默认值，内核侧的"只读"语义就依赖调用方参数，是脆弱的；
   显式设成 `PAGE_READONLY` 后，只读语义由驱动保证。
2. **为什么还要 `VM_WRITE` 检查**：`PAGE_READONLY` 决定 PTE 权限，
   而 `VM_WRITE` 是 VMA 层面的标志。两者都设成"不可写"才是双重保险：
   即使某个路径改了 `vm_page_prot`，`VM_WRITE` 检查也会在 `.mmap` 入口直接返回 `-EPERM`。
3. **实测效果**：往映射区写触发 SIGSEGV，`mmap_ro_sigsegv=1`
   （`user/ring_mmap_test.c` 用 `sigsetjmp/siglongjmp` 捕获，日志见第 7 节）。
4. 补充：真机若要映射设备寄存器（MMIO），不能用普通内存的 `remap_pfn_range` 语义，
   通常要 `ioremap` + `pgprot_noncached()/pgprot_writecombine()`（见 2.5）。

### 2.5 真机 DMA 缓冲的 cache 一致性与 `dma_sync_*`

本项目"设备"是 `virt_i2c.ko` 里的软件模块，数据经 I2C/`regmap` 回到 CPU，
**不涉及真实 DMA**，所以共享页用普通内存即可。但面试会追问"真机上这块内存要交给 DMA 怎么办"：

1. **cache 一致性**：非一致性（non-coherent）cache 架构上，设备直接写内存后，
   CPU 可能从 cache 里读到旧值；CPU 写完 buffers、DMA 可能读到还在 cache 里的旧内容。
   解决方式二选一：
   - 用 `dma_alloc_coherent()` 分配（内核保证 CPU 与设备视图一致）：
     适合长期共享的 descriptor ring / 控制块；
   - 用普通内存 + `dma_map_single()/dma_map_sg()` 建立 DMA 映射，
     再用 `dma_sync_single_for_cpu()` / `dma_sync_single_for_device()`
     （声明见 `include/linux/dma-mapping.h:120/122`）做**所有权交接时的同步**：
     设备写完 → `for_cpu` 让 CPU 看到新数据；CPU 准备 buffer → `for_device` 把脏 cache 推下去。
2. **所有权（ownership）原则**：一块 buffer 在任一时刻只能归 CPU 或设备一方所有，
   交接必须显式同步（`dma_sync_*` 或 descriptor 的 OWN 位 + `dma_wmb/dma_rmb`）。
   把它和 seqlock 类比：本质都是"**在交接点放屏障**"，只是设备侧的屏障是 DMA 屏障。
3. **映射属性**：寄存器空间要 `ioremap` + `pgprot_noncached()`；
   内存写合并缓冲可考虑 `pgprot_writecombine()`。用普通 cacheable 属性映射 MMIO
   会出现读写被 cache 合并/乱序的诡异现象。
4. 本项目**未在真机/真实 DMA 上验证**上述内容，本节属于机制说明，标「未验证」。

---

## 3. seqlock（本阶段核心）

### 3.1 `seq` 奇偶约定

内核 `seqlock_t` 的语义：**序号为偶数 = 无写者，数据一致；奇数 = 有写者在写，数据不可采信**。
见 `include/linux/seqlock.h:64-69` 的 `seqcount_t`（就一个 `unsigned sequence`）
与 `:860` 的 `seqlock_t`（`seqcount_t` + 保护写者的 `spinlock_t`）。

读侧入口 `__read_seqcount_begin()`（`include/linux/seqlock.h:325-331`）就是"自旋等待偶数"：

```c
#define __read_seqcount_begin(s)					\
({									\
	unsigned __seq;							\
									\
	while ((__seq = seqprop_sequence(s)) & 1)			\
		cpu_relax();						\
	...
	__seq;								\
})
```

本项目在**用户态可见的共享区**里复刻同一约定（`driver/sensor_ioctl.h:41` 注释）：

```c
__u32 seq;	/* seqlock 序号：偶数=一致，奇数=写入中 */
```

### 3.2 读写两侧的内存屏障要求

内核写侧（`include/linux/seqlock.h:466-492`）：

```c
static inline void do_raw_write_seqcount_begin(seqcount_t *s)
{
	s->sequence++;		/* 先置奇 */
	smp_wmb();		/* 再屏障：保证"已进入写临界区"先于后面的数据写可见 */
}

static inline void do_raw_write_seqcount_end(seqcount_t *s)
{
	smp_wmb();		/* 先屏障：保证数据写先于"退出写临界区"可见 */
	s->sequence++;		/* 再置偶 */
}
```

读侧（`do_read_seqcount_retry()`，`include/linux/seqlock.h:446-450`）：

```c
static inline int do_read_seqcount_retry(const seqcount_t *s, unsigned start)
{
	smp_rmb();						/* 先屏障 */
	return do___read_seqcount_retry(s, start);		/* 再比较序号 */
}
```

**为什么这四处屏障缺一不可**（这是面试高频追问点）：

| 位置 | 缺失的后果 |
|---|---|
| begin 的置奇 | 无意义（置奇本身必须最先做） |
| begin 的 `smp_wmb()` | 读者可能在看到"序号为奇"之前就把**新数据**读进来了，与旧序号混用 |
| end 的 `smp_wmb()` | 序号变偶可能**先于数据写**对读者可见 → 读者看到偶序号，却读到写了一半的数据 |
| 读侧 retry 的 `smp_rmb()` | 读数据可能被编译器/CPU 重排到"重读序号"之后 → 序号检查通过但数据是旧的 |

本项目驱动侧照抄这个顺序（`driver/sensor_char.c:298-330`）：

```c
spin_lock(&sd->shm_lock);                 /* 序列化写者，且禁止 RESET 与发布交错 */
seq = READ_ONCE(shm->seq) + 1;
WRITE_ONCE(shm->seq, seq);                /* → 奇数：写入中 */
smp_wmb();                                /* 与 raw_write_seqcount_begin() 一致 */
if (sd->shm_publish_delay_us)
	udelay(sd->shm_publish_delay_us); /* 测试放大器，默认 0，见 4.4 */
shm->samples[shm->write_idx] = *s;
shm->write_idx = (shm->write_idx + 1) % SENSOR_SHM_SAMPLES;
if (shm->count < SENSOR_SHM_SAMPLES)
	shm->count++;
smp_wmb();                                /* 与 raw_write_seqcount_end() 一致 */
WRITE_ONCE(shm->seq, seq + 1);            /* → 偶数：一致 */
spin_unlock(&sd->shm_lock);
```

注意源码里的实现细节：用 `READ_ONCE(shm->seq) + 1` 而不是 `shm->seq++`。
因为 `shm` 指向的内存会被用户态并发读，编译器可能把它当普通内存做优化；
`READ_ONCE/WRITE_ONCE` 保证"只访问一次、不做重排/合并"，这是给 KCSAN 与编译器看的。

**RESET 也必须走协议**：一次"清空共享区"也是一次写入，必须置奇 → 写 → 置偶
（`driver/sensor_char.c:823-831`）。不这么做会出现"count/write_idx 已清零、samples 还没覆盖"
的自相矛盾快照。

### 3.3 为什么 seqlock 适合"读多写少且可重试"

- 写者拿**自旋锁**（内核 `seqlock_t` 里是 `spinlock_t`），写者之间互斥；
- 读者**完全不加锁**，只读序号 + 读数据 + 校验序号，失败就重试；
- 读的代价与写者数量无关，且**读者之间无争用**（不需要任何原子操作或缓存行弹跳）。

所以 seqlock 的甜点区是：**写者少（独占）、读者多、写临界区短、读者可以重试**。
这正是本项目 mmap 共享区的场景：唯一写者（中断线程），多个用户态进程高频读。

**代价与前提**：
1. 读者可能无限重试（写者持续写时饿死），所以读临界区必须短；
2. 读侧读到的数据必须先校验再使用，**不能用它做副作用**（不能读一半就去写设备）；
3. 写者之间仍需锁；`raw_write_seqcount_begin` 在可抢占 seqcount 上还会
   `preempt_disable()`（`include/linux/seqlock.h:458-464`）；
4. 对**不可重试**的读路径（如不能失败的硬件寄存器读）不适用。

### 3.4 与 spinlock / mutex / rwlock / RCU 的取舍

| 机制 | 读者开销 | 写者开销 | 读者可重试 | 适用 | 本项目用在哪 |
|---|---|---|---|---|---|
| `spinlock_t` | 原子操作 + 关中断/自旋，不可睡眠 | 同上 | 否 | 临界区极短、中断上下文 | `ring_lock`（kfifo）、`shm_lock`（写者互斥） |
| `mutex` | 可睡眠，竞争时进等待队列 | 可睡眠 | 否 | 可能睡眠的长临界区（如 I2C 事务） | `sd->lock`（保护 latest/stats/I2C） |
| `rwlock`/`rwsem` | 读者间也争用同一缓存行/计数器，写者可能饿死 | 较重 | 否 | 读多写少但读侧可加锁 | 本项目未使用 |
| **seqlock** | **零原子操作**，只读 + 屏障 + 重试 | 自旋锁 + 屏障 | **是** | 读多写少、写临界区短、数据可重读 | mmap 共享区（`driver/sensor_char.c:298`） |
| RCU | 零开销读（`rcu_read_lock` 只需禁止抢占） | 拷贝-替换-延迟释放 | 否（但读期间对象不变） | 链表/指针型读多写少、生命周期可管理 | 本项目未使用 |

关键区分点（面试常被追问）：

- **seqlock vs rwlock**：rwlock 的读者也要写一个共享计数器（原子操作 + 缓存行争用），
  读多时这个缓存行就是瓶颈；seqlock 读者完全不写，只读序号，所以读侧扩展性更好。
  代价是读者可能重试、以及数据必须"可重读且无副作用"。
- **seqlock vs RCU**：RCU 保护的是**指针/对象生命周期**，读到的对象在 grace period 内稳定；
  seqlock 保护的是**同一块内存的内容**，内容会被原地更新，靠序号判断是否被写坏。
  本项目共享页是固定一块内存原地更新，天然是 seqlock 场景。
- **seqlock 不适合"读后有副作用"**：比如"读温度再决定给不给加热器上电"，
  重试会导致重复副作用。本项目用户态读到的只是快照，无副作用。

### 3.5 **"内核用 `seqlock_t` 但用户态看不到 seqcount"导致的假实现坑（本阶段最大教训）**

#### 3.5.1 现象

初版实现是这样的（错误写法）：

```c
/* 驱动结构体 */
seqlock_t shm_lock;      /* 用内核的 seqlock_t */

/* 写侧 */
write_seqlock(&sd->shm_lock);
/* 更新 shm->samples[...] / count / write_idx */
write_sequnlock(&sd->shm_lock);
```

共享区结构里**声明了 `__u32 seq;`，注释写着"偶数=一致，奇数=写入中"，
但内核从来没有给这个字段赋过值**（`grep 'shm->seq'` 无任何写操作）。

#### 3.5.2 根因

`seqlock_t` 的序号存在**它自己的 `seqcount_t` 里**（`include/linux/seqlock.h:64-69`），
也就是 `struct sensor_dev` 的**私有内核内存**里。用户态 `mmap` 到的是**那一页共享内存**，
**看不到 seqcount**。于是：

- 内核侧的 `write_seqlock()` 只保证"内核写者之间互斥"，对用户态读者**没有任何可观测信号**；
- 用户态"读 seq → 奇数重试 → 读数据 → 重读 seq → 变化重试"里的两处 `seq`
  永远是常量 0，重试条件是**死代码**；
- 数据看起来还是对的（写者临界区极短，用户态几乎不可能撞上），所以**测试不会失败**。

#### 3.5.3 为什么测试没抓到：两个负控实验的对照

验证者做了负控（`docs/verify/03-ringbuffer-mmap-验证报告.md` 第 4 节）：

| 负控 | 改动 | 期望 | 实测（旧实现） | 判定 |
|---|---|---|---|---|
| NC-A | 读侧 `s1=0; s2=0;`，重试条件永假 | 应 FAIL | **28 PASS / 0 FAIL** | 检查无效 |
| NC-B | 删掉写侧 `write_seqlock/write_sequnlock` | 应 FAIL | **28 PASS / 0 FAIL** | 检查无效 |

日志：`logs/20260914-111633-03-ringbuffer-mmap.log`（NC-A）、
`logs/20260914-111708-03-ringbuffer-mmap.log`（NC-B）。
**负控零判别力 = 检查项在骗自己**。这条教训比 seqlock 本身更重要：
任何一项检查都要问"什么样的错误实现会让它失败"，答不出来就说明它是恒真检查。

#### 3.5.4 正确写法

把 `seqlock_t` 换成普通 `spinlock_t`，**由内核亲手在共享区里维护用户可见的 `seq`**
（`driver/sensor_char.c:161`、`:298-330`）。共享区里的 `seq` 才是与用户态的**契约**；
`spinlock_t` 只负责写者互斥（本项目唯一写者是中断线程，其实用自旋锁也更简单直观）。

用户态按协议读（`user/ring_mmap_test.c:129` `shm_snapshot`）：

```c
s1 = shm->seq;                 /* 1. 先读序号 */
if (s1 & 1u) continue;         /*    奇数 = 写方正在更新 → 重试 */
__sync_synchronize();          /* 2. 全屏障（用户态没有 smp_rmb） */
... 抄 samples[]/count/write_idx ...
__sync_synchronize();          /* 3. 全屏障 */
s2 = shm->seq;
if (s1 != s2) continue;        /*    期间写过 → 重试 */
```

#### 3.5.5 怎么让"seqlock 生效"变成可观测的正向证据

自然条件下写窗口只有 ~1µs，采样周期 10ms，读者撞上的概率约 10⁻⁴——
**正向证据天然拿不到**。整改引入两件东西：

1. **确定性探针**：等待共享区 `seq` 发生变化（最多 500ms），断言 `shm_seq_advances=1`。
   这能直接抓住"内核不写 seq"的假实现（假实现下探针会等满超时）。
   实测 `shm_seq_probe_first=600 shm_seq_probe_last=602 waited_ms=5`
   （`logs/20260914-133755-main-03-ringbuffer-mmap.log`）。
   （注意：不要用"循环里 min/max"这种弱检查——100 轮一致性读只花几毫秒，
   跨不过一个 10ms 采样周期，会把"窗口太短"误判成"seq 没在动"，整改过程中真的这么误判过一次。）
2. **受控放大器**：模块参数 `shm_publish_delay_us`（`driver/sensor_char.c:205-207`），
   把写入窗口放大后测试重试路径。详见 4.4。

整改后的负控证据（反向对照 NC-1：删掉写入侧两处 `WRITE_ONCE(shm->seq, ...)`，
复现整改前的假 seqlock）：同一放大窗口下 **`mmap_invalid=24`、8 项检查 FAIL**
（`logs/20260914-123122-03-ringbuffer-mmap.log`，`pass=32 fail=8`，含
`重试后数据始终一致（放大窗口下 mmap_invalid=0） -- 期望='0' 实际='24'`）。
"好实现 invalid=0 / 坏实现 invalid>0，用的是同一套断言"——这才叫检查有判别力。

### 3.6 用户态读共享内存为什么同样需要"读-校验-重试"（没有 `smp_rmb` 时怎么办）

用户态没有 `smp_rmb()`/`smp_wmb()`，也没有 `READ_ONCE/WRITE_ONCE`。可用替代：

1. **`volatile`**：让编译器每次都从内存读（不缓存到寄存器），但
   **`volatile` 不提供内存序**——编译器仍可能对相邻访问重排，CPU（尤其 ARM 弱内存序）
   也可能乱序执行。所以 volatile 是必要条件，不是充分条件。
2. **屏障**：本项目用 GCC 内建 `__sync_synchronize()`（= 全屏障，
   等价于 `atomic_thread_fence(memory_order_seq_cst)`）。
   在 C11 下更精确的写法是 `atomic_thread_fence(memory_order_acquire)` 配合
   `atomic_load_explicit(&shm->seq, memory_order_relaxed)`。
   全屏障比 `smp_rmb` 强，代价是略慢，但用户态正确性优先。
3. **为什么读侧需要两次屏障**：
   - 第一处（读数据之前）防止"读数据被重排到读序号之前"：
     否则可能读到**新数据 + 旧序号**，序号校验却通过；
   - 第二处（重读序号之前）防止"重读序号被重排到读数据之前"：
     否则序号比较根本没覆盖数据读取的时间窗口。
4. **重试上限**：用户态必须设重试上限（本项目 `shm_snapshot` 有最大重试次数，
   超上限返回失败并计入 `mmap_invalid`），否则写者持续写时读者可能饿死。
   这是用户态版 seqlock 与内核版的一个差异：内核读者通常能接受自旋（写者很快结束），
   用户态进程被长期饿死则是**可用性事故**，必须有兜底。

**面试回答模板**："用户态没有内核的 `smp_rmb`，我用了 `__sync_synchronize()` 两次全屏障
加两次读 `seq` 比较；`volatile` 只能防编译器缓存，防不了 CPU 乱序，所以屏障必须显式写。
并且设了重试上限，避免写者持续写导致读者饿死。"

---

## 4. 并发正确性怎么证明

### 4.1 概率性测试的局限（必须先承认）

`8 进程 × 200 轮`跑通**不等于**数学上证明无竞态，它只能提高置信度。原因：

1. 测试覆盖的是**调度器恰好制造的交错**，而不是所有交错；
2. 竞态窗口通常只有几条指令，未命中不代表不存在；
3. 即使 100% 复现，也不能排除"目录里还有别的路径没被压到"。

**概率性测试的真实价值**是："**一旦实现有明显错误，就会高概率暴露**"。
本项目有两条硬证据支撑这个价值：

- 它**确实抓到了真实 bug**：`SENSOR_IOC_RESET` 没清共享区导致窗口内 `seq` 回退，
  被单调性校验在 1600 次快照中全部命中（`docs/impl/03-ringbuffer-mmap-实现记录.md` 五.2）；
- 负控 NC-C（改覆盖旧策略）立刻让 `dropped` 检查 FAIL
  （`logs/20260914-111727-03-ringbuffer-mmap.log`）。

### 4.2 单核（轮转 TCG）与多核（MTTCG）的区别

QEMU 的 `-accel tcg` 有两种线程模型：

- **单线程 TCG（默认/`thread=single`）**：无论 `-smp N` 配几个 vCPU，
  所有 vCPU 都在**同一个宿主线程**里**交替**执行。于是"两个 CPU 真正同时访问同一地址"
  这件事**结构上不可能发生**，SMP 竞态无法被触发。日志表现为
  `smp: Brought up 1 node, 1 CPU`（单核）或即使 2 CPU 也只是分时轮转。
- **MTTCG（`-accel tcg,thread=multi`）**：每个 vCPU 一个宿主线程，
  多个 vCPU 在宿主多核上**真正并行**执行，弱内存序架构（aarch64）的乱序效果才会体现。

本项目已统一为 MTTCG + 2 vCPU（`scripts/22-macos-run-test.sh:38/43/45`）：

```bash
-M virt -cpu cortex-a72 -accel tcg,thread=multi -smp 2 -m 1G -display none -serial stdio ...
```

实测日志里的证据：`[0.028778] smp: Brought up 1 node, 2 CPUs`
（`logs/20260914-133755-main-03-ringbuffer-mmap.log`）。

**本项目"单核时代"的对照证据**：早期 03 阶段的日志都是 1 CPU，例如
`logs/20260914-111852-03-ringbuffer-mmap.log`（`pass=28 fail=0`）、
`logs/20260914-111708` / `111811` / `111727` / `111633-03-ringbuffer-mmap.log`
里面全部是 `smp: Brought up 1 node, 1 CPU`。
当前仓库日志里 1 CPU 日志 108 份、2 CPU 日志 54 份——
**引用任何一条并发结论时必须先看日志里这一行**，否则会把单核结果当成多核证据。
整改后的关键实验（1 CPU → 2 CPU）与 L2 证据均落在 `...-133755-main-...`（2 CPUs）上；
负控 NC-1（`logs/20260914-123122-03-ringbuffer-mmap.log`）与 KCSAN strict
（`logs/20260914-130201-03-ringbuffer-mmap.log`）也都是 2 CPUs。

**"读到 300 次序号变化却从未撞上写窗口"的实测**：
放大器模式（窗口放大）下多数运行都能撞上窗口，但在**未放大／窗口较小时**，
确实出现过"读者观察到 300 次序号变化、却一次都没落在奇窗口"的运行：

```
[RING] shm_seq_min=66 shm_seq_max=666 shm_seq_odd_seen=0 shm_seq_changes=300
（logs/20260914-122253-03-ringbuffer-mmap.log；同现象亦见 122344 / 122357 / 122537 / 122655 等）
```

它说明两件事：
1. 即使 MTTCG 双核，**撞窗口仍是概率事件**（1µs 窗口 / 10ms 周期 ≈ 10⁻⁴），
   自然条件下拿不到正向证据——这直接推导出"需要受控放大器"（4.4）；
2. "读者与写者都在跑"（`changes=300`）与"读者落在窗口内"（`odd_seen=0`）是两件事，
   检查项必须**分开断言**，否则会把"环境没给机会"误判为"协议有问题"。

### 4.3 KCSAN / lockdep 能发现什么，本项目拿到什么

- **KCSAN（Kernel Concurrency Sanitizer，`CONFIG_KCSAN`）**：
  编译期给内存访问插桩，运行时记录 happens-before，发现"两个 CPU 无同步地访问同一地址"
  就打印 `BUG: KCSAN: data-race in ...`。它能把"可能的数据竞争"从概率猜测变成**检测器直接报错**。
  本项目用 `scripts/15-vm-build-kcsan.sh` 构建 KCSAN 内核（`-e EXPERT -e KCSAN -e KCSAN_VERBOSE`），
  启动横幅可见 `kcsan: enabled early` / `kcsan: selftest: 3/3 tests passed`。
  实测：
  - `logs/20260914-130201-03-ringbuffer-mmap.log`：**KCSAN strict 模式**内核（`#3`），
    03 阶段测试 `pass=40 fail=0`，日志里**没有**针对驱动代码的 `BUG: KCSAN`；
  - `logs/20260914-130220-01-io-models.log`：出现
    `BUG: KCSAN: data-race in folio_add_file_rmap_range / page_remove_rmap`——
    调用栈全部落在内核 `mm/rmap.c`（`filemap_map_pages` / `exit_mmap`），
    **与驱动无关**，是内核自身在使用 KCSAN 内核时的已知噪声级报告。
  - `logs/20260914-125014-03-ringbuffer-mmap.log`（KCSAN non-strict）：
    03 测试 `pass=38 fail=2`，失败项是用户态计数器溢出
    （`mmap_retries=-712206102`，`int` 溢出成负数），**不是 KCSAN 报告**。
- **lockdep（`CONFIG_PROVE_LOCKING`）**：检测**锁用法**错误——
  递归加锁、锁顺序反转、在禁止睡眠的上下文睡眠等。
  本项目日志中**未出现** lockdep 报告或初始化横幅（`grep 'lockdep'` 无命中），
  是否开启**未验证**。
  对照价值：本阶段踩过的"中断下半部重复 `mutex_lock(&sd->lock)` 导致死锁"的 bug，
  若开 lockdep 会在第一次触发时直接报 `possible recursive locking detected`，
  而不是等系统卡死（`sched: RT throttling activated`）。
- 其他可选项：`CONFIG_DEBUG_KMEMLEAK`（泄漏）、`CONFIG_SLUB_DEBUG`（越界/UAF）。

**关于"良性数据竞争"**：`sensor_has_new_sample()`（`driver/sensor_char.c:462`）
无锁读 `kfifo_is_empty()` 是**有意为之**的良性竞争，注释已写明：
`in/out` 是整字读取，瞬时不一致只会导致多睡/少睡一次，不影响数据正确性。
代价是 KCSAN 会对这类访问报 data race，属于**预期噪声**——但必须在代码注释与文档里写清楚，
否则审查者无法区分"良性"与"漏锁"。

### 4.4 "受控放大写窗口"：原理与代价

**原理**：给写者的临界区加一个可控延时（本项目 `shm_publish_delay_us`，
`driver/sensor_char.c:316-317` 的 `udelay()`），把"奇数窗口"从 ~1µs 放大到 1ms~20ms。
于是读者撞窗口的概率从 10⁻⁴ 提升到可观测量级，能同时观测：

- `mmap_retries > 0`：证明重试路径**真的被执行**（不是死代码）；
- `mmap_invalid == 0`：证明协议**真的有效**（撞上窗口也不撕裂）。

两者配合才能把"假 seqlock"与"真 seqlock"区分开。实测（当前脚本参数 `=1000µs`，
`tests/phases/03-ringbuffer-mmap.sh` 的 insmod 行；窗口 1ms / 周期 10ms）：

| 观测项 | 正常模式 | 放大器模式（1000µs，2s 紧凑循环） |
|---|---|---|
| 快照次数 `mmap_reads` | 100 | 5,015,075 |
| 撞上奇窗口 `shm_seq_odd_seen` | 0 | 147 |
| 重试次数 `mmap_retries` | 0 | 127,494,549 |
| 撕裂样本 `mmap_invalid` | 0 | **0** |
| `seq` 变化次数 `shm_seq_changes` | 0 | 347 |

（`logs/20260914-133755-main-03-ringbuffer-mmap.log`；`smp: ... 2 CPUs`）

用更大的 20ms 窗口、15s 观测时（早期参数版本）：`odd_seen≈497~509`、
`mmap_retries≈6.3×10⁸~7.3×10⁸`、`mmap_invalid=0`
（`logs/20260914-123040-03-ringbuffer-mmap.log`、`124411`、`125014`）。

**代价（面试追问"这样测准不准"要能答）**：

1. **改变被测系统行为**：延时占满写者临界区，采样线程被拖慢、定时器被推迟，
   样本合并/丢弃数变化；放大模式下测出的 `dropped`/时序**不能当作性能数据**。
2. **`udelay` 是忙等**：占用 CPU 且不能睡眠；在 MTTCG 下两个 vCPU 可能互相挤压，
   使"窗口占空比"不再等于参数/周期的简单比例（实测 147/347 ≈ 42%，而理论占空比 10%）。
3. **只放大"协议窗口"，不放大锁竞争**：它验证的是 seqlock 协议本身，
   不是"多写者/锁竞争"路径——本项目唯一写者是中断线程，本来就没有多写者竞争。
4. **必须默认关闭**：参数默认 0（`driver/sensor_char.c:205` `module_param(..., 0444)`），
   只有测试脚本 insmod 时显式打开，且实验后必须重新以默认参数 insmod 恢复
   （`tests/phases/03-ringbuffer-mmap.sh` 末尾的 `rmmod/insmod` + `check_exists /dev/sensor0`）。

---

## 5. 证据等级如何决定表述强度

不同证据能支撑的结论强度完全不同。本项目采用的等级划分：

| 等级 | 证据形态 | 能支撑的表述 | 不能支撑 |
|---|---|---|---|
| L0 冒烟级回归 | 单核/单进程跑通、无 panic/WARNING | "功能可用" | 任何并发结论 |
| L1 多核功能回归 | MTTCG + 2 vCPU，`-smp 2`，功能全绿 | "在多核环境下功能正确" | "无竞态" |
| L2 多核 + 全局唯一性断言 | 多进程消费样本 `seq` 全局无重复/无丢失（`dup_seqs=0`） | "出队路径无可见重复/丢样本" | 完全排除微小窗口竞态 |
| L3 KCSAN / lockdep 报告 | 检测器无针对本驱动的报告 | "检测器未发现数据竞争" | "数学上无竞态" |

**表述纪律（集成负责人在 `docs/11-阶段任务书.md` 阶段 03 的裁定，第 3 条）**：
> 只有「多核 + 全局唯一性断言（+ 可选的 KCSAN）」都拿到证据，
> 才允许在文档/简历里写「无竞态」；否则必须写成「冒烟级并发回归」，并说明局限。

**本项目最终拿到的是哪一档（据实描述）**：

- **L2 已达成**：MTTCG + 2 vCPU 实测（`smp: Brought up 1 node, 2 CPUs`），
  出队全局唯一性断言 `consumed_seqs=41 dup_seqs=0 seq_gaps=0 seq_overflow=0`
  （`logs/20260914-133755-main-03-ringbuffer-mmap.log`，
  检查项 `[CHECK:PASS] 出队全局无重复（dup_seqs=0...）`）；
- **L3 部分达成**：在 KCSAN 内核（strict 模式）下 03 阶段 `pass=40 fail=0`，
  **没有**针对驱动代码的 KCSAN 报告（`logs/20260914-130201-03-ringbuffer-mmap.log`）；
  但同批运行的 01 阶段日志里出现过一条内核 `mm/rmap.c` 的 KCSAN 报告，
  与本驱动无关（`logs/20260914-130220-01-io-models.log`）。lockdep 未验证。
- 因此按裁定口径，本项目在**多核 + 全局唯一性断言**这一档可以写"无竞态"，
  但同时必须附上两条诚实限制：
  1. 测试是概率性的，覆盖的是调度器实际制造的交错，不是全部交错；
  2. KCSAN 仅在 strict 模式的一次 03 运行中未报告驱动相关竞争，未做长时间/多轮扫描。

**各检查项对应的证据等级**（照抄需求里点的三类）：

- 冒烟级回归：`sensor_test` 退出码 0、`stats` 新字段可见、`dmesg` 无 `WARNING/Call trace`
  （`tests/phases/03-ringbuffer-mmap.sh` 第 4/5 节）；
- 多核唯一性断言：`dup_seqs=0`、`consumed_seqs=41`、`seq_gaps=0`；
- KCSAN 报告：`logs/20260914-130201-*` 无驱动相关报告；`logs/20260914-130220-01-io-models.log`
  有一条内核 rmap 报告（不计入驱动结论）。

---

## 6. 面试问答（≥8）

**Q1：kfifo 是线程安全的吗？`kfifo_put` 和 `kfifo_in_spinlocked` 该用哪个？**

A：`kfifo` 只在**单读者 + 单写者**且不调用 `kfifo_reset()` 时免锁
（`include/linux/kfifo.h:29-33`）。它的免锁性来自内存序：
写侧 `memcpy` → `smp_wmb()` → `in++`（`lib/kfifo.c:110`、`:125`；宏版 `kfifo_put` 见 `include/linux/kfifo.h:423`），
读侧 `memcpy` → `smp_wmb()` → `out++`（`lib/kfifo.c:149`、`:166`），
保证"数据先于索引可见"，但**索引读改写本身不是原子的**。
所以：单生产者单消费者用 `kfifo_put/kfifo_get` 即可；多读者或多写者必须用
`kfifo_in_spinlocked/kfifo_out_spinlocked`（`include/linux/kfifo.h:541/611`，内部 `spin_lock_irqsave`）。
本项目允许多进程同时 `read()`，所以必须用加锁版（`driver/sensor_char.c:388/555`）。
另外 `kfifo_reset()` 同时改写 `in/out`，任何情况都要独占（本项目用 `ring_lock` 包住，`:805-807`）。

**Q2：mmap 共享内存怎么分配才安全？为什么不能用 vmalloc？**

A：本项目用 `__get_free_pages(GFP_KERNEL | __GFP_ZERO, 0)` 拿一页
（`driver/sensor_char.c:1007`），它属于线性映射区，`virt_to_pfn()` 能直接得到物理页帧号，
`remap_pfn_range()` 才能把它填进用户页表。
如果换成 `vmalloc()`，虚拟连续但物理页不连续，`virt_to_pfn` 会算出伪造的物理地址，
把无关内存映射给用户态（安全漏洞）。正确做法是 `vmalloc_to_pfn()` 逐页换算
（`mm/vmalloc.c:757`）后逐页 `remap_pfn_range`，或者实现 `vm_ops->fault` +
`vm_insert_page()`（`mm/memory.c:2009`）在缺页时插入具体 `struct page`。
安全上还要做四件事：限制映射长度、拒绝非零 offset、
拒绝写映射（`VM_WRITE` → `-EPERM`）、显式 `PAGE_READONLY`，且只映射该给用户看的数据。

**Q3：seqlock 与读写锁（rwlock/rwsem）的区别？为什么读多写少时 seqlock 更好？**

A：rwlock 的读者也要对一个共享计数器做原子操作，读多时该缓存行成为争用点；
seqlock 的读者**完全不写**，只读序号 + 内存屏障 + 校验，失败重试，
读者之间零争用、可扩展性更好。代价：读者可能重试（写者持续写时可能饿死），
且读到的数据必须"可重读、无副作用"，不能读完就产生副作用。
本项目共享页是"固定一块内存原地更新、用户态只做快照"，正好满足这两个前提；
内核侧写者互斥用一个 `spinlock_t`（`driver/sensor_char.c:161/306`）。

**Q4：怎么证明你的驱动没有竞态？**

A：分三层说，且要主动承认局限：
①功能层——MTTCG 双核（`-accel tcg,thread=multi -smp 2`）下多进程压力测试全绿；
②**多核全局唯一性断言**——每个子进程把读到的样本 `seq` 写进共享数组，
父进程排序后查重复与缺口，本项目实测 `consumed_seqs=41 dup_seqs=0 seq_gaps=0`
（`logs/20260914-133755-main-03-ringbuffer-mmap.log`），
这是能直接抓"同一样本被读两次/出队索引被覆盖"的硬断言；
③工具层——KCSAN strict 内核下 03 阶段无驱动相关 data race 报告
（`logs/20260914-130201-03-ringbuffer-mmap.log`），lockdep 本项目未开（未验证）。
最后必须说：**概率性测试只提高置信度，不是证明**；单核 TCG 下 SMP 竞态结构上不可触发，
所以早期"28 PASS"的并发结论当时没有证据价值（早期 03 日志里是
`smp: Brought up 1 node, 1 CPU`，见 `logs/20260914-111852-03-ringbuffer-mmap.log`），
这也是本项目把 QEMU 统一改成 MTTCG 双核的原因。

**Q5：`read()` 和 `mmap()` 各自适用什么场景？零拷贝是不是永远更好？**

A：`read()` 每次一次系统调用 + 一次 `copy_to_user`，但语义由内核保证（阻塞/非阻塞/poll/SIGIO 都能用），
适合低频、需要标准 IO 模型、数据量小的场景。
`mmap()` 一次映射后取值零系统调用零拷贝，适合高频轮询最新值、低延迟采集。
代价是把复杂度推到用户态：必须自己处理布局解析和 seqlock 重试，
而且映射权限/长度/offset 都要驱动自己校验。
本项目实测：把周期拉到 2s 后 `buffered_read_ms=0`
（`logs/20260914-133755-main-03-ringbuffer-mmap.log`），证明 `read` 是"从队列取"而非"等下一个样本"；
两者并存不是冗余：`read` 走 kfifo 有历史队列，`mmap` 走共享页只看最近 32 个，语义不同。

**Q6：怎么检测丢样本？**

A：两条互补手段：
①**丢弃计数**：队满时丢弃新样本并累加 `kfifo_dropped`（`driver/sensor_char.c:390`），
通过 `ioctl(GET_STATS)` 暴露给用户态；`kfifo_dropped` 持续增长就是"读方跟不上采样"的告警。
②**守恒不变量**：`ring_count + kfifo_dropped == irq_count`（在"无消费者"阶段成立）。
本项目实测 `ring_count=85 dropped=215 irq=300`，`85+215=300`，`conservation_delta=0`
（`logs/20260914-133755-main-03-ringbuffer-mmap.log`），检查项名为
`样本守恒：ring_count + dropped == irq_count`。
这条等式能抓住"样本无声蒸发"——例如 `copy_to_user` 失败后直接丢弃
（本项目因此把样本放回队列，`driver/sensor_char.c:592`）。
另外 `dropped_at_capacity=1` 断言"发生过丢弃时队列必然是满的"，
能抓"计数算错但队列没满"的实现错误。

**Q7：为什么内核侧用了 `seqlock_t`，用户态还需要在共享区里自己维护 `seq`？**

A：因为 `seqlock_t` 的序号存在内核私有的 `seqcount_t`（`include/linux/seqlock.h:64-69`）里，
用户态 `mmap` 的那一页**看不到它**。只在内核用 `write_seqlock` 只保证内核写者互斥，
对用户态读者没有任何可观测的一致性信号，用户态的重试逻辑会变成死代码。
正确做法：把 `seqlock_t` 换成 `spinlock_t` 做写者互斥，
由内核亲手在共享区维护一个**用户可见的 `seq`**（`driver/sensor_char.c:298-330`），
并用显式 `smp_wmb()` 保证"置奇先于数据、数据先于置偶"。
本项目的验证者用负控证明了这个坑：删掉写侧 `write_seqlock` 或让读侧重试永假，
旧实现的测试**仍然 28 PASS / 0 FAIL**（`logs/20260914-111708` / `111633`），
说明当时的检查对"假 seqlock"零判别力；整改后反向对照（删掉写 seq）
在同一放大窗口下 `mmap_invalid=24`、8 项 FAIL（`logs/20260914-123122`）。

**Q8：用户态读共享内存没有 `smp_rmb()`，怎么办？**

A：用 GCC 内建 `__sync_synchronize()`（全屏障）或 C11
`atomic_thread_fence(memory_order_acquire/seq_cst)`，配合 `volatile` 或 `atomic_load`。
`volatile` 只能阻止编译器把值缓存到寄存器，**不提供内存序**，所以屏障必须显式写。
读侧要两次屏障：读数据**之前**一次（防止读到新数据配旧序号），
重读序号**之前**一次（防止序号比较被重排到读数据之前）。
还要设**重试上限**并计入错误统计，否则写者持续写时用户态读者可能饿死。

**Q9：为什么 `kfifo_alloc(64 * 24)` 最后能放 85 个？**

A：`kfifo_alloc` 里做了 `size = roundup_pow_of_two(size)`
（`lib/kfifo.c:24/31`）：请求 1536B → 取整到 2048B，`mask+1 = 2048`，
`2048 / 24 = 85` 个样本。所以**不能假定实际容量等于请求容量**，
要么用 `kfifo_size()/sizeof(elem)` 算真实容量并暴露给用户态
（`driver/sensor_char.c:471`），要么在文档里写清楚。这是实测踩到的坑：
`ring_capacity=85` 而契约写的是 64。

**Q10：`kfifo_dropped` 为什么选择"丢新"而不是"覆盖旧"？**

A：见 1.6。核心理由：生产者在线程化中断下半部，不能阻塞；覆盖旧会静默改写历史，
"丢了多少"不可知；丢新 + 计数让失配变成可观测量；而且最新值另有 `latest`/`mmap` 兜底，
不会因为丢新而"拿不到当前值"。负控 NC-C 把策略改成覆盖旧后 `dropped` 检查立刻 FAIL，
说明这个策略是被检查项守住的，不是口头承诺。

**Q11：`PAGE_READONLY` 和 `VM_WRITE` 检查是不是重复了？**

A：不是。`PAGE_READONLY` 管的是建立的 PTE 权限（页表层面的只读），
`VM_WRITE` 是 VMA 层面的标志检查。两者都设成不可写才是双重保险：
即使某条路径改了 `vm_page_prot`，`VM_WRITE` 检查也会在 `.mmap` 入口直接 `-EPERM`。
实测往映射区写会触发 SIGSEGV（`mmap_ro_sigsegv=1`）。

**Q12：真机上这块共享内存如果要给 DMA 用，要改什么？**

A：改成 `dma_alloc_coherent()`，或者普通内存 + `dma_map_single()` 建立 DMA 映射，
并在所有权交接点调用 `dma_sync_single_for_cpu()` / `dma_sync_single_for_device()`
（`include/linux/dma-mapping.h:120/122`）做 cache 同步；
遵守"任一时刻 buffer 只属于 CPU 或设备一方"的所有权原则。
本项目设备是软件模拟，不涉及真实 DMA，所以用普通内存即可；这部分**未在真机验证**。

---

## 7. 亲手验证：QEMU 内可复现命令、预期输出、实测数据

### 7.1 复现链路（本工作树，构建目录隔离）

```bash
# ① 虚拟机内编译模块 + 用户态程序 + 打包 initramfs
limactl shell dev bash -c 'WORKTREE=03v bash /Users/lucien/workspace/self-study/projects/wt-03v/scripts/13-vm-fast-cycle.sh'

# ② 把产物从虚拟机拷到 artifacts/
WORKTREE=03v bash /Users/lucien/workspace/self-study/projects/wt-03v/scripts/21-macos-sync-artifacts.sh

# ③ 启动 QEMU 跑阶段 03 测试（MTTCG + 2 vCPU，自动 PASS/FAIL 结论）
WORKTREE=03v bash /Users/lucien/workspace/self-study/projects/wt-03v/scripts/22-macos-run-test.sh 03-ringbuffer-mmap 300

# ④ 回归
WORKTREE=03v bash /Users/lucien/workspace/self-study/projects/wt-03v/scripts/22-macos-run-test.sh 01-io-models 300
WORKTREE=03v bash /Users/lucien/workspace/self-study/projects/wt-03v/scripts/22-macos-run-test.sh smoke 180
```

> 若内核树仍是 KCSAN 模式（`CONFIG_KCSAN=y`）先不要构建，等 60 秒重查（最多 10 分钟）。
> 日志落在仓库 `logs/`（文件名带时间戳与本工作树标识，如
> `logs/20260914-133755-main-03-ringbuffer-mmap.log`）。

### 7.2 QEMU 内的检查项（预期输出形态）

正常模式（`/bin/ring_mmap_test`）：

```
[RING] dropped=215
[RING] ring_count=85
[RING] ring_capacity=85
[RING] conservation_delta=0
[RING] drained_samples=10 drained_seq_distinct=1 seq_span=9
[RING] mmap_reads=100 mmap_retries=0 mmap_invalid=0
[RING] shm_seq_probe_first=600 shm_seq_probe_last=602 waited_ms=5
[RING] mmap_ro_sigsegv=1
[RING] mmap_oversize_errno=22
[RING] buffered_read_ms=0
[RING] new_fd_sees_buffered=1
[RING] OVERALL=PASS
```

并发（`/bin/concurrency_test`）：

```
[CONC] consumed_seqs=41 dup_seqs=0 seq_min=1 seq_max=41 seq_gaps=0 seq_overflow=0
[CONC] children=8 failed=0
[CONC] samples_read=41 eagain=1559 io_errors=0 invalid_samples=0 snapshots=1600 i2c_errors=0
[CONC] OVERALL=PASS
```

放大器模式（脚本在 QEMU 内执行）：

```sh
KVER=$(uname -r)
rmmod sensor_char
insmod /lib/modules/$KVER/sensor_char.ko shm_publish_delay_us=1000
sleep 1
/bin/ring_mmap_test quick
# 预期：odd_seen > 0、retries > 0、invalid == 0
rmmod sensor_char && insmod /lib/modules/$KVER/sensor_char.ko   # 恢复默认参数
```

### 7.3 本项目实测数据（取自 `logs/`，注明文件名）

主证据：**`logs/20260914-133755-main-03-ringbuffer-mmap.log`**（`-accel tcg,thread=multi -smp 2`，
`smp: Brought up 1 node, 2 CPUs`，内核 `#4`，非 KCSAN）。

| 指标 | 实测值 | 说明 |
|---|---|---|
| `irq_count` | 300 | 10ms 周期静置 3s，样本全部产生 |
| `ring_count` | 85 | 队列被填满 = 驱动打印的真实容量 |
| `kfifo_dropped` | 215 | **85 + 215 = 300**，`conservation_delta=0`，无静默丢失 |
| `dropped_at_capacity` | 1 | 发生过丢弃时队列必然满 |
| `drained_samples / seq_span` | 10 / 9 | 连读 10 次拿到 10 个不同样本（序号跨度 9）→ 历史被保留 |
| `mmap_reads / retries / invalid` | 100 / 0 / 0 | 100 轮一致性读无撕裂样本 |
| `shm_seq_probe_first → last` | 600 → 602（5ms） | 确定性探针证明内核真的在写共享区 `seq` |
| `mmap_ro_sigsegv` | 1 | 只读映射写入触发 SIGSEGV |
| `mmap_oversize_errno` | 22 (EINVAL) | 超一页映射被拒绝 |
| `buffered_read_interval_ms / ms` | 2000 / 0 | 周期 2s 下 `read` 仍立即返回（"等下一个样本"会是 ~2000ms） |
| 并发 | `samples_read=41 eagain=1559 invalid_samples=0 snapshots=1600 i2c_errors=0 failed=0` | 1600 次 mmap 快照全部有效 |
| 并发唯一性 | `consumed_seqs=41 dup_seqs=0 seq_gaps=0` | 出队全局无重复（L2 证据） |
| 放大器（1000µs 窗口） | `mmap_reads=5015075 retries=127494549 odd_seen=147 changes=347 invalid=0` | 重试路径真的被执行，且始终不撕裂 |
| 阶段结论 | `[TEST:END] 03-ringbuffer-mmap pass=40 fail=0` | 40 项全 PASS |

辅助证据：

| 用途 | 日志 | 关键值 |
|---|---|---|
| "读到 300 次变化未撞窗口" | `logs/20260914-122253-03-ringbuffer-mmap.log` | `shm_seq_changes=300 shm_seq_odd_seen=0` |
| 20ms 大窗口放大器 | `logs/20260914-123040-03-ringbuffer-mmap.log` | `odd_seen=498 retries=630138580 invalid=0`（15s） |
| 负控 NC-1（删写侧 seq） | `logs/20260914-123122-03-ringbuffer-mmap.log` | `invalid=24`，`pass=32 fail=8` |
| 负控 NC-C（改覆盖旧） | `logs/20260914-111727-03-ringbuffer-mmap.log` | `pass=25 fail=3`（dropped 检查 FAIL） |
| 旧假 seqlock 负控无判别力 | `logs/20260914-111633` / `111708-03-ringbuffer-mmap.log` | 均 `pass=28 fail=0` |
| KCSAN strict（03 无驱动报告） | `logs/20260914-130201-03-ringbuffer-mmap.log` | `pass=40 fail=0`，无驱动相关 `BUG: KCSAN` |
| KCSAN（内核 rmap 报告，非驱动） | `logs/20260914-130220-01-io-models.log` | `BUG: KCSAN: data-race in folio_add_file_rmap_range / page_remove_rmap` |
| 回归 01 / smoke | `logs/20260914-133807-main-01-io-models.log` / `logs/20260914-133818-main-smoke.log` | `01 pass=16 fail=0` / `smoke pass=15 fail=0` |

### 7.4 QEMU 内可直接复现的最小实验（不需要脚本）

```sh
# 在 QEMU 里（test=03-ringbuffer-mmap 启动后）
ls -l /dev/sensor0                      # 设备节点存在
cat /proc/kallsyms | grep kfifo         # 确认 kfifo 在内核（可选）
/bin/ring_mmap_test | head -20          # 看 dropped / ring_count / conservation_delta
/bin/concurrency_test | tail -5         # 看 dup_seqs / invalid_samples
dmesg | grep -c 'WARNING:'              # 预期 0
```

---

## 8. 内核源码引用索引（按文件）

| 文件:行号 | 内容 |
|---|---|
| `include/linux/kfifo.h:29-33` | kfifo 加锁要求（单读者单写者免锁） |
| `include/linux/kfifo.h:40` | `struct __kfifo`（in/out/mask/esize/data） |
| `include/linux/kfifo.h:199` | `kfifo_size = mask + 1` |
| `include/linux/kfifo.h:233` | `kfifo_len = in - out` |
| `include/linux/kfifo.h:243` | `kfifo_is_empty` |
| `include/linux/kfifo.h:287` | `kfifo_is_full = len > mask` |
| `include/linux/kfifo.h:205-209` | `kfifo_reset` 必须独占 |
| `include/linux/kfifo.h:390` / `:423` | `kfifo_put`（写槽位 → `smp_wmb()` → `in++`） |
| `include/linux/kfifo.h:437` / `:461` | `kfifo_get`（读槽位 → `smp_wmb()` → `out++`） |
| `include/linux/kfifo.h:519` / `:587` | `kfifo_in` / `kfifo_out` 免锁版 |
| `include/linux/kfifo.h:541` / `:611` | `kfifo_in_spinlocked` / `kfifo_out_spinlocked`（`spin_lock_irqsave`） |
| `lib/kfifo.c:24` / `:31` | `__kfifo_alloc` / `roundup_pow_of_two` |
| `lib/kfifo.c:110` / `:125` | 写路径 `smp_wmb()` / `in += len` |
| `lib/kfifo.c:149` / `:166` | 读路径 `smp_wmb()` / `out += len` |
| `include/linux/seqlock.h:64-69` / `:860` | `seqcount_t` / `seqlock_t` |
| `include/linux/seqlock.h:325-331` | `__read_seqcount_begin`（奇数自旋） |
| `include/linux/seqlock.h:342-347` | `raw_read_seqcount_begin`（`smp_rmb`） |
| `include/linux/seqlock.h:446-450` | `do_read_seqcount_retry`（`smp_rmb` + 比较） |
| `include/linux/seqlock.h:466-472` | `do_raw_write_seqcount_begin`（`sequence++` → `smp_wmb()`） |
| `include/linux/seqlock.h:487-492` | `do_raw_write_seqcount_end`（`smp_wmb()` → `sequence++`） |
| `include/linux/seqlock.h:931` | `write_seqlock()` |
| `mm/memory.c:2511` / `:2482` | `remap_pfn_range` / `remap_pfn_range_notrack` |
| `mm/memory.c:2009` / `:1963` | `vm_insert_page` / `vm_insert_pages` |
| `mm/vmalloc.c:757` | `vmalloc_to_pfn` |
| `include/linux/circ_buf.h:9/16/21` | `circ_buf` / `CIRC_CNT` / `CIRC_SPACE` |
| `include/linux/dma-mapping.h:120/122` | `dma_sync_single_for_cpu/device` |
| `driver/sensor_char.c:298-330` | 用户可见 seqlock 写侧（本项目） |
| `driver/sensor_char.c:388-390` | kfifo 入队 + 丢新计数（本项目） |
| `driver/sensor_char.c:462` / `:471` / `:478` | 无锁读 kfifo / 容量换算 / 计数换算 |
| `driver/sensor_char.c:555` | `kfifo_out_spinlocked` 出队（本项目） |
| `driver/sensor_char.c:663-702` | `sensor_mmap` 全部校验 + `remap_pfn_range` |
| `driver/sensor_char.c:805-831` | RESET：kfifo + 共享区同步清空 |
| `driver/sensor_char.c:1007` | `__get_free_pages(GFP_KERNEL | __GFP_ZERO, 0)` |
| `driver/sensor_ioctl.h:37-52` | `struct sensor_shm` 布局（magic/version/count/write_idx/seq/reserved/samples） |
| `driver/sensor_ioctl.h:53-72` | `struct sensor_stats` 追加字段（ABI 兼容） |

---

## 9. 本阶段遗留风险与"面试时不该夸大的地方"

1. **并发"无竞态"是 L2 证据等级**（多核 + 唯一性断言），不是数学证明；KCSAN 只在一次
   strict 模式 03 运行中无驱动相关报告，未做长时间扫描（见 5）。
2. **`mmap` 共享区固定 32 个样本**：用户态读得慢于 32 个周期只能看到最近 32 个，
   更长历史要靠 `read()` 的 kfifo（两者是独立通道）。
3. **`read()` 与 `mmap` 的数据不互相消耗**：`read` 出队消耗 kfifo，mmap 窗口不受影响。
   用户态必须知道"读走的样本不会从共享区消失"，否则会把"共享区还有旧样本"误判成 bug。
4. **用户态 seqlock 重试上限**是可用性兜底，不是理论保证；写者若长时间占住窗口，
   用户态会返回失败而不是无限重试。
5. **真机 DMA / cache 一致性 / MMIO 映射属性未在本项目验证**（标「未验证」）。
6. **lockdep 未启用（未验证）**；开启需要重建内核并重新跑全部回归。
