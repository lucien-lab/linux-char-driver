# 阶段 01 知识点：四种 IO 模型（阻塞 / 非阻塞 / poll·epoll / fasync·SIGIO）

> 面向目标：嵌入式 / Linux 驱动岗面试。读完应当能**讲清机制、画出调用链、答出边界**，
> 并能被追问到"为什么这么写、不这么写会怎样"。
>
> 本文所有内核源码引用都来自本项目的目标内核树
> `~/kernel-build/linux-6.6.156`（Linux 6.6.156），格式为 `文件:行号`。
> 所有驱动行为都来自 `driver/sensor_char.c` 的实测结果，日志见
> `logs/20260914-025014-01-io-models.log`（阶段测试 9 PASS）与
> `logs/20260914-025021-smoke.log`（回归 12 PASS）。

---

## 0. 一句话心智模型

**驱动不实现"阻塞"或"多路复用"，驱动只做两件事：**

1. **回答状态**：`.read/.poll` 被调用时，根据当前数据，返回"有没有数据"（返回码 / 事件掩码）；
2. **登记唤醒源**：在需要等数据时，告诉内核"数据到了用哪个等待队列叫我"。

真正让进程睡下去、真正实现 select/epoll 多路复用、真正把事件翻译成用户态 API 的，
全部是 **VFS 与系统调用层（`fs/read_write.c`、`fs/select.c`、`fs/eventpoll.c`）** 在做事。
驱动是"被问的人"，不是"管理睡觉的人"。这句话是整篇文档的地基。

四种模型的差别，本质上是**三类交互**的组合：

| 交互 | 由谁完成 | 驱动侧对应 |
|---|---|---|
| 等待数据（睡 / 不睡） | 系统调用层 | `.read` 里检查 `O_NONBLOCK`，用 waitqueue 阻塞 |
| 多路复用（一次睡多个 fd） | `fs/select.c` / `fs/eventpoll.c` | `.poll` + `poll_wait()` |
| 反向通知（内核主动戳用户态） | `fs/fcntl.c` 的 fasync 框架 | `.fasync` + `kill_fasync()` |

---

## 1. 四种 IO 模型全景

### 1.1 对照表

| 模型 | 用户态 API | 触发方式 | VFS/内核核心 | 驱动 fops | 驱动的责任 |
|---|---|---|---|---|---|
| 阻塞 read | `read()` | 同步等待 | `vfs_read()` | `.read` | 检查 `O_NONBLOCK`；在 `wq` 上等 |
| 非阻塞 read | `read()` + `O_NONBLOCK` | 立即返回 | `vfs_read()` | `.read` | 无数据返回 `-EAGAIN` |
| 多路复用 | `select()/poll()/epoll_*` | 事件就绪 | `fs/select.c`、`fs/eventpoll.c` | `.poll` | `poll_wait()` + 返回事件掩码 |
| 异步通知 | `fcntl(F_SETOWN/F_SETFL)` + `SIGIO` | 内核主动 | `fs/fcntl.c` fasync 框架 | `.fasync` | `fasync_helper()`；数据就绪时 `kill_fasync()` |

### 1.2 阻塞 read 的完整调用链

```text
用户态   read(fd, buf, n)
  │
VFS      ksys_read → vfs_read()                       fs/read_write.c:450
  │        （注意：vfs_read 本身完全不碰 O_NONBLOCK，
  │          它只做权限/合法性检查，然后把控制权交给 f_op）
  │
fops     sensor_read()                                 driver/sensor_char.c
  │        if (!sensor_has_new_sample(sf)) {
  │            if (file->f_flags & O_NONBLOCK) return -EAGAIN;   ← 非阻塞分支
  │            wait_event_interruptible_timeout(sd->wq, cond, 2000ms);
  │        }
  │
内核同步  ___wait_event()                              include/linux/wait.h:304
  │        for (;;) {
  │            prepare_to_wait_event(&wq, &entry, TASK_INTERRUPTIBLE);
  │            if (condition) break;
  │            schedule_timeout(...);      ← 真正睡在这里
  │        }
  │        finish_wait();
  │
等待队列  prepare_to_wait_event()                      kernel/sched/wait.c:310
  │        spin_lock_irqsave(&wq_head->lock)             ← 与 wake_up 共用同一把锁
  │        __add_wait_queue[_entry_tail](wq_head, entry)
  │        set_current_state(TASK_INTERRUPTIBLE)
  │        spin_unlock_irqrestore(&wq_head->lock)
  ▼
被唤醒   wake_up_interruptible(&sd->wq)   ← sensor_char.c 线程化中断下半部
```

`-ETIMEDOUT` / `-ERESTARTSYS` 的判定来自 `wait_event_interruptible_timeout` 的返回值：
超时返回 `0`，被信号打断返回负数，条件满足返回剩余 jiffies（`include/linux/wait.h:517-534` 注释）。

### 1.3 非阻塞 read 的调用链

与非阻塞共用**同一条链**，唯一分叉点在驱动第 1 行判断：

```c
if (file->f_flags & O_NONBLOCK)
    return -EAGAIN;
```

关键事实：**`vfs_read()` 里没有任何一行代码处理 `O_NONBLOCK`**（见 `fs/read_write.c:450-483`），
它只是把 `file` 原样传给 `->read`。所以"非阻塞"是**驱动自己读 `file->f_flags` 实现的**。

对比 `->read_iter` 风格：内核里 pipe/eventfd 的写法是**两个条件都检查**
（`fs/pipe.c:345`、`fs/eventfd.c:236`）：

```c
if ((filp->f_flags & O_NONBLOCK) || (iocb->ki_flags & IOCB_NOWAIT))
        return -EAGAIN;
```

其中 `IOCB_NOWAIT` 来自 `preadv2/pwritev2` 的 `RWF_NOWAIT` 标志，**不是**由 `O_NONBLOCK` 自动推导的
（`iocb_flags()` 只映射 `O_APPEND/O_DIRECT/O_DSYNC/__O_SYNC`，见 `include/linux/fs.h:3288-3300`；
`init_sync_kiocb()` 用 `filp->f_iocb_flags` 初始化，见 `include/linux/fs.h:2170-2177`）。
本驱动用 `->read`，所以只认 `f_flags`。

### 1.4 poll / select 的调用链

`select()` 与 `poll()` 在内核里是同一套机制（select 会转成 poll 表），`__pollwait` 与 `do_poll` 共用。

```text
用户态   poll(fds, nfds, timeout)  /  select(nfds, ...)
  │
VFS      do_sys_poll()                                fs/select.c:971
  │        poll_initwait(&table)        ← table 放在栈上   fs/select.c:1011
  │           └─ init_poll_funcptr(&pwq->pt, __pollwait)  fs/select.c:119-121
  │
         do_poll(&list, &table, end_time)               fs/select.c:882
  │        for (;;) {
  │            for (每个 fd)
  │                do_pollfd() → vfs_poll()             include/linux/poll.h:88
  │                              → file->f_op->poll() = sensor_poll()
  │            if (count) break;
  │            poll_schedule_timeout()   ← 睡 TASK_INTERRUPTIBLE
  │        }
  │
fops     sensor_poll()                                 driver/sensor_char.c
  │        poll_wait(file, &sd->wq, wait)   ← 只登记，不睡眠
  │        return sensor_has_new_sample(sf) ? EPOLLIN|EPOLLRDNORM : 0;
  │
登记     poll_wait()                                   include/linux/poll.h:42-58
  │        p->_qproc(filp, wait_address, p)   ← 这里 _qproc == __pollwait
  │        smp_mb()                          ← 防止后面的条件判断被重排到入队之前
  │
         __pollwait()                                  fs/select.c:219-232
  │        entry = poll_get_entry(pwq)      ← 由调用方（poll_wqueues）持有
  │        get_file(filp)
  │        init_waitqueue_func_entry(&entry->wait, pollwake)
  │        add_wait_queue(&sd->wq, &entry->wait)
  ▼
唤醒     wake_up_interruptible(&sd->wq)
           → __wake_up_common() → entry->wait.func = pollwake()  fs/select.c:208
                → __pollwake(): pwq->triggered = 1 + default_wake_function()  fs/select.c:182-205
  │
清理      poll_freewait(&table)  ← 一次系统调用返回时摘除全部 entry  fs/select.c:136-156
```

### 1.5 epoll 的调用链（两阶段，与 select 最大的不同）

```text
① 注册阶段：epoll_ctl(epfd, EPOLL_CTL_ADD, fd, ev)
   fs/eventpoll.c ep_insert()                          fs/eventpoll.c:1450
     init_poll_funcptr(&epq.pt, ep_ptable_queue_proc)  fs/eventpoll.c:1525
     ep_item_poll(epi, &epq.pt, 1)                     fs/eventpoll.c:1534
       → vfs_poll(file, pt) → sensor_poll()
         → poll_wait() → pt->_qproc = ep_ptable_queue_proc
              kmem_cache_alloc(eppoll_entry)           fs/eventpoll.c:1244-1272
              init_waitqueue_func_entry(&pwq->wait, ep_poll_callback)  fs/eventpoll.c:1260
              add_wait_queue(&sd->wq, &pwq->wait)      ← 常驻注册！生命周期 = epitem 生命周期
              epi->pwqlist 挂上这条 entry
     if (revents) 把 epi 放进 ep->rdllist（已经可读就先记一笔）

② 数据到达：wake_up_interruptible(&sd->wq)  ← 驱动中断下半部
   __wake_up_common() → pwq->wait.func = ep_poll_callback()   fs/eventpoll.c:1133
     spin_lock_irqsave(&ep->lock)
     list_add_tail(&epi->rdllink, &ep->rdllist)   ← 挂到就绪链表
     spin_unlock_irqrestore(...)
     wake_up(&ep->wq)                             ← 唤醒睡在 epoll_wait 里的进程

③ 收割阶段：epoll_wait(epfd, events, maxevents, timeout)
   ep_poll()                                          fs/eventpoll.c:1798 起
     init_wait(&wait); __add_wait_queue_exclusive(&ep->wq, &wait)
     ep_send_events(ep, events, maxevents)            fs/eventpoll.c:1649
       init_poll_funcptr(&pt, NULL)  ← _qproc = NULL！ fs/eventpoll.c:1665
       ep_item_poll(epi, &pt, 1) → sensor_poll()
         → poll_wait() 里 `if (p && p->_qproc && ...)` 不成立 → 纯 no-op
         → 仍然重新计算并返回事件掩码
```

**记住这条：`.poll` 会被调用多次，但 `poll_wait()` 真正登记等待项只在
"`_qproc != NULL`" 的时候，也就是 select/poll 的每次系统调用、以及 `epoll_ctl(ADD)`。
`epoll_wait` 内部的复检是 `_qproc == NULL`，`poll_wait` 是空操作。**

### 1.6 fasync / SIGIO 的调用链

```text
用户态   sigprocmask(BLOCK, SIGIO)            ← sigtimedwait 要求信号先阻塞
         fcntl(fd, F_SETOWN, getpid())        ← ① 指定"信号发给谁"
         fcntl(fd, F_SETFL, flags | O_ASYNC)  ← ② 打开异步开关
  │
VFS      do_fcntl()                                   fs/fcntl.c
  │   F_SETOWN: fs/fcntl.c:370 → f_setown(filp, argi, 1)  fs/fcntl.c:109
  │              → __f_setown() 写 filp->f_owner.pid / pid_type
  │   F_SETFL : fs/fcntl.c:333 → setfl()                 fs/fcntl.c:37
  │              if (((arg ^ filp->f_flags) & FASYNC) && filp->f_op->fasync)
  │                  filp->f_op->fasync(fd, filp, (arg & FASYNC) != 0)   fs/fcntl.c:73-74
  │
fops     sensor_fasync(fd, file, on)                   driver/sensor_char.c
  │        return fasync_helper(fd, file, on, &sf->sd->fasync);
  │
框架     fasync_helper()                               fs/fcntl.c:967
  │        on == 0 → fasync_remove_entry()             fs/fcntl.c:856
  │        on != 0 → fasync_add_entry()                fs/fcntl.c:938
  │                    fasync_alloc()  ← kmem_cache_alloc(GFP_KERNEL) fs/fcntl.c:882
  │                    fasync_insert_entry(): 挂进链表 + filp->f_flags |= FASYNC  fs/fcntl.c:904-934
  ▼
数据到达（驱动线程化中断下半部）：
         sd->latest 更新 → wake_up_interruptible(&sd->wq) → kill_fasync(&sd->fasync, SIGIO, POLL_IN)
  │
框架     kill_fasync()                                 fs/fcntl.c:1004
  │        if (*fp) { rcu_read_lock(); kill_fasync_rcu(...); rcu_read_unlock(); }
  │        kill_fasync_rcu() 遍历链表                   fs/fcntl.c:979
  │            read_lock_irqsave(&fa->fa_lock)
  │            send_sigio(&fa->fa_file->f_owner, fa->fa_fd, band)  fs/fcntl.c:997
  │
发送     send_sigio()                                  fs/fcntl.c:765
           read_lock_irqsave(&fown->lock)
           pid = fown->pid;  if (!pid) goto out;   ← 没做过 F_SETOWN 就发不出去
           pid_task() → send_sigio_to_task() → do_send_sig_info()
  │
用户态   SIGIO 被投递（标准信号不排队，多次会合并成一个 pending）
```

### 1.7 驱动侧"责任清单"（面试可直接背）

| 驱动必须做 | 驱动不需要做 |
|---|---|
| `.read` 里检查 `O_NONBLOCK` 并返回 `-EAGAIN` | 判断当前进程是否想做非阻塞（VFS 已把标志放进 `file->f_flags`） |
| `.poll` 里先 `poll_wait()` 再判断状态 | 让进程睡眠（`do_poll`/`ep_poll` 负责） |
| `.poll` 返回**当前**状态（电平语义） | 区分 select 还是 epoll；区分 LT/ET |
| `.fasync` 里调用 `fasync_helper()` | 维护进程/SIGIO 映射（框架负责） |
| 数据就绪时 `wake_up_*` + `kill_fasync` | 记录"谁注册了异步通知"到驱动私有结构 |
| `release` 里 `fasync(-1, file, 0)` 摘除 | —— |

---

## 2. poll 机制细节

### 2.1 `poll_wait()` 到底做了什么

源码只有 8 行（`include/linux/poll.h:42-58`）：

```c
static inline void poll_wait(struct file * filp, wait_queue_head_t * wait_address, poll_table *p)
{
	if (p && p->_qproc && wait_address) {
		p->_qproc(filp, wait_address, p);
		smp_mb();
	}
}
```

拆开看：

- **`p->_qproc` 是"登记函数"的指针**，由调用方（VFS）预先塞进 `poll_table`：
  - `poll()` / `select()` → `poll_initwait()` 填 `__pollwait`（`fs/select.c:119-121`）；
  - `epoll_ctl(ADD)` → `ep_insert()` 填 `ep_ptable_queue_proc`（`fs/eventpoll.c:1525`）；
  - `epoll_wait` 内部复检 → 填 `NULL`（`fs/eventpoll.c:1665`）。
- **`smp_mb()` 不是可有可无**：它保证"等待项已经挂进 `sd->wq`"这件事，对后续的状态判断
  以及产生唤醒的那个 CPU 是可见的；没有它，编译器/CPU 可能把后面的条件判断重排到入队之前，
  制造丢唤醒窗口（`include/linux/poll.h:48-52` 的注释明确说明它配对 `wq_has_sleeper()`）。
- **它绝不睡眠**：只做一次"把回调挂到等待队列"的登记。`.poll` 因此是"一次性函数"。

### 2.2 为什么不睡眠、entry 由谁持有

`.poll` 的调用路径是**多路复用系统调用内部的某个循环**，其栈帧属于 `do_sys_poll()` 或
`ep_insert()`。它只是问驱动一个问题："现在什么状态？要等的话我登记在哪儿？"

- 真正睡眠发生在**系统调用层**：`poll_schedule_timeout()`（`fs/select.c:234`）或
  `ep_poll()` 里的 `schedule_hrtimeout_range()`（`fs/eventpoll.c:1894`）。
- `.poll` 如果自己睡眠（例如调用了会睡眠的锁或等待），就会破坏整个循环的语义：
  调用方还没决定要不要睡，驱动先睡了；而且 `epoll_ctl` 的路径上持有 `ep->mtx`。

**entry 的持有者，决定了 select 和 epoll 的根本差异：**

| | select / poll | epoll |
|---|---|---|
| `_qproc` | `__pollwait`（`fs/select.c:219`） | `ep_ptable_queue_proc`（`fs/eventpoll.c:1244`） |
| entry 类型 | `struct poll_table_entry` | `struct eppoll_entry` |
| 分配方式 | `poll_get_entry()`：先内联数组，满了再 `__get_free_page()`（`fs/select.c:158-179`） | `kmem_cache_alloc(eppoll_entry)` |
| 生命期所有者 | `struct poll_wqueues`，**在系统调用栈上**（`include/linux/poll.h:105-113`） | `struct epitem` 的 `pwqlist`，**挂在 epoll 集合里** |
| 释放时机 | `poll_freewait()`，一次系统调用返回时全部摘除（`fs/select.c:136`） | `EPOLL_CTL_DEL` / `close(epfd)` / 文件释放时摘除 |
| 每次 `poll()` 是否重登记 | 是 | 否（ADD 时一次，之后常驻） |
| 唤醒回调 | `pollwake()` → 设 `pwq->triggered` 并 `default_wake_function()` | `ep_poll_callback()` → `epi` 挂 `ep->rdllist` 并 `wake_up(&ep->wq)` |

这也解释了"**为什么 select 有 fd 数量限制和 O(n) 开销，而 epoll 没有**"：
select 每个 fd 都要在系统调用栈/临时页里维护一个 entry；epoll 每个 fd 只在 ADD 时花一次
分配，之后事件到达是 O(1) 挂链表。

### 2.3 为什么 epoll 需要 `poll_wait` 注册的等待队列

假设驱动写了 `.poll` 但**不调用 `poll_wait`**：

```c
/* 错误示范 */
static __poll_t sensor_poll_bad(struct file *file, poll_table *wait)
{
	struct sensor_file *sf = file->private_data;
	return sensor_has_new_sample(sf) ? EPOLLIN : 0;
}
```

- `epoll_ctl(ADD)` 时驱动返回 0（当时没数据）→ `epi` 不进 `rdllist`；
- 因为没调 `poll_wait`，**`sd->wq` 上根本没有挂 `epoll` 的 `ep_poll_callback` 回调**；
- 之后中断下半部 `wake_up_interruptible(&sd->wq)` 遍历 `sd->wq`，**找不到任何 epoll 回调**，
  于是 `epi` 永远不会被放进 `rdllist`；
- `epoll_wait` 只能等到超时返回 0 —— **事件被丢掉**。

`wake_up` 只唤醒"挂在这条队列上的东西"。epoll 想要被通知，就必须先把自己挂上去，
而"挂上去"的唯一标准入口就是驱动的 `poll_wait()`。

对 select/poll 同理，只是它们的 entry 是每次调用临时的：`__pollwait` 的回调 `pollwake`
会把 `pwq->triggered` 置 1，`do_poll` 的循环因此不会真的睡下去（`fs/select.c:182-205`、`fs/select.c:882-968`）。

### 2.4 电平触发 / 边沿触发：驱动只提供电平语义

**结论：驱动侧不区分 LT/ET，驱动只负责回答"此刻是否有数据"。LT/ET 完全是
`fs/eventpoll.c` 在用户态接口层面的行为。**

证据在 `ep_send_events()`（`fs/eventpoll.c:1649-1741`）：事件被投递给用户态之后，

```c
if (epi->event.events & EPOLLONESHOT)
        epi->event.events &= EP_PRIVATE_BITS;
else if (!(epi->event.events & EPOLLET)) {
        /* 电平触发：把 epi 重新放回就绪链表，
           下一次 epoll_wait 会再调用一次 f_op->poll() 复查 */
        list_add_tail(&epi->rdllink, &ep->rdllist);
        ep_pm_stay_awake(epi);
}
```

- **LT**：每次 `epoll_wait` 都会重新 `ep_item_poll()`（即调 `.poll`）复查；只要
  `.poll` 还返回 `EPOLLIN`，就继续上报。所以驱动必须让"样本被消费后 `.poll` 返回 0"成立。
- **ET**：投递一次后不再放回就绪链表；只有新的 `wake_up`（新的 `ep_poll_callback`）
  才会重新入链。因此 ET 模式要求用户态**一直读到 `EAGAIN`**，否则残留数据不会再产生新事件。

本驱动的正确性正好落在这条分界线上：

```c
static bool sensor_has_new_sample(struct sensor_file *sf)
{
	return READ_ONCE(sf->sd->latest.seq) != sf->last_seq;
}
```

`read()` 成功后会 `sf->last_seq = snap.seq`，于是同一个 fd 的下一次 `.poll` 返回 0。
LT 语义因此成立：**消费 → 不再上报 → 不会忙轮询**。

实测（`logs/20260914-025014-01-io-models.log`）：

```text
[IO] epoll_wakeups=7
[IO] epoll_reads=7 elapsed_ms=3001
```

500ms 采样周期、3 秒窗口，期望唤醒 6~7 次；实测 7 次，与"每个样本唤醒一次"严格吻合。
若 `.poll` 退化成"曾经有过数据就报可读"，唤醒次数会达到数千次（见第 6.4 节反例实验）。

> 追问点：`epoll_wait` 里的复检传的是 `_qproc = NULL`，所以 `.poll` 里判断
> `if (wait && wait->_qproc)` 来决定"要不要真的登记"是合法优化，但**绝不能**用它来跳过
> 状态计算——`_qproc == NULL` 时仍然必须返回正确的事件掩码。

### 2.5 `poll` 返回值约定

- 返回类型是 `__poll_t`（`typedef unsigned int` 风格的位掩码），**不是** errno 风格的负数；
  出错信息通过 `EPOLLERR/EPOLLHUP` 位表达。
- **`EPOLLERR` / `EPOLLHUP` 无论用户是否在 `events` 里请求，都必须上报**；
  `select/epoll` 收到后一定会返回给用户态（不会因为 `events` 没写而被过滤）。
- **`EPOLLNVAL` 由内核填**（fd 非法），驱动不填；`select()` 里对应 `POLLNVAL`。
- 名字统一：`POLLIN` 与 `EPOLLIN` 在内核内部是同一个值（4.16 起统一为 `EPOLL*`）；
  `POLLRDNORM` ↔ `EPOLLRDNORM` 同理。本驱动返回 `EPOLLIN | EPOLLRDNORM`，
  这是字符设备表达"可读"最常见、最兼容的写法。
- 没有 `.poll` 的字符设备，`vfs_poll()` 直接返回 `DEFAULT_POLLMASK`
  （`include/linux/poll.h:88-91`，值为 `EPOLLIN|EPOLLOUT|EPOLLRDNORM|EPOLLWRNORM`），
  后果是**永远报可读可写**，epoll 会立刻返回并忙轮询。所以只要设备有"数据未就绪"状态，
  就必须实现 `.poll`。

---

## 3. fasync / SIGIO

### 3.1 数据结构

```c
/* include/linux/fs.h:1083-1090 */
struct fasync_struct {
	rwlock_t		fa_lock;
	int			magic;
	int			fa_fd;
	struct fasync_struct	*fa_next;	/* singly linked list */
	struct file		*fa_file;
	struct rcu_head		fa_rcu;
};
```

要点：

- 它是一个**单链表**，链表头就是驱动 `struct sensor_dev` 里的 `struct fasync_struct *fasync;`。
- 每个节点绑定一个 `struct file *fa_file` 和它的 `fa_fd`；节点由 `fasync_helper()` 分配/摘除，
  **驱动不参与内存管理**。
- 节点用 RCU + `rwlock_t fa_lock` 做并发保护：摘除走 `call_rcu()`（`fs/fcntl.c:872`），
  发送路径在 `rcu_read_lock()` 下遍历（`fs/fcntl.c:1004-1013`）。
- `magic` 用于自检，不匹配会打印 `kill_fasync: bad magic number`（`fs/fcntl.c:985-988`）——
  这也是"链表头没有正确初始化/被踩内存"的典型症状。

### 3.2 用户态三步（缺一不可）

```c
sigemptyset(&mask);
sigaddset(&mask, SIGIO);
sigprocmask(SIG_BLOCK, &mask, &oldmask);   /* ① 先阻塞，sigtimedwait 才能同步等 */

fcntl(fd, F_SETOWN, getpid());             /* ② 目标是哪个进程/进程组 */
fcntl(fd, F_SETFL, flags | O_ASYNC);       /* ③ 打开异步开关（O_ASYNC == FASYNC） */

/* ... sigtimedwait(&mask, NULL, &ts) 等到信号 ... */

fcntl(fd, F_SETFL, flags & ~O_ASYNC);      /* ④ 清理 */
sigprocmask(SIG_SETMASK, &oldmask, NULL);
```

- **只做 ③ 不做 ②：收不到信号。** `F_SETOWN` 写的是 `file->f_owner.pid`，
  `send_sigio()` 开头就检查：`pid = fown->pid; if (!pid) goto out_unlock_fown;`
  （`fs/fcntl.c:776-778`）。没有 owner，信号无处投递，链路静默失败。
- **标准信号不排队**：`SIGIO` 默认是标准信号，多次发送会合并成一个 pending。
  所以本项目的判定是 `sigio_count >= 1`（"收到过"），不是"N 次"。
  要精确计数需要 `F_SETSIG` 改成实时信号（`SIGRTMIN+n`）——本阶段不做。
- `O_ASYNC` 在用户态头文件里就叫 `FASYNC`；`setfl()` 里比较的位就是 `FASYNC`
  （`fs/fcntl.c:73`）。

### 3.3 `F_SETOWN` / `F_SETFL` 的内核路径

```text
fcntl(fd, F_SETOWN, pid)
  → do_fcntl() case F_SETOWN                       fs/fcntl.c:370-371
    → f_setown(filp, who, 1)                       fs/fcntl.c:109
       who >= 0 → PIDTYPE_TGID（进程组）；who < 0 → PIDTYPE_PGID
       find_vpid(who) → __f_setown() 写 f_owner.pid / f_owner.pid_type

fcntl(fd, F_SETFL, arg)
  → do_fcntl() case F_SETFL                        fs/fcntl.c:333-334
    → setfl(fd, filp, arg)                         fs/fcntl.c:37
       if (((arg ^ filp->f_flags) & FASYNC) && filp->f_op->fasync)
           error = filp->f_op->fasync(fd, filp, (arg & FASYNC) != 0);  fs/fcntl.c:73-74
                → sensor_fasync() → fasync_helper(on=1/0)
       filp->f_flags = (arg & SETFL_MASK) | ...   fs/fcntl.c:81-83
```

注意两个细节：

1. **FASYNC 位是由框架（`fasync_helper`）设置/清除的**，不是 `setfl` 直接写的：
   `fasync_insert_entry()` 里 `filp->f_flags |= FASYNC`（`fs/fcntl.c:926`），
   `fasync_remove_entry()` 里 `filp->f_flags &= ~FASYNC`（`fs/fcntl.c:873`）。
   `setfl` 只在"FASYNC 位发生变化"时才调用 `->fasync()`，避免重复登记。
2. **`setfl()` 先调用 `->fasync()`、后更新 `f_flags`**。所以 `sensor_fasync` 里读到的
   `file->f_flags` 还是旧值，**不能**靠它判断开关状态；`on` 参数才是唯一可信输入。

### 3.4 `fasync_helper` / `kill_fasync` 的调用约束

| 函数 | 调用上下文 | 能否睡眠 | 为什么 |
|---|---|---|---|
| `fasync_helper(on!=0)` | 进程上下文（fcntl 系统调用） | **会睡眠** | `fasync_add_entry()` → `fasync_alloc()` → `kmem_cache_alloc(GFP_KERNEL)`（`fs/fcntl.c:882-884`、`fs/fcntl.c:938-942`） |
| `fasync_helper(on==0)` | 进程上下文（fcntl 或驱动的 release） | 不睡眠 | `fasync_remove_entry()` 只持自旋锁 + `call_rcu()`（`fs/fcntl.c:856-878`） |
| `kill_fasync()` | **原子上下文可用** | 不睡眠 | `rcu_read_lock()` + `read_lock_irqsave(&fa->fa_lock)` + `send_sigio()`（`fs/fcntl.c:979-1015`），全程无分配、无阻塞 |
| `send_sigio()` | 原子上下文可用 | 不睡眠 | `read_lock_irqsave(&fown->lock)` + `pid_task()` + `do_send_sig_info()`（`fs/fcntl.c:765-796`） |

`kill_fasync()` 开头还有一个"无锁快路径"（`fs/fcntl.c:1006-1008`）：

```c
	/* First a quick test without locking: usually the list is empty. */
	if (*fp) { ... }
```

这解释了为什么"没注册异步通知"时每 500ms 调一次 `kill_fasync` 的代价可以忽略。

### 3.5 为什么不能在硬中断上下文做"这些事"

这条要分清**三件事**，面试时区分清楚会明显加分（很多人会答错）：

1. **驱动读数据（本驱动是 I2C 传输）**：`i2c_smbus_read_word_data()` 可能睡眠
   （等待传输完成、可能 DMA）。**硬中断上半部绝对不行** → 必须用
   `request_threaded_irq()`，上半部只登记并返回 `IRQ_WAKE_THREAD`，真正的 I2C 读
   在线程化下半部（内核线程上下文）执行。
2. **注册异步通知 `fasync_helper(on=1)`**：内部 `kmem_cache_alloc(GFP_KERNEL)`
   **可以睡眠**，不能在任何中断上下文调用。实际上它的调用者永远是
   `fcntl()` 系统调用（`setfl()`），从设计上就不会进中断。
3. **发信号 `kill_fasync()`**：**它本身在硬中断上下文是合法的**——只有 RCU 读锁、
   自旋锁和信号投递，不睡眠、不分配内存。网络上常见答法"kill_fasync 会睡眠所以不能
   中断里调"是**错的**。

那么本驱动为什么把 `kill_fasync` 放在线程化下半部？**因为它的前置动作（I2C 读 +
更新 `latest` + `wake_up`）必须在线程化下半部**，`kill_fasync` 只是跟着数据路径走。
换句话说：约束来自"数据从哪来"，不来自 `kill_fasync` 本身。

再补一条顺序约束：`kill_fasync` 必须在**数据已经更新、阻塞者已唤醒之后**调用。
否则用户态收到 SIGIO 立刻来 `read()`，可能读到旧样本——表现为"信号来了但读不到新数据"。

### 3.6 `release()` 里的清理顺序（use-after-free 陷阱）

```c
static int sensor_release(struct inode *inode, struct file *file)
{
	struct sensor_file *sf = file->private_data;
	struct sensor_dev *sd = sf->sd;

	sensor_fasync(-1, file, 0);   /* 必须先做 */
	kfree(sf);                    /* 之后才能释放 */
	file->private_data = NULL;
	...
}
```

- `fasync_remove_entry()` 要遍历链表并比对 `fa->fa_file != filp`（`fs/fcntl.c:864`），
  它**不访问驱动的 `private_data`**；但 `sensor_fasync()` 需要 `sf` 才能拿到
  `&sf->sd->fasync`。所以 `kfree(sf)` 之后再调用就是确定的 use-after-free。
- 另外，**不显式摘除会怎样**：`fa_file` 指向一个马上要被释放的 `struct file`
  （`close()` 后 `filp` 引用计数归零被回收），后续 `kill_fasync` 遍历到它就会
  访问已释放的 `f_owner`。这是真实驱动里常见的内存破坏来源。
- 摘除是幂等的：没注册过时 `sd->fasync == NULL`，`fasync_remove_entry()` 的
  `for` 循环一次都不执行，返回 0。

---

## 4. 本驱动为什么这么写

### 4.1 per-open `last_seq` 的必要性（用"不做会怎样"论证）

契约给的判定是：

```c
static bool sensor_has_new_sample(struct sensor_file *sf)
{
	return READ_ONCE(sf->sd->latest.seq) != sf->last_seq;
}
```

`struct sensor_file` 在 `sensor_open()` 里 `kzalloc`，`file->private_data = sf`，
`release()` 里 `kfree`。**如果把基准从 `sf->last_seq` 换成全局量，会怎样？**

先看清 `.poll` 的语义：它回答的是"**这个调用者**现在能不能无阻塞读到数据"。
"能读到"意味着 `read()` 会返回新数据并推进某个状态。而 `read()` 是**文件描述符操作**，
它推进的状态只可能属于该 fd。因此"有没有新样本"这个谓词**天然是 per-fd 的**；
用全局量代替它，等于偷换了语义。

具体后果有三条，都能复现：

1. **poll 退化成"曾经有过数据就永远可读" → epoll 忙轮询。**
   最朴素的错误实现是 `if (sd->latest.seq != 0) return EPOLLIN;`。
   第一次采样后这个条件恒真，`epoll_wait` 每次都立即返回，用户态 3 秒内被唤醒数千次，
   CPU 打满。实测的对照数据见第 6.4 节。
2. **多进程/多 fd 互相干扰。**
   如果用一个"全局已消费序号"，进程 A 读走后，进程 B 再 `poll` 就不再报可读，
   可 B 自己一个样本都没读——数据被 A 吃掉了。反之若不动全局量，
   同一个样本会被每个新 fd 重复上报（"读一次得同一份数据"）。
   本项目契约要求"新样本语义在每个 open 的 fd 上独立"，就是要在两个极端之间取正确点。
3. **`read()` 自己也无法定义"新"。**
   阶段 01 之前的实现用"进入 `read()` 那一刻的全局 seq"当基准；它无法回答
   "这个 fd 上次读到哪里"，非阻塞路径的判断会退化（详见
   `docs/impl/01-io-models-实现记录.md` 第一节）。

改成 per-open 后：样本被本 fd 读走 → `sf->last_seq = snap.seq` → 下一次 `.poll` 返回 0。
**电平触发语义因此成立**；多进程各开各的 fd 时 `sf` 互不共享，自然互不干扰。

> 已知边界（与实现记录第六节一致）：`SENSOR_IOC_RESET` 会把 `latest.seq` 归零，
> 此时一个已经读过样本的 fd 会认为"有新样本"，再读一次会拿到 `seq=0` 的旧样本。
> 契约把判定明确写成 `latest.seq != sf->last_seq`，当前实现与契约一致。
> 彻底解决需要引入"永不回退的代号"（例如 `u64 sample_gen`），属于接口取舍。

### 4.2 `latest.seq` 的 `READ_ONCE`

```c
return READ_ONCE(sf->sd->latest.seq) != sf->last_seq;
```

为什么写 `READ_ONCE`：

- **写者**是线程化中断下半部，在 `mutex_lock(&sd->lock)` 保护下写 `sd->latest`；
  **读者**是 `.poll`，**不持锁**。这是数据竞争（data race）场景，`READ_ONCE` 是内核
  对"无锁单调读"的标准表达方式。
- 它把这次访问**显式声明为一次不可拆分、不可合并、不可省略的原子加载**：只要在 `wait_event_*`
  的循环里重复求值 `condition`，每次都必须是真实的内存读取，不能复用上一轮寄存器里的值。
  这是内核处理无锁共享数据的强制约定。
- 它保证访问不会被拆成多次读（`u32` 在 arm64 对齐访问本来原子，`READ_ONCE` 是
  **可移植性的显式保证**；换到某些架构/某些宽度就有撕裂风险）。
- 它**不**提供对 `latest` 其余字段的可见性保证。真正的样本快照在 `sensor_read()` 里
  用 `mutex_lock(&sd->lock)` 取整份 `struct sensor_sample`，并同时更新
  `sf->last_seq = snap.seq`——保证"读到 seq 和读到 temp 是同一次采样"。
- 语义上 `.poll` 的读是**提示性**的：即使读到略旧的值判断为"无数据"，
  由于 `poll_wait()` 已经先登记、且其 `smp_mb()` 保证入队对唤醒方可见，
  新数据产生的 `wake_up` 仍会被系统调用层感知（`pwq->triggered` 或 `ep_poll_callback`
  已入 `rdllist`），不会漏事件。

### 4.3 waitqueue 丢唤醒问题与 `wait_event` 如何规避

**经典错误写法（伪代码）：**

```c
if (!has_data()) {
	add_wait_queue(&wq, &wait);        /* ① 入队 */
	set_current_state(TASK_INTERRUPTIBLE);  /* ② 置状态 */
	if (!has_data())                   /* ③ 复查 */
		schedule();                /* ④ 睡 */
	remove_wait_queue(&wq, &wait);
}
```

丢唤醒发生在 ①/②/③ 与"数据到达 + `wake_up()`"的交错上：

```text
CPU0（等待者）                       CPU1（数据到达）
③ has_data() == false
                                     has_data = true;
                                     wake_up(&wq);   ← 队列上还没有 CPU0
② set_current_state(TASK_INTERRUPTIBLE);
④ schedule();  ← 永远睡下去（直到超时），数据明明已经到了
```

即使把 ② 提到 ① 前面也不够——关键是"入队"与"置状态"不是原子的，
而 `wake_up` 只唤醒**已经在队列上、且状态非 RUNNING** 的项。

**`wait_event_*` 宏的做法**（`include/linux/wait.h:304-327`）：

```c
#define ___wait_event(wq_head, condition, state, exclusive, ret, cmd)		\
({										\
	struct wait_queue_entry __wq_entry;					\
	long __ret = ret;							\
	init_wait_entry(&__wq_entry, exclusive ? WQ_FLAG_EXCLUSIVE : 0);	\
	for (;;) {								\
		long __int = prepare_to_wait_event(&wq_head, &__wq_entry, state);\
		if (condition)							\
			break;							\
		if (___wait_is_interruptible(state) && __int) {			\
			__ret = __int;						\
			goto __out;						\
		}								\
		cmd;								\
	}									\
	finish_wait(&wq_head, &__wq_entry);					\
__out:	__ret;									\
})
```

两个关键设计：

1. **入队 + 置状态在同一把自旋锁内完成**。`prepare_to_wait_event()`
   （`kernel/sched/wait.c:310-343`）：

   ```c
   spin_lock_irqsave(&wq_head->lock, flags);
   ...
   __add_wait_queue[_entry_tail](wq_head, wq_entry);   /* 入队 */
   set_current_state(state);                            /* 置状态 */
   spin_unlock_irqrestore(&wq_head->lock, flags);
   ```

   而 `wake_up` → `__wake_up_common_lock()` 也要拿同一把 `wq_head->lock`
   （`kernel/sched/wait.c:124-140`）。于是顺序只可能是两种之一：

   - 唤醒者先拿到锁：它遍历队列时看不到等待者，但等待者随后必然在**锁内**完成
     "入队 + 置状态 + 复查条件"——**条件已经为真，`break` 掉，根本不睡**。
   - 等待者先拿到锁：它已经挂在队列上且状态已置，唤醒者随后一定能看到并唤醒它。

   **不存在"条件已真、入队已完成、状态还没置、就被唤醒"的窗口。**

2. **循环复查条件**。`for(;;)` 每轮都重新执行 `condition`，所以被唤醒后（可能是虚假唤醒、
   或者是别的等待者抢走了数据）会重新判断，不会带着错误假设返回。

另外一个容易忽略的细节：`wait_event_*` 用 `init_wait_entry()` 把 entry 的
`func` 设为 `autoremove_wake_function`（`kernel/sched/wait.c:301-306`、`kernel/sched/wait.c:422`），
被唤醒时**自动从队列摘除**，与 `finish_wait()` 的 `list_empty_careful()` 判断配合
（`kernel/sched/wait.c:396-419`），避免重复摘除。

**返回值语义（必须记准，面试常错）：**

| 返回值 | 含义 |
|---|---|
| `> 0` | 条件在超时前满足，值为剩余 jiffies |
| `0` | 超时且条件仍未满足 → 本驱动返回 `-ETIMEDOUT` |
| `< 0` | 被信号打断（`-ERESTARTSYS`）→ 本驱动返回 `-ERESTARTSYS` |

### 4.4 poll 与 read 共享判定函数

```c
static bool sensor_has_new_sample(struct sensor_file *sf);   /* read/poll 共用 */
```

理由是**防语义漂移**：`read` 与 `poll` 是同一个问题（"这个 fd 现在有没有未读样本"）
的两条使用路径。如果各写一份判断，最典型的 bug 是：

- 改了 `read` 的条件（比如加上"非阻塞时也算"），忘了改 `poll` →
  用户态看到 `EPOLLIN`，`read` 却阻塞或返回 `EAGAIN`；
- 反向漂移 → `read` 能读到、`poll` 却永远报 0 → 依赖 epoll 的程序饿死。

共享一个函数后，"有没有数据"只有一个定义，改一处即全局一致。这也是本阶段契约
第 2 条强制要求 `sensor_has_new_sample()` 存在的原因。

### 4.5 `kill_fasync` 的位置与顺序

```c
	mutex_unlock(&sd->lock);              /* 先放锁 */
	wake_up_interruptible(&sd->wq);       /* 再唤醒阻塞读者 */
	kill_fasync(&sd->fasync, SIGIO, POLL_IN);  /* 最后发异步通知 */
```

- **在锁外**：`wake_up` 放锁外可避免被唤醒者立刻去抢锁空转；`kill_fasync` 内部要走
  RCU + 自旋锁 + 信号投递，更不应在 `sd->lock` 里做。
- **在 `wake_up` 之后**：保证"信号发出时数据一定已就绪"。
- **在线程化下半部**：因为数据（I2C 读）只能在这里做（见 3.5）。

---

## 5. 面试问答（14 问）

**Q1：`.poll` 里为什么必须调用 `poll_wait()`？不调会怎样？**

`poll_wait()` 是把"本文件的等待项"挂到驱动等待队列上的唯一入口（`include/linux/poll.h:42`）。
系统调用层（`do_poll`/`ep_poll`）靠这个登记才能在数据到达时被唤醒。
不调用：select/poll 会一直睡到超时；epoll 更隐蔽——`epoll_ctl(ADD)` 时没挂上
`ep_poll_callback`，之后 `wake_up_interruptible(&sd->wq)` 在队列上找不到任何 epoll 回调，
事件永远进不了 `rdllist`，`epoll_wait` 只能超时返回 0。这是**丢失事件**，不是性能问题。

**Q2：epoll 在驱动里对应什么实现？驱动要写 `epoll` 相关代码吗？**

不写。epoll 完全复用 `.poll` + `poll_wait()` 这一套 fops 接口，驱动感知不到
调用方是 select、poll 还是 epoll。区别只在 `poll_table._qproc` 被换成了谁：

- select/poll：`__pollwait`（`fs/select.c:219`），每次系统调用临时登记、返回时 `poll_freewait` 摘除；
- epoll ADD：`ep_ptable_queue_proc`（`fs/eventpoll.c:1244`），分配常驻 `eppoll_entry`，
  回调是 `ep_poll_callback`；
- epoll_wait 复检：`NULL`（`fs/eventpoll.c:1665`），`poll_wait` 变成空操作。

所以"epoll 在驱动侧的实现" = "正确实现 `.poll`（`poll_wait` + 电平语义）"。

**Q3：`wait_event_*` 如何避免丢唤醒？**

`prepare_to_wait_event()` 在 `wq_head->lock` 内原子完成"入队 + 置 `TASK_*` 状态"
（`kernel/sched/wait.c:310-343`），`wake_up` 用同一把锁遍历（`kernel/sched/wait.c:124-140`）。
两者互斥，所以不存在"条件已真但等待者还没就绪、唤醒被漏掉"的窗口；
入队后 `___wait_event` 还会再复查一次 `condition`（`include/linux/wait.h:313-315`），
把"入队前一刻条件变真"的情况也兜住。手册写法（先 `add_wait_queue` 再手工
`set_current_state`）才有窗口。

**Q4：`O_NONBLOCK` 是 VFS 负责还是驱动负责？**

**驱动负责。** `vfs_read()` 里没有任何 `O_NONBLOCK` 判断（`fs/read_write.c:450-483`），
它只把 `file` 传给 `->read`。驱动必须自己检查 `file->f_flags & O_NONBLOCK`
并在无数据时返回 `-EAGAIN`。对照实现：`fs/pipe.c:345`、`fs/eventfd.c:236` 都是
`(file->f_flags & O_NONBLOCK) || (iocb->ki_flags & IOCB_NOWAIT)` 两个条件一起判。
`IOCB_NOWAIT` 来自 `preadv2` 的 `RWF_NOWAIT`，**不是** `O_NONBLOCK` 推导出来的
（`include/linux/fs.h:3288-3300`）。

**Q5：`poll` 的返回值怎么约定？`EPOLLERR` 呢？**

返回 `__poll_t` 位掩码，不是负 errno。约定：

- 只报"当前真正成立的事件"，**必须电平语义**（每次调用重新回答）；
- `EPOLLERR`/`EPOLLHUP` **无论用户是否请求都要上报**，且一定会被返回；
- `EPOLLNVAL` 由内核填（非法 fd），驱动不填；
- 无事件返回 0。
本驱动返回 `EPOLLIN | EPOLLRDNORM`（`POLLIN|POLLRDNORM`，字符设备可读的通用写法）。

**Q6：驱动怎么支持边沿触发（ET）？**

驱动**不需要也不能**支持 ET。驱动只提供电平语义；LT/ET 是 `fs/eventpoll.c` 在
用户态接口层实现的：`ep_send_events()` 里，LT 会把 `epi` 重新塞回 `rdllist`，
使下一次 `epoll_wait` 复查 `.poll`；ET 不塞回，只等下一次 `ep_poll_callback`
（`fs/eventpoll.c:1719-1733`）。ET 要求用户态读到 `EAGAIN`，依赖的正是驱动
"无数据返回 `-EAGAIN`、`.poll` 无数据返回 0"。

**Q7：`fasync_helper` 和 `kill_fasync` 能不能在中断上下文调用？**

`fasync_helper(on=1)` **不能**：它走 `fasync_add_entry()` → `kmem_cache_alloc(GFP_KERNEL)`，
会睡眠（`fs/fcntl.c:882`、`fs/fcntl.c:942`）；它的调用者本来就只在 `fcntl()` 里。
`kill_fasync()` **可以**：只有 `rcu_read_lock` + `read_lock_irqsave` + `send_sigio`，
无睡眠无分配（`fs/fcntl.c:1004-1015`）。
本驱动把它放在线程化下半部，不是因为 `kill_fasync` 有上下文限制，
而是因为它的前置数据来自 I2C（会睡眠），整条数据路径都在线程化下半部。

**Q8：`F_SETOWN` 和 `F_SETFL O_ASYNC` 各自作用是什么？只做后者会怎样？**

`F_SETOWN` 通过 `f_setown()` 写 `file->f_owner.pid`（`fs/fcntl.c:109-138`），决定信号发给谁；
`F_SETFL` 的 `O_ASYNC`（即 `FASYNC`）经 `setfl()` 调用 `->fasync(on=1)`（`fs/fcntl.c:73-74`），
把本 `file` 挂进驱动的 fasync 链表并置 `filp->f_flags |= FASYNC`。
只做 `O_ASYNC`：登记是成功的，但 `send_sigio()` 开头 `if (!fown->pid) goto out;`
（`fs/fcntl.c:776-778`）直接返回，信号静默丢失。

**Q9：`release()` 里为什么必须调 `fasync(-1, file, 0)`，且必须在 `kfree` 之前？**

因为 `close()` 之后 `struct file` 会被回收，而 fasync 链表节点里保存着 `fa_file`
指向它；不摘除的话，之后每次 `kill_fasync()` 都会访问已释放的 `file->f_owner`。
必须在 `kfree(sf)` 之前，是因为 `sensor_fasync()` 需要经 `file->private_data`
拿到 `sf->sd->fasync` 链表头，`sf` 释放后再取就是 use-after-free。

**Q10：为什么 `kill_fasync` 要放在数据更新和 `wake_up` 之后？**

用户态收到 SIGIO 后的第一反应通常是立刻 `read()`。如果 `kill_fasync` 先于数据更新，
就会出现"信号到了但读到旧样本/读到 `EAGAIN`"的竞态。顺序必须是：
**更新 `latest` → `wake_up`（唤醒阻塞者）→ `kill_fasync`（通知异步者）**。

**Q11：per-open 的 `last_seq` 为什么必要？不用会怎样？**

`.poll` 回答的是"**这个调用者**现在能否无阻塞读到数据"，而 `read` 推进的状态属于具体 fd，
所以判定必然 per-fd。用全局量会让 `.poll` 退化成"曾经有过数据就永远可读"，
`epoll_wait` 立即返回造成忙轮询（实测对照：正确实现 3 秒唤醒 7 次，错误实现数千次）；
多进程/多 fd 还会互相偷数据或重复上报同一份样本。

**Q12：`.poll` 里能加锁吗？能睡眠吗？**

不能睡眠——`.poll` 是被多路复用系统调用的循环调用的"一次性状态查询"，
睡眠由系统调用层负责；在 `epoll_ctl(ADD)` 的路径上还持有 `ep->mtx`。
加睡眠锁（mutex）是错的；持普通自旋锁虽语法可行，但没有必要：
`latest.seq` 是 4 字节，用 `READ_ONCE` 读即可，完整快照在 `read()` 里用 `sd->lock` 取。
持锁再 `poll_wait()` 会把"登记等待项"放进临界区，增加锁竞争和潜在死锁风险。

**Q13（加分题）：`.poll` 里 `wait->_qproc == NULL` 时怎么办？**

仍然必须正常计算并返回事件掩码——这只说明调用方不想登记等待项
（典型是 `epoll_wait` 的复检 `fs/eventpoll.c:1665`，或 `poll()` 超时为 0）。
`poll_wait()` 自己会跳过登记（`include/linux/poll.h:44`），驱动不需要写
`if (wait) poll_wait(...)` 之类判断；唯一不能做的是"因为没登记就跳过状态计算"。

**Q14（加分题）：`wake_up_interruptible` 与 `wake_up_interruptible_poll` 有何区别？**

`wake_up_interruptible` 传递的 `key == NULL`；`wake_up_interruptible_poll(wq, key)`
带上事件掩码（如 `EPOLLIN`）。`ep_poll_callback` 会检查
`if (pollflags && !(pollflags & epi->event.events)) goto out_unlock;`
（`fs/eventpoll.c:1161-1162`），即带 `key` 的唤醒可以只唤起关心该事件的 waiter。
本驱动用 `wake_up_interruptible`（`key == NULL`），`pollflags == 0`，
这个过滤被跳过，所有 waiter 都会被唤起（`ep_poll_callback` 内对 `waitqueue_active(&ep->wq)`
仍会正常 `wake_up(&ep->wq)`）。对本设备"只有可读一种事件"的场景，两种写法都正确。

---

## 6. 亲手验证

### 6.1 一键复现（推荐，与 `docs/10-开发与验证守则.md` 一致）

```bash
cd /Users/lucien/workspace/self-study/projects/linux-char-driver

# ① 构建模块 + 用户态程序 + initramfs（一次构建可跑任意阶段）
limactl shell dev bash -c 'cd ~ && TEST=01-io-models bash /Users/lucien/workspace/self-study/projects/linux-char-driver/scripts/13-vm-fast-cycle.sh'

# ② 产物拷回宿主
bash scripts/21-macos-sync-artifacts.sh

# ③ 阶段测试（QEMU，TCG + cortex-a72）
bash scripts/22-macos-run-test.sh 01-io-models 180

# ④ 回归
bash scripts/22-macos-run-test.sh smoke 180
```

### 6.2 逐条对照证据（实测，来自 `logs/20260914-025014-01-io-models.log`）

程序原始输出：

```text
[IO] device=/dev/sensor0
[IO] blocking_read=ok
[IO] blocking_sample=seq=1 temp=25.100C irq=1 t=1288392992 ns
[IO] nonblock_eagain=1
[IO] epoll_wakeups=7
[IO] epoll_reads=7 elapsed_ms=3001
[IO] epoll_wait_ok=1
[IO] sigio_count=1
[IO] checks blocking=1 nonblock=1 epoll=1 level_triggered=1 sigio=1
[IO] OVERALL=PASS
```

脚本判定：

```text
[CHECK:PASS] 阻塞 read 成功返回样本
[CHECK:PASS] 非阻塞 read 返回 EAGAIN（实测 1 次）
[CHECK:PASS] epoll 在 3 秒窗口内报告可读
[CHECK:PASS] io_models_test 退出码为 0
[CHECK:PASS] poll 为电平触发、无忙轮询（3 秒内仅唤醒 7 次）
[CHECK:PASS] SIGIO 异步通知到达（实测 1 次）
[CHECK:PASS] 程序整体结论为 PASS
[CHECK:PASS] ftrace 捕获到 kill_fasync 调用（实测 8 次）
[CHECK:PASS] dmesg 无 WARNING/Call trace
[TEST:END] 01-io-models pass=9 fail=0
```

**每条证据对应的机制：**

| 观测 | 预期 | 证明了什么 |
|---|---|---|
| `blocking_read=ok` + `seq=1 temp=25.100C` | read 阻塞后返回一行样本 | `wait_event_interruptible_timeout` 被中断下半部 `wake_up` 唤醒 |
| `nonblock_eagain=1` | 无新样本时返回 `EAGAIN` | 驱动检查 `f_flags & O_NONBLOCK`，VFS 未代劳 |
| `epoll_wait_ok=1`、`epoll_reads=7` | 在 3s 内可读且真的读到数据 | `poll_wait` 登记成功 + `EPOLLIN` 掩码正确 |
| `epoll_wakeups=7`（阈值 ≤12） | 500ms × 3s ≈ 6~7 次 | 电平触发正确：消费后不再上报，非忙轮询 |
| `sigio_count=1` | ≥1 | `F_SETOWN`+`O_ASYNC`+`fasync_helper`+`kill_fasync` 四环全通 |
| ftrace `kill_fasync` 8 次 | >0 | 直接证据：中断下半部确实调用了它 |

`kill_fasync` 与 `wake_up` 的调用来自同一条路径，dmesg 可看到 open/close 与采样节奏一致：

```text
[    1.288814] i2c 0-0048: opened (count=1)
[    1.316675] i2c 0-0048: closed
[    1.316974] i2c 0-0048: opened (count=2)
[    1.317165] i2c 0-0048: closed
[    1.317384] i2c 0-0048: opened (count=3)
[    4.320168] i2c 0-0048: closed
[    4.322176] i2c 0-0048: opened (count=4)
[    4.819482] i2c 0-0048: closed
```

（count=3 那次就是 epoll 的 3 秒窗口，count=4 是 SIGIO 测试。）

### 6.3 在 QEMU 里手动验证

先按 `scripts/04-run-qemu-vm.sh` 的交互模式启动（产物齐全时可用）：

```bash
# 在虚拟机里（limactl shell dev）：
INTERACTIVE=1 bash <项目路径>/scripts/04-run-qemu-vm.sh
```

进入 shell 后：

```sh
# 1) 加载模块（顺序不能反：i2c-stub 提供虚拟总线 0）
insmod /lib/modules/6.6.156/i2c-stub.ko chip_addr=0x48
insmod /lib/modules/6.6.156/sensor_char.ko
ls -l /dev/sensor0

# 2) 阻塞 read：会等到下一个采样周期（500ms）才返回一行
cat /dev/sensor0
# 预期（ctrl-c 退出循环）：
#   seq=25 temp=25.100C irq=25 t=... ns
#   seq=26 temp=25.000C irq=26 t=... ns
#   ...

# 3) 非阻塞行为：busybox 的 dd 不能直接设 O_NONBLOCK，
#    用自带程序验证（它内部 open(O_NONBLOCK) 并连读到 EAGAIN）
/bin/io_models_test
# 预期：OVERALL=PASS，且 nonblock_eagain >= 1

# 4) ftrace 直接证据（注意：必须在切回 nop 之前读 trace）
mount -t debugfs none /sys/kernel/debug 2>/dev/null
TR=/sys/kernel/debug/tracing
echo 0 > $TR/tracing_on
echo function > $TR/current_tracer
echo kill_fasync > $TR/set_ftrace_filter
echo 1 > $TR/tracing_on
/bin/io_models_test > /tmp/io.txt 2>&1
echo 0 > $TR/tracing_on
grep -c kill_fasync $TR/trace      # 预期 >0（实测 8）
echo nop > $TR/current_tracer      # 切回 nop 会清空缓冲区，所以上一行必须在前

# 5) 确认无内核告警
dmesg | grep -E 'WARNING:|Call trace:'   # 预期无输出
```

> 注意：`echo nop > current_tracer` 会清空 trace 环形缓冲区。
> 如果在切回 `nop` **之后**才 `grep trace`，永远读到 0——
> 这是本阶段踩过的坑，见 `docs/impl/01-io-models-实现记录.md` 第四节问题 1。

### 6.4 反例实验：把 `.poll` 改成"永远可读"（证明 7 次不是随便写的）

这是验证"电平触发语义确实由 per-open `last_seq` 撑起来"的最直接方式。
在 `driver/sensor_char.c` 里把 `sensor_poll()` 临时改成：

```c
static __poll_t sensor_poll(struct file *file, poll_table *wait)
{
	struct sensor_file *sf = file->private_data;

	poll_wait(file, &sf->sd->wq, wait);

	if (READ_ONCE(sf->sd->latest.seq) != 0)   /* 错误：全局量，不是本 fd 消费位置 */
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}
```

重跑 6.1 的第 ①~③ 步，**预期**（未验证：本节只给出实验方法与预期，实际数值需自行跑出后记录）：

- `[IO] epoll_wakeups` 会从 7 暴涨到数百~数千（等于"3 秒内 `epoll_wait` 返回次数"）；
- 阶段脚本的"poll 为电平触发、无忙轮询（≤12）"变成 `[CHECK:FAIL]`；
- `[IO] OVERALL=FAIL`，退出码非 0。

实验后必须把代码还原（`git checkout driver/sensor_char.c`），并重跑
`bash scripts/22-macos-run-test.sh smoke` 确认回归绿。

### 6.5 复现的边界

- QEMU 必须用 `-accel tcg -cpu cortex-a72`；`-accel hvf -cpu host` 下 ftrace 会挂死
  （见 `docs/10-开发与验证守则.md` 第九节）。
- 测试框架通过内核命令行 `test=<阶段名>` 选择脚本；一次构建可跑任意阶段，
  但**换阶段不需要重新构建**、**换代码必须重新构建**（模块烤进 initramfs）。
- `sigio_count` 通常为 1：标准信号不排队。不要写成"必须等于采样次数"。

---

## 附录 A：本文引用的内核符号速查

| 符号 | 位置 | 作用 |
|---|---|---|
| `struct poll_table_struct` | `include/linux/poll.h:37-40` | `_qproc` + `_key` |
| `poll_wait()` | `include/linux/poll.h:42-58` | 登记等待项，不睡眠，含 `smp_mb()` |
| `vfs_poll()` | `include/linux/poll.h:88-92` | 无 `.poll` 时返回 `DEFAULT_POLLMASK` |
| `struct poll_table_entry` / `poll_wqueues` | `include/linux/poll.h:95-113` | 系统调用栈上的临时 entry |
| `poll_initwait()` | `fs/select.c:119-128` | 装填 `__pollwait` |
| `__pollwait()` | `fs/select.c:219-232` | 分配 entry + `add_wait_queue` |
| `pollwake()` / `__pollwake()` | `fs/select.c:182-217` | 置 `pwq->triggered` + 唤醒任务 |
| `poll_freewait()` | `fs/select.c:136-156` | 摘除全部临时 entry |
| `do_poll()` | `fs/select.c:882-968` | select/poll 的 "poll → 无则睡 → 再 poll" 循环 |
| `do_sys_poll()` | `fs/select.c:971-1030` | `poll(2)` 入口 |
| `ep_insert()` | `fs/eventpoll.c:1450-1571` | `EPOLL_CTL_ADD`，此调用驱动 `.poll` |
| `ep_ptable_queue_proc()` | `fs/eventpoll.c:1244-1272` | 常驻注册 + 回调 `ep_poll_callback` |
| `ep_poll_callback()` | `fs/eventpoll.c:1133-1238` | 中断唤醒路径：epi 入 `rdllist` |
| `ep_item_poll()` | `fs/eventpoll.c:926-947` | 对 fd 调 `vfs_poll` |
| `ep_send_events()` | `fs/eventpoll.c:1649-1739` | 投递事件；LT 重新入链、ET 不入 |
| `ep_poll()` | `fs/eventpoll.c:1798` 起 | `epoll_wait` 主循环 |
| `setfl()` | `fs/fcntl.c:37-87` | `F_SETFL`，触发 `->fasync()` |
| `f_setown()` | `fs/fcntl.c:109-138` | `F_SETOWN` |
| `fasync_remove_entry()` | `fs/fcntl.c:856-878` | 摘除 + 清 `FASYNC` + `call_rcu` |
| `fasync_alloc()` | `fs/fcntl.c:882-884` | `kmem_cache_alloc(GFP_KERNEL)` |
| `fasync_insert_entry()` | `fs/fcntl.c:904-934` | 挂链 + 置 `FASYNC` |
| `fasync_helper()` | `fs/fcntl.c:967-974` | `on==0` 摘除，否则登记 |
| `kill_fasync_rcu()` | `fs/fcntl.c:979-1002` | 遍历链表 + `send_sigio` |
| `kill_fasync()` | `fs/fcntl.c:1004-1015` | 无锁快路径 + RCU |
| `send_sigio()` | `fs/fcntl.c:765-794` | 按 `f_owner.pid` 投递 |
| `struct fasync_struct` | `include/linux/fs.h:1083-1090` | fasync 链表节点 |
| `___wait_event()` | `include/linux/wait.h:304-327` | `wait_event` 展开模板 |
| `wait_event_interruptible_timeout()` | `include/linux/wait.h:534-542` | 本驱动使用的等待宏 |
| `prepare_to_wait_event()` | `kernel/sched/wait.c:310-344` | 锁内入队 + 置状态 |
| `finish_wait()` | `kernel/sched/wait.c:396-420` | 退出等待清理 |
| `autoremove_wake_function()` | `kernel/sched/wait.c:422-431` | 唤醒时自动摘除 |
| `vfs_read()` | `fs/read_write.c:450-483` | 不处理 `O_NONBLOCK` |

## 附录 B：易错 errno / 返回值

| 场景 | 正确返回 | 常见错误 | 为什么错 |
|---|---|---|---|
| 非阻塞且无数据 | `-EAGAIN` | `0` | `read()==0` 被用户态解释为 EOF |
| 非阻塞且无数据 | `-EAGAIN` | `-EBUSY` | 约定用 `EAGAIN`；ET 模式也依赖它作为"读干净"的信号 |
| 阻塞等待超时 | `-ETIMEDOUT` | `0` | 同样是 EOF 歧义 |
| 被信号打断 | `-ERESTARTSYS` | `-EINTR` | 内核惯例交给上层决定是否重启系统调用 |
| 无数据时的 `.poll` | 返回 `0`（掩码） | 返回 `-EAGAIN` | `.poll` 不返回负 errno |
| 未知 ioctl | `-ENOTTY` | `-EINVAL` | 未实现命令的标准返回 |
| `poll_wait` 用错队列 | —— | 用 `file->f_ep_links` 等 | 必须用驱动自己的 `sd->wq` |
