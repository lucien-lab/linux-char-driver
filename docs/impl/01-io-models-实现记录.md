# 阶段 01 实现记录：四种 IO 模型

- 阶段目标：把字符设备接口从"只有阻塞 read"补全为 **阻塞 / 非阻塞 / poll·epoll / fasync·SIGIO**
  四种 IO 模型，并让"有没有新样本"的判定在**每个 open 的文件描述符上独立**。
- 最终结论：**阶段测试 9/9 PASS，回归 smoke 12/12 PASS**
  - 阶段日志：`logs/20260914-025014-01-io-models.log`
  - 回归日志：`logs/20260914-025021-smoke.log`

---

## 一、改动清单

| 文件 | 改动 | 说明 |
|---|---|---|
| `driver/sensor_char.c` | 新增 `struct sensor_file`、`sensor_has_new_sample()`、`sensor_poll()`、`sensor_fasync()`；重写 `sensor_open/release/read`；`struct sensor_dev` 增加 `fasync` 字段；中断下半部增加 `kill_fasync()`；fops 增加 `.poll/.fasync` | 核心实现 |
| `user/io_models_test.c` | 新增 | 用户态四种模型验证程序（输出 `[IO] 键=值`） |
| `tests/phases/01-io-models.sh` | 新增 | 阶段测试脚本（9 项检查） |
| `tests/runner/init.sh` | 修改 | **测试框架修复**：允许用内核命令行 `test=` 选择阶段脚本（原因见第四节） |
| `scripts/13-vm-fast-cycle.sh` | 修改 | 把全部阶段脚本打进 initramfs，支持阶段专属 `*.modules` 清单 |
| `scripts/22-macos-run-test.sh` | 修改 | 启动 QEMU 时把阶段名通过 `-append "... test=$TEST"` 传进内核 |

### 驱动侧关键点

**1) per-open 上下文（本阶段的地基）**

```c
struct sensor_file {
	struct sensor_dev	*sd;
	u32			last_seq;	/* 本 fd 已经消费到的样本序号 */
};
```

原来的实现把 `file->private_data` 直接指向全局的 `sensor_dev`，read() 用"进入函数那一刻的
全局 seq"当基准。这个写法有三个问题：

- 同一个 fd 连续两次 read 可能拿到**同一个样本**（第二次进入时基准已经变成刚读到的 seq，
  看似没问题，但只要中间没有新样本，非阻塞路径的判断就会退化）；
- 更严重的是 **poll 无法正确实现**：poll 的语义是"调用者此刻能否无阻塞读到数据"，
  这是**相对于调用者**的事实。若用全局最新序号判断，样本被读走后 poll 仍报可读 →
  `epoll_wait` 立即返回 → 用户态死循环空转（忙轮询，CPU 打满）。
- 多进程各开各的 fd 时互相干扰。

改成 per-open 之后，"有没有新样本" = `latest.seq != sf->last_seq`，
样本被本 fd 读走（`sf->last_seq = snap.seq`）之后 poll 自然回到 0，
**电平触发语义**才成立。测试里用"3 秒内 epoll 唤醒 7 次"量化验证了这一点
（错误的实现会是数千次）。

**2) 数据可用判定只写一份**

```c
static bool sensor_has_new_sample(struct sensor_file *sf)
{
	return READ_ONCE(sf->sd->latest.seq) != sf->last_seq;
}
```

read 与 poll 共用它，避免两条路径的语义漂移（这类漂移在真实驱动里很常见：
read 改了判断条件，poll 忘了同步改，于是"read 能读到但 poll 不报可读"）。

## 二、新增内核 API 的作用与上下文约束

### `poll_wait(file, &sd->wq, wait)` —— 只登记，不睡眠

```c
static __poll_t sensor_poll(struct file *file, poll_table *wait)
{
	struct sensor_file *sf = file->private_data;

	poll_wait(file, &sf->sd->wq, wait);

	if (sensor_has_new_sample(sf))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}
```

- **它做什么**：把"本文件"加入 `sd->wq` 这个等待队列（在 epoll 场景下，实际是把驱动私有
  等待队列与 epoll 的"就绪队列"通过 `poll_table` 里的回调关联起来）。
- **为什么不睡眠**：真正睡眠的是调用方（`poll()`/`select()`/`epoll_wait()` 系统调用）。
  驱动里的 `.poll` 只是"一次性函数"：告诉内核"要等谁 + 现在什么状态"。内核拿到这两条
  信息后，在系统调用层面把当前任务挂起。因此 `.poll` 里**不能睡眠**。
- **为什么不需要持锁**：`sd->lock` 是 mutex，而 `.poll` 的调用路径（VFS）允许睡眠，
  本身不禁止加锁；但这里加了没意义——要读的 `latest.seq` 只有 4 字节，`READ_ONCE`
  就能保证读到完整值，而真正需要保护的复合状态（`latest` 整体）在 `read()` 里才取。
  更重要的一条：**如果持锁再去 `poll_wait`，就会把"登记等待项"这个动作放进临界区，
  在 epoll 注册回调等路径上引入不必要的锁竞争与潜在死锁风险**。惯例是：
  先 `poll_wait`，再读状态，且都不持互斥锁。
- **顺序不能反**：必须先 `poll_wait` 再判断状态。反过来的话，在"判断无数据"与
  "登记等待项"之间到来的新样本会被漏掉（丢唤醒）。

### `kill_fasync(&sd->fasync, SIGIO, POLL_IN)` —— 异步通知

调用位置在**线程化中断下半部**，紧跟 `wake_up_interruptible()` 之后：

```c
	wake_up_interruptible(&sd->wq);
	kill_fasync(&sd->fasync, SIGIO, POLL_IN);
```

- **上下文约束**：`kill_fasync()` 内部只做链表遍历 + `send_sigio()`，不睡眠，
  可以在原子上下文调用。放在线程化下半部（内核线程）更是完全安全。
- **顺序约束**：必须在"数据已经写好 + 阻塞者已唤醒"之后调用。否则用户态收到 SIGIO
  立刻来读，可能读到旧样本——表现为"信号来了但没数据"。
- **数据流**：`fasync_helper()` 维护的是一条 `fasync_struct` 链表，
  链表节点里记录了 `fa_file`（哪个 file）与 `fa_fd`。`kill_fasync` 遍历它，
  按 `F_SETOWN` 登记的所有者把信号发给对应进程。
- **实测证据**：用 ftrace 对 `kill_fasync` 做函数过滤，测试期间捕获到 **8 次调用**：

```
[CHECK:PASS] ftrace 捕获到 kill_fasync 调用（实测 8 次）
```

  同时用户态 `sigtimedwait` 收到 1 次 SIGIO（标准信号不排队，多个 SIGIO 会合并，
  所以计数为 1 是正常的，判定用 "≥1"）。

### `fasync_helper(fd, file, on, &sd->fasync)` —— 登记/摘除

```c
static int sensor_fasync(int fd, struct file *file, int on)
{
	struct sensor_file *sf = file->private_data;

	return fasync_helper(fd, file, on, &sf->sd->fasync);
}
```

- VFS 在用户态 `fcntl(F_SETFL, ...|O_ASYNC)` 时调用 `on=1`，清掉 O_ASYNC 时调用 `on=0`；
- `close()` 时 `release()` 必须显式调用 `sensor_fasync(-1, file, 0)` 摘除，
  **且必须在 `kfree(sf)` 之前**（`fasync_helper` 会通过 `filp->private_data` 找驱动私有数据，
  释放后再调用就是 use-after-free）；
- 未注册过异步通知时 `sd->fasync == NULL`，`fasync_helper(on=0)` 内部循环直接不执行，
  返回 0，安全。

### `O_NONBLOCK` —— 谁负责"非阻塞"？

**驱动的责任**。`O_NONBLOCK` 是用户态通过 `open()/fcntl()` 设置的 `file->f_flags` 位，
VFS 不会替驱动决定行为；驱动必须自己在可能等待的地方检查它：

```c
	if (!sensor_has_new_sample(sf)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/* ... 阻塞等待 ... */
	}
```

返回 `-EAGAIN`（而不是 0 或 `-EBUSY`）是约定：`-EAGAIN` 明确告诉用户态"现在没数据，
稍后再来"，这是 `epoll` 边缘触发模式必须依赖的语义（ET 模式要求一直读到 EAGAIN）。

### `wait_event_interruptible_timeout()` —— 为什么不会丢唤醒

```c
		ret = wait_event_interruptible_timeout(
			sd->wq, sensor_has_new_sample(sf),
			msecs_to_jiffies(SENSOR_READ_TIMEOUT_MS));
		if (ret == 0)  return -ETIMEDOUT;
		if (ret < 0)   return -ERESTARTSYS;
```

- 宏内部是 **"先把当前任务加进等待队列 → 再判断条件 → 条件不成立才真正睡眠"**
  的循环结构，条件判断与"加入队列"之间由等待队列的自旋锁保护，所以不会出现
  "条件刚成立、唤醒已发出、任务还没睡下"的丢唤醒窗口；
- 返回值语义容易记错，这里写清楚：**超时返回 0，被信号打断返回负数**（`-ERESTARTSYS`），
  返回正数表示条件满足（剩余 jiffies）；
- 之所以要 `timeout` 版本而不是无限等待：避免硬件/中断异常时用户态永久卡死，
  这也是块设备/网络驱动里的通用做法。

## 三、用户态测试程序（user/io_models_test.c）

四种模型的判定标准与"为什么这样判定"：

| 模型 | 实测 | 判定标准 |
|---|---|---|
| 阻塞 read | `blocking_read=ok`，读到 `seq=1 temp=25.100C` | 无 O_NONBLOCK 打开后 read 返回 >0 |
| 非阻塞 read | `nonblock_eagain=1` | 必须观察到 `EAGAIN`：若驱动把非阻塞当阻塞处理，用户态就无法用 epoll 组织 IO；若重复返回旧样本则永远看不到 EAGAIN |
| epoll | `epoll_wait_ok=1`、`epoll_wakeups=7`、`epoll_reads=7`、`elapsed_ms=3000` | 3 秒窗口内被唤醒且确实读到数据 |
| poll 电平触发 | `epoll_wakeups=7 ≤ 12` | 500ms 周期 3 秒应唤醒 6~7 次。判断错误的实现（消费后仍报可读）会达到数千次 |
| SIGIO | `sigio_count=1` | 收到过即链路（F_SETOWN + O_ASYNC + kill_fasync）全通 |

程序把每项结果打印成 `[IO] 键=值`，并输出 `[IO] OVERALL=PASS|FAIL` 与退出码。

## 四、遇到的问题与修复过程

### 问题 1：ftrace 证据始终为 0（自己写的检查，先失败后修好）

- **现象**：第一次跑阶段测试，7 项必需检查全 PASS，但附加的
  `[CHECK:FAIL] ftrace 捕获到 kill_fasync 调用（实测 0 次）`。
- **排查**：SIGIO 确实收到了（`sigio_count=1`），说明 `kill_fasync` 必然被调用过，
  于是怀疑是采集方式而不是代码。
- **根因**：脚本里在**把 `current_tracer` 切回 `nop` 之后**才去读 `trace` 文件，
  而**切换 tracer 会清空 trace 环形缓冲区**，所以永远读到 0。
- **修复**：把计数挪到"停止 tracing 之后、切回 nop 之前"：

```sh
echo 0 > $TR/tracing_on
KF=$(grep -c kill_fasync $TR/trace 2>/dev/null)   # 必须在切回 nop 之前读
echo nop > $TR/current_tracer
```

- **结果**：修复后捕获到 8 次调用（见上方证据）。
- **教训**：ftrace 的读取必须在"停止 tracing 且未复位 tracer"的窗口内完成。

### 问题 2：`smoke` 回归其实没跑 smoke（测试框架缺陷，已修）

- **现象**：按 `docs/10` 的开发循环，先用 `TEST=01-io-models` 构建 initramfs，
  再执行 `bash scripts/22-macos-run-test.sh smoke`。结果输出的是 **01-io-models 的检查项**——
  因为 initramfs 是构建时把阶段脚本"烤"进去的，QEMU 只是启动它，
  换测试名并不会换脚本。
- **影响**：这会得出**假的"回归通过"结论**——正是 `docs/10` 明令禁止的。
- **修复**（最小改动，3 处）：
  1. `tests/runner/init.sh`：优先从内核命令行读 `test=<名字>`，
     据此从 `/tests/phases/<名字>.sh` 选择脚本，找不到才回退到构建时注入的默认脚本；
  2. `scripts/13-vm-fast-cycle.sh`：把 `tests/phases/*.sh`（及 `*.modules`）全部打进 initramfs；
  3. `scripts/22-macos-run-test.sh`：`-append "console=ttyAMA0 test=$TEST"`。
- **结果**：同一次构建的 initramfs，连续两次运行分别真正执行了两个脚本：

```
logs/20260914-025014-01-io-models.log: 内核: 6.6.156 / aarch64   阶段脚本: /tests/phases/01-io-models.sh
logs/20260914-025021-smoke.log        : 内核: 6.6.156 / aarch64   阶段脚本: /tests/phases/smoke.sh
```

- **副作用（无害，已核实）**：内核会打印一行信息
  `Unknown kernel command line parameters "test=smoke", will be passed to user space.`
  这只是提示，不影响启动，也不匹配检查项里的 `WARNING:`/`Call trace:`。

## 五、最终验证结论

### 阶段测试 `logs/20260914-025014-01-io-models.log`：9 PASS / 0 FAIL

```
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

### 回归 `logs/20260914-025021-smoke.log`：12 PASS / 0 FAIL

```
[CHECK:PASS] 设备节点 /dev/sensor0 存在
[CHECK:PASS] 设备树解析到 i2c-bus/sensor-addr
[CHECK:PASS] I2C 客户端创建成功
[CHECK:PASS] 虚拟中断请求注册成功
[CHECK:PASS] 字符设备注册成功
[CHECK:PASS] sensor_test 退出码为 0
[CHECK:PASS] read() 返回带 seq 的样本
[CHECK:PASS] ioctl(GET_SAMPLE) 成功
[CHECK:PASS] ioctl(GET_STATS) 输出统计
[CHECK:PASS] 测试正常结束
[CHECK:PASS] ftrace 抓到 sensor_irq_thread（次数 10）
[CHECK:PASS] dmesg 无 WARNING/Call trace
[TEST:END] smoke pass=12 fail=0
```

两次运行的 `内核 panic: 0`、`内核告警: 0`。

### 复现命令

```bash
cd /Users/lucien/workspace/self-study/projects/linux-char-driver

# 构建（只需一次；一次构建可跑任意阶段测试）
limactl shell dev bash -c 'cd ~ && TEST=01-io-models bash /Users/lucien/workspace/self-study/projects/linux-char-driver/scripts/13-vm-fast-cycle.sh'
bash scripts/21-macos-sync-artifacts.sh

# 阶段测试 + 回归
bash scripts/22-macos-run-test.sh 01-io-models 180
bash scripts/22-macos-run-test.sh smoke 180
```

## 六、已知限制与遗留风险

1. **`SENSOR_IOC_RESET` 与 per-open 基准的交互**：
   RESET 会把 `latest.seq` 归零，此时某个已经读过样本的 fd 会认为"有新样本"，
   于是再 read 一次会返回 `seq=0` 的旧样本。契约把判定明确为
   `latest.seq != sf->last_seq`，因此当前实现与契约一致；
   现有测试（含 `sensor_test`）都不调用 RESET，未受影响。
   彻底解决需要引入"永不回退的样本代号"（例如另设 `u64 sample_gen`），属于接口层面的取舍，
   已在此登记为遗留项。
2. **SIGIO 计数通常为 1**：标准信号不排队，多个 SIGIO 会合并成一次 pending，
   因此测试判定用 "≥1" 而不是"N 次"。若要精确计数需改用实时信号（`SIGRTMIN+n`）+
   `F_SETSIG`，本阶段不做。
3. **忙碌轮询阈值 12 次**：这是"3 秒窗口 / 500ms 周期"下的经验阈值。
   若后续阶段把默认采样周期改小（例如阶段 03 的 10ms），
   `epoll_wakeups` 会随之增大，该阈值需要同步调整（阶段 03 的脚本里已按 10ms 场景单独处理）。
4. **测试框架改动的影响面**：`tests/runner/init.sh` 的选择逻辑改成了
   "内核命令行优先、构建时默认值兜底"，对所有后续阶段生效；
   旧行为（不传 `test=`）完全保留，因此不会破坏已有用法。
