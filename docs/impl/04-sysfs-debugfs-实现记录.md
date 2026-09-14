# 阶段 04 实现记录：sysfs 设备参数 + debugfs 运行统计

- 改动文件：`driver/sensor_char.c`（+约 260 行）、`tests/phases/04-sysfs-debugfs.sh`（新增）、
  `tests/runner/lib.sh`（把 `tag_val` 提升为公共断言 + 补充正则转义说明）
- 验证日志：`logs/20260914-133749-main-04-sysfs-debugfs.log`（本轮最后一次全绿运行）
- 结论：04 阶段 **42 项检查全绿**；回归 03（40）、02（19）、01（16）、smoke（15）全绿

---

## 一、为什么是"两个接口"而不是一个

内核向用户态暴露信息有四套机制，本阶段用了其中两套，选择依据是**ABI 承诺强度**：

| 机制 | ABI 承诺 | 本阶段用途 |
|------|---------|-----------|
| sysfs | **稳定 ABI**：一旦发布，语义不能再变 | 设备参数（周期）与设备状态（序号/错误数/容量） |
| debugfs | 明确声明**不是稳定 ABI**，内核不保证兼容 | 统计快照、寄存器现场值、缓冲明细 |

这条边界是硬性的，不是风格偏好：

- sysfs 有 "one value per file" 约定。若把 `stats` 那种一行 10 个字段的东西塞进 sysfs，
  就等于发布了一个永远不能改格式的 dump 接口；将来想加一个字段都是 ABI 变更。
- debugfs 的价值恰恰在于"随便改"：加字段、改格式、删文件都不算破坏用户空间，
  所以它适合放调试信息。代价是**不能**把它当生产接口依赖。

代码里对应两处（`driver/sensor_char.c`）：

```c
static struct attribute *sensor_attrs[] = {   /* 稳定 ABI：4 个属性 */
	&dev_attr_interval_ms.attr,   /* RW */
	&dev_attr_seq.attr,           /* RO */
	&dev_attr_i2c_errors.attr,    /* RO */
	&dev_attr_ring_capacity.attr, /* RO */
	NULL,
};
```

```c
	debugfs_create_file("stats", 0444, sd->dbg, sd, &sensor_dbg_stats_fops);
	debugfs_create_file("ring",  0444, sd->dbg, sd, &sensor_dbg_ring_fops);
	debugfs_create_file("regs",  0444, sd->dbg, sd, &sensor_dbg_regs_fops);
```

**挂载点选在字符设备类设备上**（`sd->char_dev` → `/sys/class/sensor_char/sensor0/`），
而不是 i2c client 设备（`/sys/bus/i2c/devices/0-0048/`）。理由：用户面对的是
"这个传感器字符设备"，class 路径与 `/dev/sensor0` 一一对应；
`device_create()` 的第 4 个参数就是 drvdata，属性回调里 `dev_get_drvdata(dev)` 直接拿回 `sd`。

---

## 二、属性的读写语义与锁

### interval_ms（RW）

```c
static ssize_t interval_ms_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sensor_dev *sd = dev_get_drvdata(dev);
	unsigned long ms;
	int ret;

	ret = kstrtoul(buf, 0, &ms);
	if (ret)
		return ret;                 /* 非法数字 → -EINVAL，不静默当 0 */
	if (!sensor_interval_valid(ms))
		return -EINVAL;
	sensor_apply_interval(sd, ms);  /* 内部: mutex 保护 interval_ms + 重启 hrtimer */
	return count;
}
```

三个要点：

1. **用 `kstrtoul` 而不是 `simple_strtoul`**：后者对 `"abc"` 返回 0 且不报错，
   于是"用户写错"会变成"用户要求 0ms"。`kstrtoul` 会返回 `-EINVAL`。
2. **范围判定与 ioctl 共用同一个函数**（见第四节）。
3. **store 的返回语义**：成功必须返回 `count`（否则用户态 `echo` 会报错），
   失败返回负 errno。

读侧用 `sysfs_emit`（而不是 `sprintf`）——这是 sysfs 的规范写法，
它内部处理了缓冲区大小约束与 `PAGE_SIZE` 限制。

### seq / i2c_errors（RO）

只读计数器，用 `READ_ONCE()` 读取。为什么不用锁：这些字段是 `u32`，
单次对齐读在 CPU 层面不会读到"半个值"，而 sysfs 读到的是瞬时快照，
不需要与其他字段保持一致（真正的强一致场景在 `debugfs/stats`，那里用 mutex）。

### ring_capacity（RO）

```c
return sysfs_emit(buf, "%u\n", sensor_ring_capacity(sd));
/* = kfifo_size(&sd->ring) / sizeof(struct sensor_sample) */
```

**这条属性存在的理由不是"好看"**：`kfifo_alloc` 会把请求的字节数向上取整到 2 的幂，
请求 `64 × 24 = 1536` 字节实际得到 2048 字节 → **真实容量是 85 个样本**（实测值）。
用户态要用它判断"队列满没满、还能读多少"，必须拿到真实容量，
而不是驱动源码里那个看起来更整齐的 64。

---

## 三、debugfs 三个文件

三个文件都是**每次 read 现场生成文本**（不缓存），所以读到的永远是当前值。

### stats —— 一行汇总

```c
	len = scnprintf(kbuf, sizeof(kbuf),
		"open=%u read=%u irq=%u i2c_err=%u interval=%lu dropped=%u ring_count=%u ring_capacity=%u seq=%u shm_writes=%u\n", ...);
```

持 `sd->lock`（mutex）读取全部字段，保证这一行是**同一时刻的一致快照**；
格式化完成后立即释放锁，再 `simple_read_from_buffer()` 拷给用户态
（即"不在持锁期间做拷贝到用户空间"）。

### regs —— 证明 regmap 通路是活的

```c
	for (reg = SENSOR_REG_TEMP; reg <= SENSOR_REG_CONFIG; reg++) {
		ret = regmap_read(sd->regmap, reg, &val);
		if (ret)
			len += scnprintf(..., "%02x: <read error %d>\n", reg, ret);
		else
			len += scnprintf(..., "%02x: %04x\n", reg, val);
	}
```

这个文件的意义不只是"看一眼寄存器"：它跑的是**与中断线程采集样本完全相同**的
寄存器访问路径（`regmap_read` → i2c core → `smbus_xfer`）。所以它是
"regmap 通路真的在工作"最直接的证据，而不是间接推断。

实测输出（`logs/20260914-133749-main-04-sysfs-debugfs.log`）：

```
    00: 0188     ← 温度 raw，0x0188 = 392 → 392/16 = 24.5 ℃，落在芯片模拟区间 24.0~26.0
    01: 0fa5     ← 湿度 raw = 4005 → 40.05 %RH
    02: 0001     ← probe 写入的"连续转换使能"位
```

**失败时必须如实显示错误码**，不能显示假值——否则看日志的人会以为总线正常，
这正是"证据必须来自实测"的纪律（详见阶段 02 的假绿教训）。

### ring —— 缓冲状态 + 最近 8 条样本

样本从 **mmap 共享区**里取，而不是去动 kfifo 的读写索引：

```c
	spin_lock(&sd->shm_lock);
	total = sd->shm->count;
	widx = sd->shm->write_idx;
	n = min(total, ARRAY_SIZE(snap));
	start = (widx + SENSOR_SHM_SAMPLES - n) % SENSOR_SHM_SAMPLES;
	for (i = 0; i < n; i++)
		snap[i] = sd->shm->samples[(start + i) % SENSOR_SHM_SAMPLES];
	spin_unlock(&sd->shm_lock);
	/* 锁外格式化 + 拷贝 */
```

为什么这样取数：`read()` 是通过 kfifo 出队消费的，
如果调试接口也用 kfifo 去取，就会**改变用户态的消费进度**（调试行为影响被观测对象）。
共享区是"只读快照"，取它不影响任何人的状态。

为什么只持锁拷贝、不持锁格式化：`shm_lock` 是 spinlock，
且中断线程也会拿它发布样本；持锁期间做 `scnprintf` 只是浪费中断延迟预算，
拷到用户空间更是禁止的（可能睡眠）。

实测输出：

```
    count=6 dropped=0 ring_count=6 ring_capacity=85 shm_writes=6 recent=6
    sample[0] seq=1 temp_milli=24062
    sample[1] seq=2 temp_milli=24187
    sample[2] seq=3 temp_milli=24250
```

---

## 四、顺手修掉的一个真实不一致：周期下限

实现本阶段时发现：`ioctl(SENSOR_IOC_SET_INTERVAL)` 的判定是 `interval < 10 || > 60000`，
而 sysfs 契约要求 `1~60000`。**同一个"设置采样周期"，走 A 接口成功、走 B 接口失败**，
这类不一致是排查成本最高的 bug 之一。

处理：抽出单一判定与单一生效动作，两个入口都调它们：

```c
#define SENSOR_INTERVAL_MIN_MS	1UL
#define SENSOR_INTERVAL_MAX_MS	60000UL

static bool sensor_interval_valid(unsigned long ms)
{
	return ms >= SENSOR_INTERVAL_MIN_MS && ms <= SENSOR_INTERVAL_MAX_MS;
}

static void sensor_apply_interval(struct sensor_dev *sd, unsigned long ms)
{
	mutex_lock(&sd->lock);
	sd->interval_ms = ms;
	mutex_unlock(&sd->lock);
	hrtimer_cancel(&sd->timer);                                  /* 等回调结束 */
	hrtimer_start(&sd->timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
	dev_info(&sd->client->dev, "interval -> %lu ms\n", ms);
}
```

这是**放宽**（10→1）而不是收紧，属于向后兼容的行为变化；
测试里同时断言两个入口给出一致结果（见验证证据第 3 条）。
说明：1ms 下限对本实验环境是实际可用的；真机上看 I2C 事务耗时与中断延迟再决定下限。

---

## 五、故障注入：怎么证明"错误处理路径被执行到"

代码写得再对，"没跑过的错误处理"等于没写。能制造故障是验证错误处理的唯一办法：

```
# 注入 3 次失败：接下来 3 次 I2C 传输返回 -EIO
echo 3 > /sys/kernel/debug/virt_i2c/inject_error
```

注入后的完整链路（实测日志）：

```
virt_i2c: fault injection armed: next 3 transfer(s) will fail
virt_i2c: injected error: addr=0x48 reg=0x00 (left=2)
sensor_char 0-0048: regmap read failed: -5     ← -EIO 传到驱动
... （共 3 次）
注入 3 次：i2c_errors 0 → 3（增量 3）
恢复期样本序号：108 → 159                       ← 注入耗尽后采样自恢复
```

这个实验同时验证了三件事：

1. `regmap_read` 的失败**确实**被驱动检查了（`sd->i2c_errors++` + `dev_warn_ratelimited`）；
2. 驱动的错误处理没有破坏后续采样（序号继续增长）；
3. 用户态通路在注入后仍然正常（`sensor_test` 退出码 0）。

**顺序陷阱（写测试时踩到的）**：注入的是"接下来 N 次传输"，而读 `debugfs/regs`
自己也要发起 3 次传输。如果在注入和检查之间读 regs，注入的失败次数会被它消耗掉，
`i2c_errors` 的增量就不再是 3，结论直接错。所以测试脚本里：
先取基准 → 注入 → 等待 → 检查，中间不碰任何会发起 I2C 事务的接口。

---

## 六、踩到的问题与解决

| # | 现象 | 根因 | 解决 |
|---|------|------|------|
| 1 | 编译报 `implicit declaration of sensor_interval_valid` + `conflicting types` | 辅助函数被插到了 `sensor_ioctl()` **之后**，而 ioctl 里已经调用它 | 把"范围判定/生效动作"整块移到 ioctl 之前（C 里 `static` 函数必须先声明或先定义） |
| 2 | 检查项 `ring 含样本明细 sample[` 失败，日志里却有 `sample[0] seq=1 ...` | `check_contains` 底层是 `grep`，模式按**正则**解释；`sample[` 是未闭合字符类，busybox grep 直接报 `bad regex` | 转义为 `sample\[`，并在 `lib.sh` 的注释里写明"模式是正则，字面量里的 `[ ( . *` 必须转义" |
| 3 | ioctl 与 sysfs 的周期下限不一致（10 vs 1） | 两个入口各自写了一份判定 | 抽成 `sensor_interval_valid()` + `sensor_apply_interval()`（第四节） |
| 4 | sysfs 里"非法值被拒绝"容易写成假绿 | 只判断"echo 是否失败"不够：内核可能接受但改成了别的值 | 断言**值保持不变**（`60000` 仍是 `60000`）+ 报错信息含 `nvalid`，两个角度一起判 |
| 5 | debugfs 不可用时是否该让 probe 失败 | 生产内核常常不挂 debugfs，`debugfs_create_dir` 会返回 `ERR_PTR(-ENODEV)` | 不致命：`dev_warn` + `sd->dbg = NULL`；`debugfs_remove_recursive(NULL)` 是安全的（清理路径统一，无需分支） |

---

## 七、验证证据

运行方式（约 30 秒）：`bash scripts/22-macos-run-test.sh 04-sysfs-debugfs 300`

| 测试 | 项数 | 结果 | 日志 |
|------|------|------|------|
| 04-sysfs-debugfs | **42** | 全绿 | `logs/20260914-133749-main-04-sysfs-debugfs.log` |
| 03-ringbuffer-mmap（回归） | 40 | 全绿 | 同批运行 |
| 02-i2c-driver（回归） | 19 | 全绿 | 同批运行 |
| 01-io-models（回归） | 16 | 全绿 | 同批运行 |
| smoke（回归） | 15 | 全绿 | 同批运行 |

关键实测值（都可在上面的日志里 grep 到）：

1. `user interfaces ready: sysfs=/sys/class/sensor_char/sensor0, debugfs=/sys/kernel/debug/sensor_char`
2. 属性初值：`interval_ms=500 seq=0 i2c_errors=0 ring_capacity=85`
   （`ring_capacity=85` 正是 kfifo 向上取整后的真实容量）
3. 两个入口一致：`写入 100 后：sysfs=100  ioctl=100`
4. 边界与非法值：下界 `1`、上界 `60000` 被接受；`0`、`60001`、`abc` 被拒绝且值不变
5. 寄存器现场值：`00: 0188`（24.5 ℃）、`01: 0fa5`、`02: 0001`
6. 故障注入：`注入 3 次：i2c_errors 0 → 3（增量 3）`；`恢复期样本序号：108 → 159`
7. 告警检查（测试体**之后**取样）：`dmesg 无 WARNING/Call trace/BUG`

## 八、遗留风险

1. `debugfs/ring` 只打印最近 8 条样本，且样本来自共享区（受 `count` 上限 32 限制）；
   想看完整历史要用 `read()` 出队——这是刻意的（调试接口不应改变被观测状态）。
2. `interval_ms` 下限放宽到 1ms 后，用户态可以设到 1ms 导致 kfifo 快速溢出
   （表现为 `dropped` 增长）——这是**预期行为**，缓冲满就丢并计数，
   不是缺陷，但文档需要说清楚（阶段 03 已记录丢弃策略）。
3. sysfs 属性挂在 class 设备上，因此 `rmmod` 后 `/sys/class/sensor_char/` 整个消失
   （class 也销毁）——如果将来做成多实例驱动，需要重新考虑该路径的稳定性。
