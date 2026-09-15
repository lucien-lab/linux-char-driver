# 阶段 06 知识点：Runtime PM（运行时电源管理）+ KUnit（内核单元测试）

> 面向目标：嵌入式 / Linux 驱动岗面试。读完应当能**讲清 runtime PM 的引用计数模型、
> 画出 get/put 到 `runtime_suspend` 回调的调用链、说清"假省电"为什么最危险，
> 并说清 KUnit 与用户态测试/kselftest 的边界**，能被追问到"不这么写会怎样"。
>
> **引用约定**
> - 内核源码引用来自本项目目标内核树 `~/kernel-build/linux-6.6.156`（Linux 6.6.156），
>   格式 `文件:行号`，行号可用 `grep -n` 直接复核。
> - 本项目源码引用格式 `driver/sensor_char.c:行号`、`driver/sensor_calc.h:行号`。
> - 所有"实测"结论都标了日志文件名。
>
> **本文使用的日志**
> | 日志 | 内容 | 结论 |
> |---|---|---|
> | `logs/20260914-135029-main-06-runtime-pm-kunit.log` | 阶段 06 最终测试（当前 HEAD） | **31 PASS / 0 FAIL** |
> | `logs/20260914-134436-main-06-runtime-pm-kunit.log` | 第一版（读错 sysfs 路径 + KUnit 判定写成 `ok N -`） | **11 FAIL**，本文 §1.5 / §4.3 引用其失败证据 |
> | `logs/20260914-135048-main-04-sysfs-debugfs.log` | 回归（行为变化适配后） | 42 PASS / 0 FAIL |
> | `logs/20260914-135054-main-03-ringbuffer-mmap.log` | 回归 | 40 PASS / 0 FAIL |
> | `logs/20260914-135106-main-02-i2c-driver.log` | 回归 | 19 PASS / 0 FAIL |
> | `logs/20260914-135111-main-01-io-models.log` | 回归 | 16 PASS / 0 FAIL |
> | `logs/20260914-135122-main-smoke.log` | 基线回归 | 15 PASS / 0 FAIL |
>
> 说明：`logs/` 目录位于主工作树
> `/Users/lucien/workspace/self-study/projects/linux-char-driver/logs/`（被 `.gitignore` 忽略，
> 不随 worktree 复制）。本文引用时仍写相对名 `logs/xxx.log`，与项目文档保持一致。

---

## 0. 一句话心智模型

阶段 05 的心智模型是"**驱动把数据交给子系统去缓冲**"。
阶段 06 的心智模型是：

> **Runtime PM 不是"驱动去关电"，而是"驱动向 PM 核心报告设备的使用状态；
> 核心按引用计数决定何时调用驱动的 suspend/resume 回调"。**

三句话概括本阶段的全部内容：

1. **Runtime PM = 引用计数 + 回调**。驱动只在"用"的时候 `get`（引用 +1）、
   用完 `put`（引用 −1）；引用归零后由 **PM 核心**回调 `->runtime_suspend()`。
   设备什么时候省电由核心决定，驱动只负责"报告"和"实现动作"。
2. **"省电"必须是物理动作**。回调里要真的停掉数据源（定时器/中断），
   而不是只把状态标成 `suspended`。否则 `runtime_status` 说已挂起、硬件还在跑，
   这就是**假省电**——最难发现的一类错误（测试项专门抓它）。
3. **KUnit 不是"再写一个测试程序"，而是"在内核里断言纯函数"**。
   它补的是集成测试的盲区：边界值难自然出现、失败难定位、并发/副作用无法覆盖。
   能测什么、不能测什么必须写清楚，否则就是给自己造假证据。

本阶段项目的实测证据（`logs/20260914-135029-main-06-runtime-pm-kunit.log`）：

```text
[    0.642255] sensor_char 0-0048: runtime PM enabled (control=auto autosuspend=1000ms)
[    1.646894] sensor_char 0-0048: runtime suspend: sampling stopped (irq_count=2 seq=2)
[    3.697208] sensor_char 0-0048: runtime resume: sampling restarted (interval=500ms)
[    3.697578] sensor_char 0-0048: opened (count=1)
[    4.735020] sensor_char 0-0048: closed
[    5.756469] sensor_char 0-0048: runtime suspend: sampling stopped (irq_count=6 seq=6)
```

---

## 1. Linux 电源管理全景

### 1.1 System sleep 与 Runtime PM：区别与联系

Linux 的电源管理是**两层**的：系统级睡眠（system sleep）和工作时的运行时电源管理
（runtime PM）。它们回调函数集不同、生效范围不同、触发者不同。

| 维度 | System sleep（系统睡眠） | Runtime PM（运行时电源管理） |
|---|---|---|
| 英文/缩写 | suspend-to-RAM（S3）、suspend-to-idle（S2I）、hibernation（S4） | runtime PM / runtime suspend |
| 作用范围 | **整机**（全局低功耗状态，用户态被冻结） | **单个设备**（其它设备照常工作） |
| 触发者 | 用户态写 `/sys/power/state`、或空闲策略 | 内核：设备引用计数归零（+ autosuspend 延迟） |
| 与用户态的关系 | 用户态代码无法执行 | 用户态进程照常运行 |
| 回调宏 | `SET_SYSTEM_SLEEP_PM_OPS`（`include/linux/pm.h:342`） | `SET_RUNTIME_PM_OPS`（`include/linux/pm.h:363`） |
| 回调函数 | `->suspend/->resume`（+`_late/_early/_noirq`） | `->runtime_suspend/->runtime_resume/->runtime_idle` |
| 编译依赖 | 无 `CONFIG_PM_SLEEP` 时宏展开为空（`include/linux/pm.h:345`） | 无 `CONFIG_PM` 时宏展开为空（`include/linux/pm.h:366`） |

**两者的联系**（面试常追问）：

1. 二者描述的是**同一颗硬件的不同低功耗路径**，所以同一份"进入低功耗前要停数据源"
   的逻辑常常被两个回调共用（本项目只实现了 runtime 路径，所以没有这个问题）。
2. 交互点在"系统睡眠时设备已经 runtime-suspended 怎么办"。内核文档给出了处理方式
   （`Documentation/power/runtime_pm.rst:640` 起，§6 "Runtime PM and System Sleep"）：
   系统 resume 时**一律把设备恢复到 full power**，然后用

   ```c
   pm_runtime_disable(dev);
   pm_runtime_set_active(dev);
   pm_runtime_enable(dev);
   ```

   把 runtime PM 状态同步成真实状态（`Documentation/power/runtime_pm.rst:682`）。
   理由很实在：设备的 wakeup 设置/供电档位在系统睡眠前后可能不同、
   固件可能丢远程唤醒事件、子设备恢复需要父设备先上电
   （`Documentation/power/runtime_pm.rst:659` 的列表）。
3. 系统睡眠是"全局状态"，但它**不是**由某个设备驱动的回调单独决定的；
   设备驱动只提供"我该怎么睡/怎么醒"。这也是为什么两个回调集要分开。

> 本项目**只实现了 Runtime PM**（`SET_RUNTIME_PM_OPS`），没有实现系统睡眠
> （未实现 `SET_SYSTEM_SLEEP_PM_OPS`）。这是刻意的范围控制：把没验证过的路径写进
> 驱动只会制造"看起来支持、实际没测过"的假象。这条已写进实现记录"遗留风险"。

### 1.2 Runtime PM 的引用计数模型

`struct device` 里有一组 runtime PM 状态字段，定义在 `struct dev_pm_info`
（`Documentation/power/runtime_pm.rst:205` 起，§3 "Runtime PM Device Fields" 有逐字段说明）。
最核心的几个：

| 字段 | 含义 | 本项目对应的观测 |
|---|---|---|
| `power.usage_count` | **引用计数**（`atomic_t`） | `open()` +1、`release()` −1 |
| `power.disable_depth` | 0 = 已启用；初始值 **1** = runtime PM 未启用 | 决定 `runtime_status` 是否显示 `unsupported` |
| `power.runtime_status` | 核心认为的状态：`active/suspended/suspending/resuming` | `/sys/.../power/runtime_status` |
| `power.runtime_auto` | 用户态是否允许核心自动挂起 | `/sys/.../power/control`（`auto`/`on`） |
| `power.last_busy` | 最后一次"忙"的时间戳 | autosuspend 的计时起点 |

三个字段的**初始值**来自 `pm_runtime_init()`（`drivers/base/power/runtime.c:1796` 起）：

```c
dev->power.runtime_status = RPM_SUSPENDED;   /* runtime.c:1798 */
...
dev->power.disable_depth = 1;                /* runtime.c:1802 */
...
dev->power.runtime_auto = true;              /* runtime.c:1809 */
```

这三个初始值直接解释了三件"看起来反直觉"的事：

**（1）为什么没启用 PM 的设备 `runtime_status` 恒为 `unsupported`。**

```c
/* drivers/base/power/sysfs.c:150 */
static ssize_t runtime_status_show(...)
{
	if (dev->power.runtime_error)
		output = "error";
	else if (dev->power.disable_depth)      /* ← disable_depth 非 0 就是 unsupported */
		output = "unsupported";
	else ...
}
```

`disable_depth` 初始为 1，只有驱动调用 `pm_runtime_enable()` 才会减到 0
（`drivers/base/power/runtime.c:1528`，`pm_runtime_enable()` 里 `--dev->power.disable_depth`）。
**没调过 `pm_runtime_enable()` 的设备，它的 `runtime_status` 恒为 `unsupported`**——
这不是错误，而是"该设备/该驱动没参与 runtime PM"的正式表达。

**（2）为什么 `power/control` 对所有设备默认就是 `auto`。**

`runtime_auto = true`（`runtime.c:1809`）→ `control_show()` 输出 `auto`
（`drivers/base/power/sysfs.c:101`）。`control_store()` 写 `on`/`auto` 分别调用
`pm_runtime_forbid()/pm_runtime_allow()`（`drivers/base/power/sysfs.c:108`）。
**所以 `control == auto` 不能作为"驱动启用了 PM"的判据**——它对任何设备都成立。
本项目第一版测试就栽在这上面（§1.5 有实测证据）。

**（3）为什么设备初始被当成"已挂起"。**

`runtime_status = RPM_SUSPENDED`（`runtime.c:1798`）。这是"保守假设"：
PM 核心宁可多做一次（无意义的）resume，也不愿假设设备还活着。
所以驱动在 probe 里必须显式 `pm_runtime_set_active()`（§2.4）。

### 1.3 引用计数 → suspend 的调用链（带源码行号）

以本项目 `open()` 为例，`pm_runtime_resume_and_get()` 的内部路径：

```text
sensor_open()                            driver/sensor_char.c:513
  pm_runtime_resume_and_get(dev)         include/linux/pm_runtime.h:432
    __pm_runtime_resume(dev, RPM_GET_PUT)  drivers/base/power/runtime.c:1158
      atomic_inc(&dev->power.usage_count)  runtime.c:1167   ← 引用 +1
      rpm_resume(dev, rpmflags)            runtime.c:761
        RPM_GET_CALLBACK(dev, runtime_resume)  runtime.c:48 → __rpm_get_callback() runtime.c:21
        rpm_callback(cb, dev)              runtime.c:426
          __rpm_callback(cb, dev)          runtime.c:360
            cb(dev)  ==  sensor_runtime_resume()  driver/sensor_char.c:1278
```

`release()` 方向是：

```text
sensor_release()                         driver/sensor_char.c:528
  pm_runtime_mark_last_busy(dev)         include/linux/pm_runtime.h:221  ← 记 last_busy
  pm_runtime_put_autosuspend(dev)        include/linux/pm_runtime.h:464
    __pm_runtime_suspend(dev, RPM_GET_PUT|RPM_ASYNC|RPM_AUTO)  runtime.c:1122
      rpm_drop_usage_count()             runtime.c:1052   ← 引用 −1（负数会打 underflow）
      rpm_idle → rpm_suspend             runtime.c:466 / runtime.c:558
        （autosuspend 到期后，异步 work）
        rpm_callback → sensor_runtime_suspend()  driver/sensor_char.c:1252
```

`rpm_check_suspend_allowed()`（`drivers/base/power/runtime.c:257`）是**唯一的守门函数**，
说明"什么情况下不允许挂起"：

```c
if (dev->power.runtime_error)                        retval = -EINVAL;
else if (dev->power.disable_depth > 0)               retval = -EACCES;   /* PM 未启用 */
else if (atomic_read(&dev->power.usage_count))       retval = -EAGAIN;   /* 还有人持有 */
else if (... atomic_read(&dev->power.child_count))   retval = -EBUSY;    /* 子设备活跃 */
...
else if (dev->power.runtime_status == RPM_SUSPENDED) retval = 1;         /* 已挂起 */
```

**记住这张表，就能回答"为什么我的设备不挂起"**：引用计数不为 0、`disable_depth != 0`、
有活跃子设备、或 `runtime_error` 被置位（回调曾经返回致命错误）。四个原因覆盖了
绝大多数现场问题。

### 1.4 autosuspend 的意义

`autosuspend` 是"**延迟挂起**"，不是"自动挂起"。
内核文档原话（`Documentation/power/runtime_pm.rst:850` 起，§9）：

> The term "autosuspend" is an historical remnant. It doesn't mean that the
> device is automatically suspended ... rather it means that runtime suspends
> will automatically be delayed until the desired period of inactivity has elapsed.
> （原文见 `Documentation/power/runtime_pm.rst:862`）

它解决的问题是：**改变电源状态本身有代价**（要重新上电、重新拉总线、重新写寄存器）。
如果引用计数刚归零就立刻挂起，用户态"读完关掉、100ms 后又打开"这种模式会让设备
在低功耗和满功耗之间**反复抖动**（bouncing），省下的电还不够切换的开销。

机制上是 `power.last_busy` 决定的（`Documentation/power/runtime_pm.rst:869`）：

1. `pm_runtime_mark_last_busy()` 把 `last_busy` 更新为当前时间
   （`include/linux/pm_runtime.h:221`，实质是
   `WRITE_ONCE(dev->power.last_busy, ktime_get_mono_fast_ns())`）。
2. 带 `RPM_AUTO` 的挂起请求会先算 `pm_runtime_autosuspend_expiration()`
   （`drivers/base/power/runtime.c:163`）：如果"距上次忙还不够 `autosuspend_delay`"，
   就重新排一个延迟定时器，而不是立刻挂起（`runtime.c:580` 起）。
3. 驱动要先用 `pm_runtime_use_autosuspend()` 打开该机制
   （`include/linux/pm_runtime.h:576`）、用 `pm_runtime_set_autosuspend_delay()` 设延迟
   （`include/linux/pm_runtime.h:88`）。之后**一律用带 autosuspend 的变体**
   （`put_autosuspend`/`autosuspend`/`request_autosuspend`/`put_sync_autosuspend`）。

本项目延迟设为 **1000ms**（宏 `SENSOR_AUTOSUSPEND_DELAY_MS`，`driver/sensor_char.c:1246`），
实测挂起发生在 close 之后约 1.02 秒：

```text
[    4.735020] sensor_char 0-0048: closed
[    5.756469] sensor_char 0-0048: runtime suspend: sampling stopped (irq_count=6 seq=6)
```

（1.021s ≈ autosuspend 延迟；注意延迟不是精确值，核心还会加约 25% 的 slack，
见 `drivers/base/power/runtime.c:597` 的注释。）

**延迟可以由用户态改**：`/sys/.../power/autosuspend_delay_ms`（`drivers/base/power/sysfs.c:192`）。
内核文档明确说注册之后这项策略"the length should be controlled by user space"
（`Documentation/power/runtime_pm.rst:872`）——驱动设的是**初始值**，不是永久值。

### 1.5 为什么 PM 状态挂在硬件设备上（本项目实测，面试高频）

这是本阶段第一个"看起来像 bug"的现象，也是最值得讲的面试点。

**现象**（`logs/20260914-134436-main-06-runtime-pm-kunit.log`，11 项 FAIL 的那一版）：

```text
[CHECK:PASS] power/control 存在
[CHECK:PASS] power/runtime_status 存在
      · power/control = auto（auto = 允许核心自动挂起）
[CHECK:PASS] power/control 为 auto
      · 持有 fd 时 runtime_status = unsupported
[CHECK:FAIL] 打开期间 runtime_status 为 active -- 期望='active' 实际='unsupported'
      · 关闭并等待 2 秒后 runtime_status = unsupported
[CHECK:FAIL] 关闭后自动挂起（runtime_status=suspended） -- 期望='suspended' 实际='unsupported'
```

注意这两行的对照：**`control` 读出 `auto`（检查通过），`runtime_status` 读出 `unsupported`（检查失败）**。
如果只检查 `control == auto`，会得到一个"全绿但什么都没验证"的假结论。
这正是 §1.2（2）说的"`control` 作为判据几乎恒真"；最终测试把判据升级为
**`runtime_status != unsupported`**（`tests/phases/06-runtime-pm-kunit.sh:51`）。

**根因**：Runtime PM 挂在**硬件设备**上，不是挂在字符设备接口上。

| 设备对象 | 谁创建 | `/sys/.../power/` 位置 | `runtime_status` |
|---|---|---|---|
| i2c 从设备 `i2c_client`（`0-0048`） | i2c 核心根据设备树枚举 | `/sys/bus/i2c/devices/0-0048/power/` | `active` / `suspended` |
| 字符设备类设备 `sensor0` | 驱动 `device_create()` | `/sys/class/sensor_char/sensor0/power/` | **`unsupported`（恒）** |

原因是核心对**每一个** `struct device` 都初始化 PM 字段（`pm_runtime_init()`，
`drivers/base/power/runtime.c:1796`），并通过 `dpm_sysfs_add()` 把 `power/` 属性组挂到
该 device 的 kobject 上（`drivers/base/power/sysfs.c:694`）。但是：

- 驱动只对 **i2c client 设备**调了 `pm_runtime_enable(dev)`，
  其中 `dev = &client->dev`（`driver/sensor_char.c:1484`）；
- `/sys/class/sensor_char/sensor0` 是 `device_create()` 造出来的**纯软件接口**，
  它自己的 `disable_depth` 永远是初始值 1（`runtime.c:1802`），
  所以它的 `runtime_status` 恒为 `unsupported`（`drivers/base/power/sysfs.c:157`）。

**为什么内核这么设计**（面试时的加分回答）：

> Runtime PM 描述的是"**这颗硬件是否上电/是否在低功耗**"。
> 字符设备 `/dev/sensor0` 只是"给用户态看的一个软件句柄"，
> 它没有电源引脚、没有寄存器、没有 I²C 从地址——把它挂起没有意义。
> 反过来，同一个硬件可以有多种软件接口（字符设备、IIO、块设备……），
> 电源状态必须只有一份，所以只能挂在硬件设备（`i2c_client`）上。

顺带一个推论：**这个坑对所有"驱动自己 `device_create()` 出子设备"的写法都成立**
（IIO、input、hwmon 的 sysfs 节点同理）。看到 `runtime_status = unsupported`
先问一句"我读的是硬件设备还是软件接口"。

---

## 2. 驱动侧正确写法

### 2.1 `dev_pm_ops` 与 `SET_RUNTIME_PM_OPS`

`struct dev_pm_ops` 定义在 `include/linux/pm.h`（`runtime_suspend/runtime_resume/runtime_idle`
三个函数指针在其尾部）。宏有两层（`include/linux/pm.h:336,363`）：

```c
#define RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn) \
	.runtime_suspend = suspend_fn, \
	.runtime_resume  = resume_fn, \
	.runtime_idle    = idle_fn,

#ifdef CONFIG_PM
#define SET_RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn) \
	RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn)
#else
#define SET_RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn)   /* 空展开 */
#endif
```

**为什么要包一层 `SET_` 前缀**：没开 `CONFIG_PM` 时宏展开为空，
这些回调函数就不会被引用，编译器不会报"未使用函数"，代码天然可裁剪。
`SET_SYSTEM_SLEEP_PM_OPS` 保护的是 `CONFIG_PM_SLEEP`（`include/linux/pm.h:342`），
两套保护条件不同（可以只有 runtime PM 没有系统睡眠）。

本项目（`driver/sensor_char.c:1300`）：

```c
static const struct dev_pm_ops sensor_pm_ops = {
	SET_RUNTIME_PM_OPS(sensor_runtime_suspend, sensor_runtime_resume, NULL)
};
```

挂到驱动上而不是设备上（`driver/sensor_char.c:1589`）：

```c
static struct i2c_driver sensor_i2c_driver = {
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= sensor_of_match,
		.pm		= &sensor_pm_ops,   /* ← 同一套逻辑适用于该驱动的所有设备实例 */
	},
	...
};
```

**回调优先级**（被追问时的加分项）：PM 核心不是只找 `dev->driver->pm`。
`__rpm_get_callback()`（`drivers/base/power/runtime.c:21`）按
`pm_domain` → `dev->type->pm` → `dev->class->pm` → `dev->bus->pm` 的顺序找
**子系统级回调**，只有它们都没提供时，才回退到驱动的 `dev->driver->pm`：

```c
if (dev->pm_domain)          ops = &dev->pm_domain->ops;
else if (dev->type && dev->type->pm)  ops = dev->type->pm;
else if (dev->class && dev->class->pm) ops = dev->class->pm;
else if (dev->bus && dev->bus->pm)     ops = dev->bus->pm;
...
if (!cb && dev->driver && dev->driver->pm)
	cb = *(pm_callback_t *)((void *)dev->driver->pm + cb_offset);
```

这就是驱动"不用写 `runtime_suspend` 也能省电"的原因：
如果总线/类已经有通用实现，驱动程序回调可以省略。

### 2.2 `get` 的变体：`get_sync` / `resume_and_get` / `get_noresume`

| 函数 | 位置 | 语义 | 何时用 |
|---|---|---|---|
| `pm_runtime_get_sync(dev)` | `include/linux/pm_runtime.h:419` | **同步** resume，然后引用 +1；**出错也不退还引用** | 老代码；文档不推荐（见下） |
| `pm_runtime_resume_and_get(dev)` | `include/linux/pm_runtime.h:432` | 同步 resume，**成功才 +1**，失败自动退还 | **推荐**（本项目 `open()` 用它） |
| `pm_runtime_get_noresume(dev)` | `include/linux/pm_runtime.h:119` | 只 +1，**不**触发 resume | probe 里"声明设备是活的" |
| `pm_runtime_get(dev)` | `include/linux/pm_runtime.h` | **异步** +1 并发起 resume | 中断上下文（IRQ-safe 设备） |
| `pm_runtime_get_if_in_use()` / `pm_runtime_get_if_active()` | `include/linux/pm_runtime.h` | 条件 +1 | 可选的后台访问 |

`pm_runtime_get_sync()` 的文档注释就是一次公开的"设计复盘"
（`include/linux/pm_runtime.h:414`）：

> the runtime PM usage counter of @dev **remains incremented in all cases, even if
> it returns an error code**. Consider using pm_runtime_resume_and_get() instead of it,
> especially if its return value is checked by the caller, as this is likely to result
> in cleaner code.

**这是一类经典泄漏**：`get_sync` 失败 → `open()` 返回错误码给用户态 → 用户态拿不到 fd，
自然**永远不会调用 `release()`** → 那个引用没有任何地方会归还 → 引用计数永远不为 0 →
**设备永远不挂起**，而且没有任何报错。所以本项目的 `open()` 用的是
`pm_runtime_resume_and_get()`（`driver/sensor_char.c:513`），失败时引用已被核心退还。

`pm_runtime_get_noresume()` 为什么存在：probe 期间设备可能已经在跑（本项目 probe
里启动了 hrtimer），此时调用 `get_sync` 会触发一次**多余**的 resume 回调。
`get_noresume` 只声明"我在用"，配合 `pm_runtime_set_active()` 把核心状态标成 active。

### 2.3 `put` 的变体：`autosuspend` / `put_sync` / `put`

| 函数 | 位置 | 语义 | 何时用 |
|---|---|---|---|
| `pm_runtime_put_autosuspend(dev)` | `include/linux/pm_runtime.h:464` | −1；归零后按 **autosuspend 延迟**异步挂起 | 一般 I/O 用完（本项目 `release()`） |
| `pm_runtime_put_sync(dev)` | `include/linux/pm_runtime.h:483` | −1；归零后**同步**挂起（带 `RPM_GET_PUT`） | 需要确定性的"我返回时设备已挂起" |
| `pm_runtime_put_sync_autosuspend` | `include/linux/pm_runtime.h:516` | −1；同步 +尊重 autosuspend | |
| `pm_runtime_put(dev)` | `include/linux/pm_runtime.h:452` | −1；归零后异步 idle 检查 | 需要与 `RPM_AUTO` 无关时 |
| `pm_runtime_put_noidle(dev)` | `include/linux/pm_runtime.h` | **只 −1，不做任何 idle/suspend 判断** | 内部/错误路径归还 |

三者的差别本质是**两个正交维度**：

```text
               │ 减引用后是否立即处理      │ 是否尊重 autosuspend 延迟
───────────────┼───────────────────────────┼────────────────────────
put_autosuspend│ 异步（排 work item）      │ 是
put_sync       │ 同步（等到回调结束才返回） │ 否
put            │ 异步                      │ 否
```

本项目 `release()`（`driver/sensor_char.c:549-550`）：

```c
pm_runtime_mark_last_busy(dev);
pm_runtime_put_autosuspend(dev);
```

**`mark_last_busy()` 放在 `put` 前面**是必须的：`pm_runtime_put_autosuspend` 发的是
带 `RPM_AUTO` 的请求，核心会用 `last_busy` 判断"是否已经空闲足够久"。
少了这一句，`last_busy` 还停在很久以前 → 核心认为设备早就"不忙了" → **可能立即挂起**，
autosuspend 延迟形同虚设。文档明确要求
"typically just before calling `pm_runtime_put_autosuspend()`"
（`Documentation/power/runtime_pm.rst:869`）。

### 2.4 probe 里的初始化顺序（本项目实测）

`driver/sensor_char.c:1482`：

```c
pm_runtime_get_noresume(dev);
pm_runtime_set_active(dev);
pm_runtime_enable(dev);
pm_runtime_set_autosuspend_delay(dev, SENSOR_AUTOSUSPEND_DELAY_MS);
pm_runtime_use_autosuspend(dev);
pm_runtime_mark_last_busy(dev);
pm_runtime_put_autosuspend(dev);
```

每一步都有理由：

| 调用 | 为什么 | 不做会怎样 |
|---|---|---|
| `get_noresume()` | 声明"probe 期间设备在用"，避免后面 put 时引用变负 | 引用从 0 直接 −1 → `rpm_drop_usage_count()` 打 underflow（`runtime.c:1067`） |
| `set_active()` | 核心初始认为设备 `RPM_SUSPENDED`（`runtime.c:1798`），这里定时器已在跑，必须纠正 | 首次 open 会触发一次多余 resume |
| `enable()` | `disable_depth` 从 1 → 0，PM 才生效 | 一切 PM 调用返回 `-EACCES`，`runtime_status` 永远 `unsupported` |
| `set_autosuspend_delay()` | 设初始延迟 1000ms | 用户态"读完就关"会触发立即挂起 |
| `use_autosuspend()` | 打开 autosuspend 机制 | 延迟设置**不生效**（核心仍走普通挂起路径） |
| `mark_last_busy()` | 把"最后忙"设为现在 | 立即挂起（同 §2.3） |
| `put_autosuspend()` | 交还 `get_noresume` 的引用 → 归零 → 1 秒后挂起 | 引用永远不为 0，**设备永远不挂起且无任何报错** |

**顺序约束**：`set_active()` 必须在 `enable()` 之前。
反过来的话，`enable()` 那一刻 `runtime_status` 还是 `RPM_SUSPENDED`，
核心会认为"设备已挂起"，多跑一次无意义的 resume 流程
（代码注释，`driver/sensor_char.c:1479`）。

### 2.5 常见错误清单（每条都能"被追问到"）

| # | 错误写法 | 现象 | 为什么 |
|---|---|---|---|
| 1 | **漏 `put`**（或 `get` 失败没退还） | 设备**永不挂起**；不报错、不告警；只有 `runtime_status` 一直是 `active` | `usage_count` 永不归零，`rpm_check_suspend_allowed()` 永远返回 `-EAGAIN`（`runtime.c:265`） |
| 2 | 在**原子上下文**调用 `put_sync`/`get_sync` | 内核报 `BUG: sleeping function called from invalid context` | `__pm_runtime_resume()` 有 `might_sleep_if(...)`（`runtime.c:1163`）；同步路径会睡眠等待回调 |
| 3 | 忘 `mark_last_busy()` 就 `put_autosuspend` | 关闭后**立刻挂起**，抖动 | `last_busy` 是 autosuspend 的计时起点（`runtime_pm.rst:869`） |
| 4 | suspend 回调里做**阻塞操作**（拿 mutex、`msleep`、发起可能睡眠的 I2C 传输） | 默认可以睡眠，**但** `pm_runtime_irq_safe()` 之后就不行 | 回调默认在进程上下文（`Documentation/power/runtime_pm.rst:77`），IRQ-safe 时在原子上下文 |
| 5 | suspend 只改标志不停数据源 | **假省电**：状态 `suspended`、实际还在采样 | 文档明说"core regards the device as suspended... need not mean it has been put into a low power state"（`Documentation/power/runtime_pm.rst:95`） |
| 6 | probe 里忘了 `put_autosuspend()` | 永不挂起（同 #1） | 引用没有配平 |
| 7 | remove 里不 `pm_runtime_disable()` | 拆卸过程中 resume 回调把定时器/中断重新启动 → use-after-free | 见 §2.6 |
| 8 | `pm_runtime_set_active()` 放在 `enable()` 之后 | 多一次无意义 resume；状态机时序错 | 见 §2.4 |
| 9 | 直接调 `pm_runtime_suspend()` 而不是让核心调度 | 绕过引用计数与子设备检查；与系统睡眠竞争的窗口 | 文档推荐用 helper 而不是自定义流程 |
| 10 | 把 PM 状态挂在软件接口设备上（读错 sysfs） | 检查恒真/恒 `unsupported` | 见 §1.5 |

### 2.6 remove 里 `disable` 与停中断/定时器的顺序

**本项目对契约做了一次有意偏离并记录在案**：任务书写的是"先停 hrtimer/中断，再
`pm_runtime_disable()`"，实现改成**先 `disable()`**（`driver/sensor_char.c:1537`，
内存释放在 `:1557`）：

```c
static void sensor_remove(struct i2c_client *client)
{
	...
	pm_runtime_disable(&client->dev);   /* ① 先关 PM，并同步等待正在执行的回调结束 */
	hrtimer_cancel(&sd->timer);         /* ② 停采样源 */
	free_irq(sd->virq, sd);             /* ③ 摘中断 */
	...
	free_pages(sd->shm_addr, 0);        /* ④ 最后释放内存 */
	kfifo_free(&sd->ring);
}
```

理由（推理链，不是"时序侥幸"）：

1. `pm_runtime_disable()` 的语义（`drivers/base/power/runtime.c:1484`，`__pm_runtime_disable()`）：
   - `disable_depth++`：此后的 PM 请求一律返回 `-EACCES`（`rpm_check_suspend_allowed()`），
     不会再有新的 suspend/resume 进来；
   - `if (!dev->power.disable_depth++) __pm_runtime_barrier(dev)`：**同步等待**正在执行的
     PM 回调结束，并把 `last_status` 记下来。
2. 如果反过来（先 `hrtimer_cancel/free_irq`，最后才 `disable`），在"停完定时器"到
   "disable 生效"之间存在一个窗口：核心仍可能回调 `sensor_runtime_resume()`。
   resume 里会 `hrtimer_start()`（`driver/sensor_char.c:1294`），
   **把采样在"正在拆卸设备"的过程中原地重新启动**，随后 `kfifo_free()` 释放的缓冲
   还会被中断线程写入 → 内存破坏。
3. `disable()` 在最前面是**结构性**地排除这个窗口，而不是靠"大概率撞不上"。

**内存释放必须在停生产者之后**：`free_pages(shm)` / `kfifo_free(ring)` 放在
`hrtimer_cancel + free_irq` 之后（`driver/sensor_char.c:1557`），否则中断下半部
还可能往已释放的缓冲写数据。这条顺序原则与阶段 01 的
"`release()` 里摘 fasync / 关队列 / free 的顺序"是同一类问题。

> **面试时怎么讲这条偏离**：不要说"随便放的"。正确讲法是
> "`pm_runtime_disable()` 是唯一能保证 PM 回调不会再进来的操作，
> 所以它必须在任何'拆资源'动作之前；停中断/定时器是第二步。
> 契约原本的顺序能工作，但存在一个内核仍可回调 resume 的窗口，
> 属于结构性缺陷，不是健壮性问题。"

---

## 3. suspend/resume 的实现细节

### 3.1 进入低功耗前必须停止数据源（hrtimer / 中断）

这是本阶段最重要的正确性要求。内核文档（`Documentation/power/runtime_pm.rst:95`）写得很明白：

> Once the subsystem-level suspend callback ... has completed successfully ...
> the PM core regards the device as suspended, **which need not mean that it has
> been put into a low power state**. It is supposed to mean, however, that the
> device **will not process data and will not communicate with the CPU(s) and RAM**
> until the appropriate resume callback is executed for it.

也就是说：**`suspended` 的契约是"不再处理数据、不再和 CPU/内存通信"**。
只改状态标志而不停数据源，就是违反契约——文档层面的"假省电"。

本项目这里的"数据源"有两层（`driver/sensor_char.c:1269`）：

```c
hrtimer_cancel(&sd->timer);   /* ① 停定时器：同步等正在执行的 timer callback 结束 */
disable_irq(sd->virq);        /* ② 关中断线：真机上传感器可能自己报 DRDY */
WRITE_ONCE(sd->suspended, true);
```

**为什么必须"先定时器、后中断"**：
定时器回调 `sensor_timer_fn()` 会调 `generic_handle_domain_irq()`
（`driver/sensor_char.c:444`）去触发中断。先 `hrtimer_cancel()` 保证没有新的中断被排队，
再 `disable_irq()` 保证已排队的中断也不会进入 handler。
反过来的话，`disable_irq()` 之后定时器还可能再触发一次，在中断线已关的状态下
残留一个未处理的中断。

**为什么"光停定时器不够"**：真机上传感器有独立的数据就绪（DRDY/ALERT）引脚，
即使驱动不主动发起采样，芯片也会拉中断。所以"停中断线"是硬件层的保险。
（本项目的虚拟中断由定时器触发，两者效果叠加。）

**`hrtimer_cancel()` 的同步语义很关键**：它"同步等待正在执行的定时器回调结束"
（同时也是 `pm_runtime_disable()` 能安全工作的前提）。若改用
`hrtimer_try_to_cancel()`，回调可能还在另一个 CPU 上跑，
"停完之后还在采样"的窗口就真实存在了。

**验证项：挂起期间样本序号与中断计数必须完全冻结**
（`tests/phases/06-runtime-pm-kunit.sh:86` 起）。实测（同一日志）：

```text
      · 挂起 2 秒：seq 6 -> 6，irq 6 -> 6
[CHECK:PASS] 挂起期间样本序号冻结
[CHECK:PASS] 挂起期间中断计数冻结
```

这条检查的判别力来自"同时看 `seq` 与 `irq`"：`seq` 由中断下半部推进，
`irq` 由上半部累加。只看 `seq` 会漏掉"中断还在触发、只是样本被丢弃"的情况。

### 3.2 resume 的触发路径

resume 有两条触发路径，都最终落到 `sensor_runtime_resume()`：

| 触发方式 | 路径 | 本项目的使用 |
|---|---|---|
| **主动 get**（推荐） | `pm_runtime_resume_and_get()` → `__pm_runtime_resume(RPM_GET_PUT)` → `rpm_resume()` → 回调 | `open()`（`driver/sensor_char.c:513`）、debugfs `regs` 读（`driver/sensor_char.c:1148`） |
| **核心自动 resume** | 父设备 resume 时子设备跟随（`child_count` 逻辑） | 本项目无子设备，未触发 |
| **用户态写 `power/control`** | `auto`→`pm_runtime_allow()` | 本项目未用 |

本项目的 resume 实现（`driver/sensor_char.c:1278`）：

```c
static int sensor_runtime_resume(struct device *dev)
{
	...
	interval = READ_ONCE(sd->interval_ms);   /* ① 用"最新"周期，而不是旧值 */
	enable_irq(sd->virq);                    /* ② 先开中断 */
	WRITE_ONCE(sd->suspended, false);        /* ③ 再清标志 */
	hrtimer_start(&sd->timer, ms_to_ktime(interval), HRTIMER_MODE_REL);  /* ④ 启动采样 */
	...
}
```

顺序与 suspend 对称（中断先于定时器启动），保证"定时器第一次触发时中断线已经开着"。
**① 用 `interval_ms` 的当前值**是必需的：挂起期间用户态可能改过周期，
而那时 `sensor_apply_interval()` 只能"记账"（§3.4）。resume 时按最新值生效，
不会丢设置——实测：

```text
[    9.835857] sensor_char 0-0048: interval -> 200 ms (deferred: device runtime-suspended)
[   10.892665] sensor_char 0-0048: runtime resume: sampling restarted (interval=200ms)
```

### 3.3 用户态如何观察：`power/{control,runtime_status,runtime_suspended_time}`

三个属性由**同一套通用 sysfs 属性组**提供，任何 `struct device` 都有
（`drivers/base/power/sysfs.c:652` 的 `runtime_attrs[]`，包含
`runtime_status`、`control`、`runtime_suspended_time`、`runtime_active_time`、
`autosuspend_delay_ms`；由 `dpm_sysfs_add()` 合并到 `power/` 目录，
`drivers/base/power/sysfs.c:694`）。

| 属性 | 读什么 | 写什么 | 本项目实测值 |
|---|---|---|---|
| `power/control` | `auto` / `on`（`sysfs.c:101`） | `auto`→`pm_runtime_allow()`；`on`→`pm_runtime_forbid()`（`sysfs.c:108`） | `auto` |
| `power/runtime_status` | `active`/`suspended`/`suspending`/`resuming`/**`unsupported`**（`sysfs.c:150`） | 只读 | 空闲→`suspended`；持有 fd→`active` |
| `power/runtime_suspended_time` | 累计挂起毫秒数（`sysfs.c:137`，来自 `pm_runtime_suspended_time()`，`runtime.c:119`） | 只读 | `9076ms` |
| `power/runtime_active_time` | 累计活跃毫秒数 | 只读 | （同源统计） |
| `power/autosuspend_delay_ms` | 延迟毫秒（`sysfs.c:182`） | 可写（`sysfs.c:192`） | `1000` |

**`runtime_suspended_time` 为什么是"真的进过低功耗"的最硬证据**：
它不是驱动写的，是 **PM 核心**在状态切换时打的时间戳累积的
（`update_pm_runtime_accounting()`，`drivers/base/power/runtime.c:65`；
在 `__pm_runtime_disable()` 里也会先更新一次，`runtime.c:1512`）。
驱动即使想造假也造不出来。本项目实测 `runtime_suspended_time = 9076ms`
（`logs/20260914-135029-main-06-runtime-pm-kunit.log`）。

**注意 `/sys/class/.../power/control` 与硬件设备的 是两份不同的属性文件**
（每个 device 一套），不要混（§1.5）。本项目测试用变量把路径拼出来
（`tests/phases/06-runtime-pm-kunit.sh:39`）：

```sh
I2CDEV=$(ls -d /sys/bus/i2c/devices/*-0048 2>/dev/null | head -1)
PWR=$I2CDEV/power
```

### 3.4 sysfs 写参数在挂起时"只记账、resume 生效"

这是个很能体现设计意识的点。`interval_ms` 有两个入口（sysfs 与 ioctl），
它们共用同一个"范围判定 + 生效动作"（`driver/sensor_char.c:794`，
`sensor_apply_interval()`）：

```c
static void sensor_apply_interval(struct sensor_dev *sd, unsigned long ms)
{
	...
	mutex_lock(&sd->lock);
	sd->interval_ms = ms;          /* ① 先记账：无论挂起与否都记住新值 */
	mutex_unlock(&sd->lock);

	if (READ_ONCE(sd->suspended)) {
		dev_info(dev, "interval -> %lu ms (deferred: device runtime-suspended)\n", ms);
		return;                 /* ② 挂起中：只记账，不启动定时器 */
	}

	hrtimer_cancel(&sd->timer);    /* ③ 活跃中：取消后按新周期重启 */
	hrtimer_start(&sd->timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
}
```

**为什么挂起期间不能顺手把定时器启动起来**（这是设计的关键）：

> `runtime_status` 是"设备是否省电"的契约。如果挂起期间写 sysfs 把定时器启动，
> 就出现了"状态说 `suspended`、实际在采样"的假省电，而且**用户态完全看不出来**。
> 更糟的是，这种"写个参数就把设备弄醒"的副作用会让 `runtime_suspended_time`
> 停止增长——一个本该无关的操作破坏了电源管理。

所以语义被定义成：**写参数永远成功（返回写入字节数），但生效时机是"下一次 resume"**。
实测：

```text
[    9.835857] sensor_char 0-0048: interval -> 200 ms (deferred: device runtime-suspended)
[CHECK:PASS] 挂起期间写 interval_ms 不会把设备唤醒
[CHECK:PASS] 驱动记录了被延迟的周期设置
[   10.892665] sensor_char 0-0048: runtime resume: sampling restarted (interval=200ms)
[CHECK:PASS] 挂起期间的设置已生效（200ms）
```

注意 sysfs 的 `store` 回调**不会**取 PM 引用——这正是它能"只记账"的原因。
对比 debugfs 的 `regs` 读（§3.5）：它需要真实 I²C 事务，所以必须取引用、
必须把设备唤醒。**"要不要唤醒"取决于操作是否真的需要硬件参与**，
这就是区分"记账型"和"I/O 型"用户接口的判据。

### 3.5 调试接口也要遵守电源管理（本项目顺手修的真实缺陷）

`debugfs/regs` 会发起 I²C 事务读寄存器。真机上设备挂起时总线接口可能是关的
（读不到或读到无意义值），所以该接口先 `pm_runtime_resume_and_get()`、读完归还
（`driver/sensor_char.c:1148` 与 `:1164`）：

```c
ret = pm_runtime_resume_and_get(dev);
if (ret < 0) { /* 如实展示错误码，不显示假值 */ }
for (reg = SENSOR_REG_TEMP; reg <= SENSOR_REG_CONFIG; reg++)
	ret = regmap_read(sd->regmap, reg, &val);
pm_runtime_mark_last_busy(dev);
pm_runtime_put_autosuspend(dev);
```

**"否则'看现场'这个动作本身就会破坏现场"**（读到假数据）。
这是一句很好的面试话术：调试接口读的是**硬件状态**，硬件状态的前提是**硬件通电**。

---

## 4. KUnit：在模块里写内核单元测试

### 4.1 KUnit 与用户态测试 / kselftest 的区别

三个层次要分清（面试常被要求"说出差别"）：

| 维度 | **KUnit** | **kselftest** | **本项目 QEMU 阶段测试** |
|---|---|---|---|
| 代码跑在哪 | **内核态**（测试与被测代码同地址空间） | 用户态（`tools/testing/selftests/`） | 用户态 + 内核态混合 |
| 测什么 | 内核内部函数/子系统（白盒） | 系统调用、用户态可见 ABI（黑盒为主） | 整条驱动链路（集成） |
| 类型系统 | 内核类型/宏（`KUNIT_EXPECT_EQ` 等） | 用户态 C/Python/shell | busybox shell + C 程序 |
| 运行环境 | 任意内核（含真实硬件），可 built-in 或模块 | 需要一个**已启动**的系统 | 需要完整驱动的 QEMU 环境 |
| 输出格式 | KTAP（`ok 1 <name>`） | TAP/自定义 | 自定义 `[CHECK:PASS]` 协议 |
| 想测"边界值" | **最容易**（直接构造参数） | 难（要构造能触发边界的系统状态） | 几乎不可能（边界值很难自然出现） |
| 想测"并发/时序" | 不适合（没有真实调度压力） | 可以（多线程/多进程） | 可以（阶段 03 的并发测试） |
| 想测"硬件时序" | **不能** | 不能（模拟器里近似） | 部分（QEMU 里可观察） |

**KUnit 的核心定位**（`Documentation/dev-tools/kunit/index.rst:42`）是
"KUnit follows the white-box testing approach. The test has access to internal
system functionality... KUnit runs in kernel space and is not restricted to things
exposed to user-space"——**它就是用来测"用户态看不见的东西"的**。

一个常被追问的点：**KUnit 不能替代 kselftest**。
KUnit 测函数逻辑，kselftest 测 ABI 契约。本项目里：
`interval_ms` 的**范围判定逻辑**由 KUnit 测（§5），
而"写 0 被拒绝、写 60001 被拒绝"这条**用户可见契约**由阶段 04 的 sysfs 测试测
（`logs/20260914-135048-main-04-sysfs-debugfs.log` 里有对应 PASS）。
两者**必须都存在**，因为前者能测所有边界组合，后者才能证明"用户态真的会被拒绝"。

### 4.2 在（外部）模块里怎么写

`driver/sensor_kunit.c`（编译成独立模块 `sensor_kunit.ko`）。核心骨架：

```c
#include <kunit/test.h>
#include <linux/module.h>
#include "sensor_calc.h"

static void sensor_raw_to_milli_boundaries(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 0, sensor_raw_to_milli(0x0000));
	KUNIT_EXPECT_EQ(test, -128000, sensor_raw_to_milli(0x0800));
	...
}

static struct kunit_case sensor_calc_test_cases[] = {
	KUNIT_CASE(sensor_raw_to_milli_boundaries),
	...
	{}
};

static struct kunit_suite sensor_calc_test_suite = {
	.name = "sensor_calc",
	.test_cases = sensor_calc_test_cases,
};

kunit_test_suite(sensor_calc_test_suite);   /* 注册到 .kunit_test_suites 段 */
MODULE_LICENSE("GPL");
```

**四个要点**：

1. **`kunit_test_suite()` 做的事**：把 suite 放进 ELF 的 `.kunit_test_suites` 段
   （`include/kunit/test.h:351` 的 `__kunit_test_suites`，见 `:370` 起）。
   KUnit executor 扫描这个段来发现测试。这也是"测试必须关联到 suite 才会跑"的原因
   （`Documentation/dev-tools/kunit/usage.rst:172`）。
2. **用例数组必须以 `{}` 结尾**（空 `kunit_case` 作为哨兵）。
3. **built-in 还是模块**：built-in 测试在内核 boot 时（`late_init` 之后）自动跑；
   模块测试在 **`insmod` 时**跑（`Documentation/dev-tools/kunit/run_manual.rst:48`）。
   本项目选模块：不污染正常功能，测试脚本按需 `insmod`
   （`driver/Makefile` 的注释也写了这点）。
4. **`.ko` 能不能用 KUnit 符号**：KUnit 的符号是 `EXPORT_SYMBOL_GPL`，
   本模块是 `MODULE_LICENSE("GPL")`，所以外部模块可以直接编、直接 `insmod`，
   不需要把测试编进内核。本项目实测 `insmod /lib/modules/6.6.156/sensor_kunit.ko`
   成功（日志中 `KTAP version 1` 输出）。

### 4.3 TAP / KTAP 输出怎么读

本项目实测输出（`logs/20260914-135029-main-06-runtime-pm-kunit.log`）：

```text
[   16.000405] KTAP version 1
[   16.000544] 1..1
[   16.000771]     KTAP version 1
[   16.000858]     # Subtest: sensor_calc
[   16.001247]     # module: sensor_kunit
[   16.001414]     1..5
[   16.002475]     ok 1 sensor_raw_to_milli_boundaries
[   16.002782]     ok 2 sensor_raw_to_milli_chip_range
[   16.003166]     ok 3 sensor_raw_to_milli_ignores_high_bits
[   16.003546]     ok 4 sensor_interval_valid_bounds
[   16.003956]     ok 5 sensor_fifo_next_wraps
[   16.004094] # sensor_calc: pass:5 fail:0 skip:0 total:5
[   16.004217] # Totals: pass:5 fail:0 skip:0 total:5
[   16.004373] ok 1 sensor_calc
```

逐行含义（对照 `Documentation/dev-tools/ktap.rst`）：

| 行 | 含义 |
|---|---|
| `KTAP version 1` | KTAP 版本行（`ktap.rst:41`；本项目内核实测输出 KTAP v1，不是 TAP 13） |
| `1..1` / `1..5` | 测试计划：**本层**有几个子测试（`ktap.rst:56`） |
| `# Subtest: sensor_calc` | 子测试名字（嵌套）。**判"这一版用例真的跑了"就靠它** |
| `# module: sensor_kunit` | 用例来自哪个模块（模块化 KUnit 专有） |
| `ok 1 sensor_raw_to_milli_boundaries` | 用例 1 通过。格式是 `ok <序号> <名字>`（`ktap.rst:120`） |
| `# sensor_calc: pass:5 fail:0 skip:0 total:5` | 套件汇总 |
| `# Totals: ...` | 全局汇总 |
| `ok 1 sensor_calc` | 外层把整个套件当一个测试，通过 |

**关键陷阱（本项目真实踩过）**：**KTAP 是 `ok 1 <名字>`，用例名与序号之间是空格，没有 `-`。**
`ok 1 - <名字>` 是 TAP 13 的写法（`ktap.rst:30` 说明 KTAP 与 TAP 的分歧）。
第一版脚本照 TAP 13 写模式匹配，于是 5 条用例全部"假失败"：

```text
[   10.974792]     ok 1 sensor_raw_to_milli_boundaries
[CHECK:FAIL] 零点用例通过 -- 在 /tmp/kunit.txt 中未找到 'ok 1 - sensor_raw_to_milli_boundaries'
```

**判定 KUnit 结果要两条一起看**（本项目脚本的做法，
`tests/phases/06-runtime-pm-kunit.sh:148` 起）：

1. **逐条匹配用例名**（证明**这一版**用例跑了，不是 dmesg 里恰好有 `ok` 字样）；
2. **看汇总行 `pass:5 fail:0` + `not ok` 计数为 0**
   （防止"只看有 ok"被蒙混过关——旧版本的 `ok` 行会留在缓冲里）。

**另一个可用的观察通道**：本内核 `CONFIG_KUNIT_DEBUGFS=y`（内核树
`.config` 中该项为 y），结果同时暴露在 debugfs
（`Documentation/dev-tools/kunit/run_manual.rst:56`）：

```sh
cat /sys/kernel/debug/kunit/sensor_calc/results
```

（`init.sh` 已挂载 debugfs；本项目没有用这条路径，但它是 KUnit 标准接口。）

### 4.4 `kunit.filter_glob` 的用法

KUnit executor 提供几个 boot 期参数（`lib/kunit/executor.c:18,34,37,40`，
都是 `module_param_named`，所以在内核命令行写作 `kunit.<name>=`）：

| 参数 | 作用 | 值 |
|---|---|---|
| `kunit.filter_glob` | **按名字**过滤 suite/test | `<suite_glob>[.<test_glob>]`，支持 `*`（`executor.c:35`） |
| `kunit.filter` | **按属性**过滤（`speed>slow`、`module=xxx`） | 见 `executor.c:37` |
| `kunit.filter_action` | 被过滤掉的测试是跳过还是仍执行 | `skip` 等（`executor.c:40`） |
| `kunit.action` | `list`（只列名）/ `list_attr` | `executor.c:18` |
| `kunit.enable` | 总开关（默认 1，见 `Documentation/admin-guide/kernel-parameters.txt:2560`） | 0/1 |

**用法示例**（本项目套件名是 `sensor_calc`，用例名如 `sensor_fifo_next_wraps`）：

```sh
# 只跑 sensor_calc 这个套件：
kunit.filter_glob=sensor_calc

# 只跑 sensor_calc 下名字含 fifo 的用例（注意分隔符是 "."）：
kunit.filter_glob=sensor_calc.sensor_fifo_next*

# 只列不跑：
kunit.action=list
```

**格式要点**（`Documentation/dev-tools/kunit/running_tips.rst:28`）：
`"<suite_glob>[.test_glob]"`，**suite 与 test 之间用点号分隔**，
`*` 通配。`kunit.py` 的命令行位置参数是同一个 glob
（`tools/testing/kunit/kunit.py:373`）：

```sh
./tools/testing/kunit/kunit.py run 'sensor_calc*'
./tools/testing/kunit/kunit.py run 'sensor_calc*.sensor_fifo*'
```

`executor.c` 里 `kunit_parse_glob_filter()`（`lib/kunit/executor.c:69`）
会把 `suite.test` 拆成两段分别用 `glob_match()` 匹配——所以
**套件名里不能含点**（否则会被误拆）。

**本项目为什么没用 `filter_glob`**：测试模块只有一个套件，
且脚本通过 `insmod` 触发（模块加载时执行所有用例）。
`filter_glob` 的价值在"一个大内核里有成百上千套件，只想跑自己那部分"。
**未验证**：本项目没有实测过给模块传 `kunit.filter_glob` 的行为。

### 4.5 KUnit 在真实内核社区的使用（含 CI）

**规模**：在当前内核树里，用 `kunit_test_suite()` 注册测试的 `.c` 文件有
**101 个**（`grep -rln "kunit_test_suite" --include=*.c`，本内核树实测统计），
分布靠前的是：`drivers/gpu`（19）、`lib/kunit`（6）、`drivers/base`（5）、
`drivers/hid`（4）、`drivers/fpga`（3）、`arch/s390`（3），
以及 `mm/kasan` / `drivers/iio` / `drivers/clk` / `net/mptcp` 等。
即 **KUnit 已经不只是"测试框架自己"，而是被 GPU、HID、IIO、clk、mm 等子系统真正采用**。

**子系统自带配置**：内核树里有 **16 个 `.kunitconfig`**
（`find -name .kunitconfig`），例如
`fs/ext4/.kunitconfig`、`kernel/kcsan/.kunitconfig`、`mm/kfence/.kunitconfig`、
`drivers/base/test/.kunitconfig`、`drivers/gpu/drm/tests/.kunitconfig`、
`net/handshake/.kunitconfig`。它们就是各子系统给 CI 用的"只跑我这部分测试"的最小配置
（`Documentation/dev-tools/kunit/run_wrapper.rst:53`）：

```sh
./tools/testing/kunit/kunit.py run --kunitconfig=fs/ext4/.kunitconfig
```

**维护者与列表**：MAINTAINERS 里 KUnit 是独立条目
（`MAINTAINERS:11485` `KERNEL UNIT TESTING FRAMEWORK (KUnit)`），
维护者 Brendan Higgins / David Gow，邮件列表 `linux-kselftest`、
`kunit-dev@googlegroups.com`，代码树 `shuah/linux-kselftest.git` 的 `kunit` 分支。
**注意 KUnit 与 kselftest 共用同一个列表 `linux-kselftest@vger.kernel.org`**
——社区把它们当成"一套测试体系的两个层次"。

**CI 形态**（社区共识 + 文档证据）：

- KUnit 的主要执行者就是 `kunit_tool`
  （`tools/testing/kunit/kunit.py`）。它**自带**"配置内核 → 编译 → 在 QEMU/UML 里跑 →
  解析 KTAP → 打印结果"的完整流水线
  （`Documentation/dev-tools/kunit/index.rst:46` 起），
  所以它天生适合放进 CI 脚本，无需额外测试驱动。
- 各子系统把 `.kunitconfig` 提交到仓库，CI 里跑
  `kunit.py run --kunitconfig=<子系统>`；上游还有统一的
  `tools/testing/kunit/configs/default.config` 作为默认基线
  （`Documentation/dev-tools/kunit/run_wrapper.rst:81`）。
- 局部执行的默认入口（社区 CI 的常见写法）是：

  ```sh
  ./tools/testing/kunit/kunit.py run --timeout=30 --jobs=$(nproc)
  ```

  （`Documentation/dev-tools/kunit/run_wrapper.rst:35`）
- **KUnit 默认不是为了生产系统**：文档明确警告
  "KUnit is not designed for use in a production system"
  （`Documentation/dev-tools/kunit/run_manual.rst:11`）。
  所以本项目的做法（测试模块独立、按需 `insmod`、日常不加载）与社区一致。

---

## 5. 为什么把纯逻辑抽成 `static inline` 头文件

### 5.1 三类代码与可测性

驱动里的代码可以分成三类（`driver/sensor_calc.h` 文件头注释）：

| 类别 | 例子 | 能不能 KUnit 测 |
|---|---|---|
| ① 内核交互 | `regmap_read`、`request_threaded_irq`、`file_operations` | **不能**（需要真实子系统/硬件） |
| ② **纯计算** | 12 位补码换算、参数合法性、环形下标推进 | **最适合** |
| ③ 策略/时序 | 何时挂起、何时丢样本、竞态顺序 | 不适合（需要真实调度） |

第 ② 类被抽进 `driver/sensor_calc.h`，用 `static inline` 实现：

```c
static inline s32 sensor_raw_to_milli(u16 raw);              /* 12 位补码 -> 毫摄氏度 */
static inline bool sensor_interval_valid(unsigned long ms);  /* 1..60000 */
static inline u32 sensor_fifo_next(u32 idx, u32 cap);        /* 环形下标推进 */
```

**为什么是 `static inline` 头文件，而不是一个 `.c`**：

1. **对驱动零开销**：编译器内联，不引入跨编译单元的调用与符号
   （传感器采样路径在中断线程里，任何一次函数调用都是成本）。
2. **不用为几个函数多编一个模块**，也就不需要维护 `EXPORT_SYMBOL` 与模块依赖
   （驱动和测试模块都能直接 `#include`）。
3. **驱动与 `sensor_kunit.ko` include 同一份实现** ——
   测试测的就是驱动实际执行的代码，不存在"测试一份、跑另一份"的漂移。
   **这是单元测试有意义的前提**：如果抽取后驱动还留着一个旧实现，
   KUnit 全绿也只能证明"那份复制品是对的"。

**约束（写在文件头）**：只能放**无副作用、不依赖内核子系统**的函数。
一旦需要睡眠、锁、寄存器访问，就不属于这个头文件。

### 5.2 边界选择：这三条函数为什么值得测

| 函数 | 最容易写错的地方 | 真机上的表现 |
|---|---|---|
| `sensor_raw_to_milli()` | **负温度补码**：12 位符号扩展必须做 | `0x0FFF` 被算成 +255.9 ℃ 而不是 −0.0625 ℃ |
| `sensor_interval_valid()` | 端点与溢出：`ULONG_MAX` 是否被误判合法 | 用户写一个极大值导致定时器/除法异常 |
| `sensor_fifo_next()` | **回绕 off-by-one**：`cap-1 → 0` | 数据偶尔错位一个样本，极难复现 |
| （换算的隐含约定） | 高 4 位状态位参与算术 | 某些状态位组合下温度跳变 |

本项目在实现 KUnit 的过程中还**修正了一个真实语义错误**：
原实现用宏 `RAW_TO_MILLI(raw) = (s32)raw * 1000 / 16`（按 16 位补码解释），
现在按芯片约定实现为**低 12 位补码**。对芯片实际输出（24.0~26.0 ℃ → raw 384~416）
两者结果一致（所以之前从未暴露），但 `0x0FFF` 这类"高位为 0、低 12 位为负"的值
只有 12 位版本才正确。KUnit 用例 `sensor_raw_to_milli_ignores_high_bits`
把"高 4 位不参与算术"这条约定**显式测了出来**。

### 5.3 单元测试能覆盖什么 / 不能覆盖什么（诚实清单）

| **能覆盖**（实测 5 个用例） | **不能覆盖**（必须靠集成测试） |
|---|---|
| 12 位补码正/负边界：`0x0000/0x07FF/0x0800/0x0FFF` | regmap/I²C 传输是否真的正确（需要真实总线） |
| 换算系数（24.0 ℃→24000、26.0 ℃→26000、25.3125 ℃→25312） | 中断上下文约束、回调能否睡眠 |
| 高 4 位状态位不参与算术（`0x8190`/`0xF190` 与 `0x0190` 同值） | Runtime PM 的**时序**（挂起/恢复只能在 QEMU 集成测试里验） |
| 周期端点与越界（0/1/60000/60001/`ULONG_MAX`） | 并发正确性（需要 KCSAN/真实调度压力） |
| 环形下标推进、回绕、容量为 0 的防御分支 | 真实硬件时序（转换时间、总线毛刺、上电延迟） |

**特别说明"有副作用的函数为什么测不了"**：`sensor_shm_publish()`
（`driver/sensor_char.c:310`）会写共享内存、`udelay`、拿自旋锁，
它有副作用、依赖调用上下文，**不属于纯逻辑**，所以留在阶段 03 的 QEMU 集成测试
（放大器实验 + KCSAN）里覆盖。这是刻意的取舍，不是遗漏。

---

## 6. 面试问答

> 每个回答都给了"可验证的落点"（源码行号或实测日志），方便被追问时展开。

### Q1. Runtime PM 和系统 suspend（suspend-to-RAM）有什么区别？

**作用范围**：runtime PM 针对**单个设备**，系统运行时反复进出；系统 suspend 是
**整机**低功耗状态，用户态被冻结（`Documentation/admin-guide/pm/sleep-states.rst:13`）。
**回调集**：`SET_RUNTIME_PM_OPS` vs `SET_SYSTEM_SLEEP_PM_OPS`
（`include/linux/pm.h:363` vs `:342`）。**编译保护**：`CONFIG_PM` vs `CONFIG_PM_SLEEP`。
**触发者**：runtime PM 由引用计数归零 + autosuspend 延迟触发；系统 suspend 由
用户态写 `/sys/power/state` 触发。
**联系**：系统 resume 时内核会**一律把设备恢复 full power**，再用
`pm_runtime_disable/set_active/enable` 同步状态
（`Documentation/power/runtime_pm.rst:682`）。本项目只做了 runtime PM，
没做系统睡眠，理由是不写没验证过的路径。

### Q2. 驱动里漏了 `pm_runtime_put()` 会怎样？

设备**永远不会挂起**，而且**不会有任何报错或告警**——这是最难发现的一类错误。
根因：`usage_count` 永不归零，`rpm_check_suspend_allowed()` 在
`usage_count != 0` 时直接返回 `-EAGAIN`（`drivers/base/power/runtime.c:265`），
于是 `runtime_suspend` 回调永远不会被调用。表面上功能正常（采样照跑、sysfs 正常），
只有功耗不对。**验证手段**：反复 open/close 后看 `runtime_status` 是否回到 `suspended`
——本项目专门有这个检查项（`tests/phases/06-runtime-pm-kunit.sh:136`），实测通过。
如果是 `pm_runtime_get_sync()` 失败路径，情况更隐蔽：`get_sync` **出错也不退还引用**
（`include/linux/pm_runtime.h:414` 的文档注释），所以推荐用
`pm_runtime_resume_and_get()`（失败自动 `put_noidle`，`:432`）。

### Q3. `runtime_suspend` 回调里能不能睡眠？

**默认可以**。回调在**进程上下文、中断使能**状态下执行
（`Documentation/power/runtime_pm.rst:77`："By default, the callbacks are always
invoked in process context with interrupts enabled"）。
**但**如果驱动调用过 `pm_runtime_irq_safe()`，回调就变成原子上下文，
不能再睡眠（同一段文档）。另外 `get_sync/put_sync` 这类**同步**入口自带
`might_sleep_if(...)`（`drivers/base/power/runtime.c:1163`），在原子上下文调用会被抓。
本项目 `sensor_runtime_suspend()` 里用了 `hrtimer_cancel()` 和 `disable_irq()`，
两者都可能睡眠/等待，**只有在默认（进程上下文）前提下才合法**——
代码注释（`driver/sensor_char.c:1250`）特意写了这一点。

### Q4. 怎么验证设备真的进了低功耗，而不是"假省电"？

四层证据，缺一不可（本项目全部实测）：

1. **`runtime_status == suspended`** —— 这只是"核心认为"，最弱。
2. **数据源冻结**：挂起期间采样序号与中断计数完全不变
   （实测 `seq 6 -> 6`、`irq 6 -> 6`，2 秒窗口）。
   这条才证明"设备不再处理数据"，对应文档对 `suspended` 的定义
   （`Documentation/power/runtime_pm.rst:95`）。
3. **`runtime_suspended_time` 增长**：由 PM 核心统计（`runtime.c:65`），
   驱动造不了假。实测 `9076ms`。
4. **`runtime_resume` 后能恢复**：`seq` 继续推进、周期设置生效
   （实测 resume 后 1 秒内 `seq 11 -> 16`）。
   只证明"能停"不证明"能醒"，必须同时验证。

反面教材就写在文档里：**"core regards the device as suspended,
which need not mean that it has been put into a low power state"**
（`Documentation/power/runtime_pm.rst:95`）——只改标志就是假省电。

### Q5. KUnit 与 kselftest 有什么区别？

KUnit 跑在**内核态**、测**内部函数**（白盒），输出 KTAP，能 built-in 或做成模块，
不需要完整用户态系统（`Documentation/dev-tools/kunit/index.rst:42`）；
kselftest 跑在**用户态**、测**系统调用/ABI 契约**（黑盒），需要一个已启动的系统。
两者共用 `linux-kselftest@vger.kernel.org` 列表，社区视为一套测试体系的两层。
**不能互相替代**：KUnit 测不了 ABI（它不经过 syscall 层），kselftest 很难测边界组合
（要构造触发边界的系统状态）。本项目里 `sensor_interval_valid()` 的**逻辑**归 KUnit，
"sysfs 写 0 被 `-EINVAL` 拒绝"这条**用户可见契约**归阶段 04 的集成测试。

### Q6. 为什么 PM 的 sysfs 节点不在字符设备上？

Runtime PM 描述的是"**这颗硬件是否上电/是否在低功耗**"，必须挂在**硬件设备**
（`i2c_client`）上；字符设备 `/dev/sensor0` 只是给用户态的软件接口，
没有电源、没有寄存器。实测：`/sys/bus/i2c/devices/0-0048/power/runtime_status`
是 `active/suspended`，而 `/sys/class/sensor_char/sensor0/power/runtime_status`
恒为 `unsupported`。`unsupported` 的来源是 `disable_depth != 0`
（`drivers/base/power/sysfs.c:157`），而每个 device 的 `disable_depth` 初始就是 1
（`drivers/base/power/runtime.c:1802`）——只有调用过 `pm_runtime_enable()` 的
设备才会变成 0。**追问"那 `control` 呢"**：`control` 默认对所有设备都是 `auto`
（`runtime_auto = true`，`runtime.c:1809`），所以它不能当"PM 已启用"的判据，
否则会得到一个恒真的弱检查。

### Q7. autosuspend 是什么？为什么需要它？

"延迟挂起"。改变电源状态本身有代价（上电、总线重新初始化、寄存器重写），
设备刚空闲就立刻挂起会导致"抖动"（bouncing），省的电不够切换开销。
机制：`mark_last_busy()` 记录最后使用时刻，`RPM_AUTO` 请求先检查
`pm_runtime_autosuspend_expiration()`（`drivers/base/power/runtime.c:163`），
没到延迟就重排定时器（`runtime.c:580`）。
术语的坑：autosuspend **不是**"自动挂起"（`Documentation/power/runtime_pm.rst:862`），
挂起请求还是子系统/驱动发的，它只是把挂起**延后**。
本项目延迟 1s，实测 close 到 suspend 间隔 1.021s。

### Q8. 挂起期间用户写 sysfs 参数会怎样？设计上应该怎么处理？

**取决于这个操作是否真的需要硬件参与**：
- **记账型**（改采样周期、改阈值）：应该**只记住，resume 时生效**，
  绝不为了生效而唤醒设备。本项目 `sensor_apply_interval()`（`driver/sensor_char.c:794`）
  先写 `interval_ms`，若 `suspended` 就打印 `deferred` 并返回
  （`driver/sensor_char.c:808`）。
  否则会出现"状态说挂起、实际在采样"的假省电，且 `runtime_suspended_time` 停止增长。
- **I/O 型**（读寄存器、写配置寄存器）：必须取 PM 引用唤醒设备
  （本项目 debugfs `regs` 用 `pm_runtime_resume_and_get()`，`driver/sensor_char.c:1148`），
  否则读到的是假数据。**"看现场"的动作本身会破坏现场**就是这个意思。

### Q9. `pm_runtime_get_sync()` 和 `pm_runtime_resume_and_get()` 有什么区别？

两者都"同步 resume + 引用 +1"，区别在**失败时的引用计数**：
`get_sync` **即使返回错误也保留引用**（`include/linux/pm_runtime.h:414` 的注释），
调用者必须自己补一个 put；`resume_and_get` 失败时自动 `pm_runtime_put_noidle()`
退还（`include/linux/pm_runtime.h:432`）。文档明确"Consider using
pm_runtime_resume_and_get() instead"。**为什么重要**：open() 里用 get_sync，
resume 失败 → open 返回错误 → 用户态没有 fd → 永远不调 release → 引用泄漏 →
设备永不挂起。本项目用 `resume_and_get`（`driver/sensor_char.c:513`）。

### Q10. 为什么 `remove()` 里 `pm_runtime_disable()` 要在停中断/定时器之前？

`pm_runtime_disable()` 会 `disable_depth++` 让后续 PM 请求返回 `-EACCES`，
并**同步等待正在执行的 PM 回调结束**（`__pm_runtime_disable()`，
`drivers/base/power/runtime.c:1484` 与其中的 `__pm_runtime_barrier()`）。
如果先停定时器、最后才 disable，两者之间存在窗口：核心仍可能回调
`runtime_resume()`，而 resume 会 `hrtimer_start()` 把采样**在拆卸过程中重启**，
随后 `kfifo_free()` 之后中断线程仍可能写已释放内存。
所以"disable 在前"是**结构性**排除竞态，而不是靠时序侥幸。
（本项目对任务书契约做了一处有意偏离并在实现文档记录；契约原顺序能工作，
但存在真实窗口。）

### Q11. 系统睡眠时如果设备已经 runtime-suspended，内核怎么处理？

系统 resume 时**一律恢复到 full power**：因为设备的 wakeup 设置/供电档位可能不同、
固件可能丢远程唤醒、子设备恢复需要父设备先上电、hibernation 恢复后驱动的状态
可能和硬件不一致等（`Documentation/power/runtime_pm.rst:659` 的列表）。
然后用 `pm_runtime_disable(dev)` → `pm_runtime_set_active(dev)` →
`pm_runtime_enable(dev)` 把 runtime PM 状态同步成真实状态（`:682`）。
PM 核心在调用 `->suspend()` 前加引用、`->resume()` 后减引用，
所以临时 disable runtime PM 不会永久丢掉挂起机会。

### Q12. KUnit 用例命名有什么讲究？为什么测试脚本要按用例名逐条匹配？

命名要"**有意义并能定位**"（`Documentation/dev-tools/kunit/style.rst`：
测试名描述被测行为）。本项目用 `sensor_raw_to_milli_boundaries`、
`sensor_fifo_next_wraps` 这类"函数_边界"命名。
脚本逐条匹配用例名（而不只是数 `ok` 行数）的理由是**防假绿**：
dmesg 是环形缓冲，旧版本的 `ok` 行可能还在；只看"有 ok"就会把
"这一版用例根本没跑"当成通过。再配合"汇总行 `pass:5 fail:0` + `not ok` 计数 0"，
就同时排除了"没跑"和"部分失败但脚本没看"两种假结论。
这是阶段 02 "告警检查必须在测试体之后取样"的同类教训。

---

## 7. 亲手验证

### 7.1 一键复现（与 `docs/10-开发与验证守则.md` 一致）

本工作树（06）的隔离命令（构建目录 `~/lab-06`，与其他工作树互不干扰）：

```bash
cd /Users/lucien/workspace/self-study/projects/wt-06

# ① 在虚拟机里编译模块 + 用户态程序 + 打包 initramfs（约 10~30s，不重编内核）
limactl shell dev bash -c 'WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/13-vm-fast-cycle.sh'

# ② 产物拷回宿主
WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/21-macos-sync-artifacts.sh

# ③ 阶段测试（QEMU，TCG + cortex-a72）
WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 06-runtime-pm-kunit 300

# ④ 回归（括号内为检查项数）
WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 04-sysfs-debugfs 300   # 42
WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 03-ringbuffer-mmap 300  # 40
WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 02-i2c-driver 300       # 19
WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 01-io-models 300       # 16
WORKTREE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh smoke 300              # 15
```

**一次构建可跑任意阶段**：`initramfs` 里打包了 `tests/runner/init.sh` 与全部
`tests/phases/*.sh`，运行时由内核命令行 `test=<阶段名>` 选择（`tests/runner/init.sh`）。
只有**改了源码才需要重跑 ①**。

本项目本轮实测（同一份产物，顺序执行）：

| 测试 | 结果 | 日志 |
|---|---|---|
| 06-runtime-pm-kunit | **31 PASS / 0 FAIL** | `logs/20260914-135029-main-06-runtime-pm-kunit.log` |
| 04-sysfs-debugfs | 42 PASS / 0 FAIL | `logs/20260914-135048-main-04-sysfs-debugfs.log` |
| 03-ringbuffer-mmap | 40 PASS / 0 FAIL | `logs/20260914-135054-main-03-ringbuffer-mmap.log` |
| 02-i2c-driver | 19 PASS / 0 FAIL | `logs/20260914-135106-main-02-i2c-driver.log` |
| 01-io-models | 16 PASS / 0 FAIL | `logs/20260914-135111-main-01-io-models.log` |
| smoke | 15 PASS / 0 FAIL | `logs/20260914-135122-main-smoke.log` |

合计 **163 项检查全绿**，无 panic、无 `WARNING:`/`Call trace:`。

> KCSAN 模式的内核树不要构建；若发现内核树处于 KCSAN 模式，等 60 秒重查
> （最多 10 分钟）。**绝不改 `~/kernel-build`**。

### 7.2 在 QEMU 里手动验证（`runtime_status` / KUnit）

initramfs 里带了 busybox（`/bin/sh`）。**用 `init=/bin/sh` 直接进 shell**
（比 `04-run-qemu-vm.sh` 的交互模式更可控：`INTERACTIVE=1` 只加 `nokaslr`，
`init.sh` 仍会自动跑完测试并 `poweroff`）：

```bash
# 在 macOS 上（wt-06 目录，产物已由 21-macos-sync-artifacts.sh 拷到 artifacts/）
cd /Users/lucien/workspace/self-study/projects/wt-06
qemu-system-aarch64 -M virt -cpu cortex-a72 -accel tcg,thread=multi -smp 2 -m 1G \
  -display none -serial stdio \
  -dtb artifacts/virt-sensor.dtb -kernel artifacts/Image -initrd artifacts/initramfs.cpio.gz \
  -append "console=ttyAMA0 init=/bin/sh"
```

进入 shell 后（下面的顺序即依赖顺序）：

```sh
# 1) 挂载文件系统 + 拿 KVER
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mkdir -p /sys/kernel/debug
mount -t debugfs none /sys/kernel/debug
KVER=$(uname -r)                      # 6.6.156

# 2) 按依赖顺序加载（virt_i2c 先：它注册带 of_node 的 i2c_adapter，
#    i2c 核心才会从设备树枚举 sensor@48 这个 client）
insmod /lib/modules/$KVER/virt_i2c.ko
insmod /lib/modules/$KVER/sensor_char.ko

# 3) 确认 PM 节点在硬件设备上（不是字符设备上）
I2CDEV=$(ls -d /sys/bus/i2c/devices/*-0048 | head -1)
echo "$I2CDEV"                         # 预期：/sys/bus/i2c/devices/0-0048
cat $I2CDEV/power/runtime_status       # 预期：active（probe 后 1 秒内）
cat $I2CDEV/power/control              # 预期：auto
cat $I2CDEV/power/autosuspend_delay_ms # 预期：1000

# 反例：字符设备的 power/ 是 unsupported（本文 §1.5 的核心证据）
cat /sys/class/sensor_char/sensor0/power/runtime_status   # 预期：unsupported
cat /sys/class/sensor_char/sensor0/power/control          # 预期：auto（弱判据，见 §1.2）

# 4) 等 autosuspend 到期（延迟 1s，给 3s 余量）-> 自动挂起
sleep 3
cat $I2CDEV/power/runtime_status            # 预期：suspended
cat $I2CDEV/power/runtime_suspended_time    # 预期：非 0，且随时间增长
dmesg | grep 'runtime suspend'              # 预期：sampling stopped (irq_count=2 seq=2)

# 5) open 持引用 -> 立刻 active + 采样恢复
exec 3<>/dev/sensor0
sleep 1
cat $I2CDEV/power/runtime_status            # 预期：active
dmesg | grep 'runtime resume'               # 预期：sampling restarted (interval=500ms)

# 6) close -> 1 秒后挂起；再过 2 秒看采样是否真的冻结
exec 3<&- ; exec 3>&-
sleep 3
cat $I2CDEV/power/runtime_status            # 预期：suspended
SEQ_A=$(cat /sys/class/sensor_char/sensor0/seq)
IRQ_A=$(sed -n 's/.* irq=\([0-9]*\) .*/\1/p' /sys/kernel/debug/sensor_char/stats)
sleep 2
SEQ_B=$(cat /sys/class/sensor_char/sensor0/seq)
IRQ_B=$(sed -n 's/.* irq=\([0-9]*\) .*/\1/p' /sys/kernel/debug/sensor_char/stats)
echo "seq $SEQ_A -> $SEQ_B ; irq $IRQ_A -> $IRQ_B"
# 预期：两对完全相同（本项目实测 "seq 6 -> 6，irq 6 -> 6"）——"真省电"的核心证据

# 7) 挂起期间写参数：只记账、不唤醒
echo 200 > /sys/class/sensor_char/sensor0/interval_ms
sleep 1
cat $I2CDEV/power/runtime_status            # 预期：仍是 suspended（没被弄醒）
dmesg | grep deferred                       # 预期：interval -> 200 ms (deferred: device runtime-suspended)

# 8) 重新 open：按新周期恢复（deferred 生效）
exec 3<>/dev/sensor0
sleep 1
cat /sys/class/sensor_char/sensor0/interval_ms  # 预期：200
dmesg | grep 'runtime resume'                   # 预期：sampling restarted (interval=200ms)
exec 3<&- ; exec 3>&-

# 9) KUnit：加载测试模块，看 KTAP 输出
insmod /lib/modules/$KVER/sensor_kunit.ko
sleep 1
dmesg | grep -A12 'KTAP version 1'
# 预期（本项目实测，注意没有 "-" 分隔符）：
#   KTAP version 1
#   1..1
#       KTAP version 1
#       # Subtest: sensor_calc
#       # module: sensor_kunit
#       1..5
#       ok 1 sensor_raw_to_milli_boundaries
#       ok 2 sensor_raw_to_milli_chip_range
#       ok 3 sensor_raw_to_milli_ignores_high_bits
#       ok 4 sensor_interval_valid_bounds
#       ok 5 sensor_fifo_next_wraps
#   # sensor_calc: pass:5 fail:0 skip:0 total:5
#   # Totals: pass:5 fail:0 skip:0 total:5
#   ok 1 sensor_calc

# KUnit 的 debugfs 通道（本内核 CONFIG_KUNIT_DEBUGFS=y）
cat /sys/kernel/debug/kunit/sensor_calc/results

# 10) 无告警
dmesg | grep -E 'WARNING:|Call trace:'    # 预期：无输出
poweroff -f
```

### 7.3 反例实验（验证"检查项真的能抓到问题"）

这四条是本项目实现记录里列出的负控实验（**未验证：本节给出方法与预期，
实际 FAIL 输出需自行跑出后记录**）。做法：改 `driver/sensor_char.c`（或
`sensor_calc.h`）→ 重跑 7.1 的 ①~③ → 观察对应检查项变 FAIL → 还原代码。

| 改动 | 预期失败项 | 说明 |
|---|---|---|
| 删掉 `release()` 里的 `pm_runtime_put_autosuspend` | "反复开关后仍能自动挂起" FAIL | 抓引用计数不平衡（Q2） |
| `sensor_runtime_suspend()` 里不 `hrtimer_cancel` | "挂起期间样本序号冻结" FAIL | 抓假省电（Q4） |
| `sensor_calc.h` 里把 `0x0800` 的符号处理写错 | `sensor_raw_to_milli_boundaries` `not ok` | 抓补码边界（§5.2） |
| `probe` 里去掉最后的 `pm_runtime_put_autosuspend` | "无使用者时自动挂起" FAIL | 抓 probe 引用未配平（§2.4） |

### 7.4 复现的边界

- QEMU 必须 `-accel tcg,thread=multi`；`-accel hvf -cpu host` 下 ftrace 会挂死
  （`docs/10-开发与验证守则.md` 第九节）。本项目测试脚本已固定
  `tcg,thread=multi -smp 2`：单核下 SMP 竞态不可触发。
- **autosuspend 是 1 秒的时序**，本项目的检查统一留 3 秒余量（`sleep 3`），
  不要用紧窗口"凑刚好通过"。
- **观察 `runtime resume` 日志前必须先让设备真的挂起**：第一版测试 open 得太早
  （probe 后立即 open，autosuspend 还没到期），根本没有一次 suspend，
  自然也没有 resume 日志（`logs/20260914-134436-...log` 里
  "dmesg 有 runtime resume 日志 FAIL" 就是这个原因之一）。
  正确顺序：**先 sleep 让挂起发生，再 open 触发 resume**。
- `runtime_suspended_time` 是**累计值**，不会清零；比较要看增量或看它增长。
- KUnit 结果匹配必须用 `ok .* <用例名>`（不是 `ok N - <用例名>`），见 §4.3。

---

## 附录 A：本文引用的内核源码/符号速查

### Runtime PM（`drivers/base/power/runtime.c`）

| 符号 | 行号 | 一句话 |
|---|---|---|
| `__rpm_get_callback()` | 21 | 回调优先级：pm_domain > type > class > bus > driver->pm |
| `update_pm_runtime_accounting()` | 65 | 累计 active/suspended 时间（`runtime_suspended_time` 的来源） |
| `pm_runtime_active_time()` / `pm_runtime_suspended_time()` | 114 / 119 | 给 sysfs 用的读取接口 |
| `pm_runtime_autosuspend_expiration()` | 163 | autosuspend 到期时间计算 |
| `rpm_check_suspend_allowed()` | 257 | **为什么没挂起**的唯一守门函数（-EACCES/-EAGAIN/-EBUSY） |
| `rpm_get_suppliers()` | 283 | 设备链接的 supplier 唤醒 |
| `__rpm_callback()` | 360 | 真正调用回调（含设备链接处理） |
| `rpm_callback()` | 426 | 回调包装；错误写进 `power.runtime_error` |
| `rpm_idle()` | 466 | idle 通知；无回调则转 `rpm_suspend(RPM_AUTO)` |
| `rpm_suspend()` | 558 | autosuspend 延迟判断在这里（`:580`） |
| `rpm_resume()` | 761 | 同步/异步 resume、等待正在进行的相反操作 |
| `pm_schedule_suspend()` | 1020 | 延迟挂起 |
| `rpm_drop_usage_count()` | 1052 | 引用 −1；负数打 `Runtime PM usage count underflow!`（`:1067`） |
| `__pm_runtime_suspend()` | 1122 | `pm_runtime_put_*` 的底层 |
| `__pm_runtime_resume()` | 1158 | `pm_runtime_get_*` 的底层；`might_sleep_if` 在 `:1163` |
| `__pm_runtime_set_status()` | 1286 | 直接设置 active/suspended 状态 |
| `__pm_runtime_disable()` | 1484 | disable + barrier（remove 顺序的关键） |
| `pm_runtime_enable()` | 1528 | `disable_depth--`；不平衡会打 `Unbalanced pm_runtime_enable!` |
| `pm_runtime_init()` | 1796 | 初值：`RPM_SUSPENDED`(:1798)、`disable_depth=1`(:1802)、`runtime_auto=true`(:1809) |

### Runtime PM 头文件（`include/linux/pm_runtime.h`）

| 符号 | 行号 | 语义 |
|---|---|---|
| `pm_runtime_get_noresume()` | 119 | +1，不 resume |
| `pm_runtime_mark_last_busy()` | 221 | 更新 `power.last_busy` |
| `pm_runtime_get_sync()` | 419 | 同步 resume + 引用（失败也保留引用） |
| `pm_runtime_resume_and_get()` | 432 | 同步 resume + 引用（失败自动退还，推荐） |
| `pm_runtime_put()` | 452 | −1；异步 idle 检查 |
| `pm_runtime_put_autosuspend()` | 464 | −1；归零后异步 autosuspend |
| `pm_runtime_put_sync()` | 483 | −1；同步挂起 |
| `pm_runtime_put_sync_autosuspend()` | 516 | −1；同步 + 尊重延迟 |
| `pm_runtime_set_active()` | 530 | 标记 active（probe 里必须在 enable 之前） |
| `pm_runtime_disable()` | 559 | → `__pm_runtime_disable(dev, true)` |
| `pm_runtime_use_autosuspend()` | 576 | 打开 autosuspend |
| `pm_runtime_set_autosuspend_delay()` | 88 | 设延迟（ms） |

### PM 宏与文档

| 符号 | 位置 | 说明 |
|---|---|---|
| `SYSTEM_SLEEP_PM_OPS` | `include/linux/pm.h:312` | 系统睡眠回调（suspend/freeze/poweroff…） |
| `SET_SYSTEM_SLEEP_PM_OPS` | `include/linux/pm.h:342` | 受 `CONFIG_PM_SLEEP` 保护 |
| `RUNTIME_PM_OPS` | `include/linux/pm.h:336` | 三个 runtime 回调 |
| `SET_RUNTIME_PM_OPS` | `include/linux/pm.h:363` | 受 `CONFIG_PM` 保护 |
| sysfs `control` | `drivers/base/power/sysfs.c:101,108` | `auto`/`on` ↔ allow/forbid |
| sysfs `runtime_suspended_time` | `drivers/base/power/sysfs.c:137` | 累计挂起毫秒 |
| sysfs `runtime_status` | `drivers/base/power/sysfs.c:150` | `unsupported` 分支在 `:157` |
| sysfs `autosuspend_delay_ms` | `drivers/base/power/sysfs.c:182,192` | 可写延迟 |
| `runtime_attrs[]` | `drivers/base/power/sysfs.c:652` | 属性清单 |
| `dpm_sysfs_add()` | `drivers/base/power/sysfs.c:694` | 安装 `power/` 属性组 |
| runtime PM 回调语义 | `Documentation/power/runtime_pm.rst:95` | "suspended ≠ 真的省电" |
| 回调优先级 | `Documentation/power/runtime_pm.rst:40` | §2 |
| 设备字段清单 | `Documentation/power/runtime_pm.rst:205` | §3 |
| 回调上下文 | `Documentation/power/runtime_pm.rst:77` | 默认进程上下文、中断使能 |
| runtime PM 与系统睡眠 | `Documentation/power/runtime_pm.rst:640` | §6；状态同步写法 `:682`；恢复 full power 的理由 `:659` |
| autosuspend | `Documentation/power/runtime_pm.rst:850` | §9；"不是自动挂起"`:862`；`mark_last_busy` 时机 `:869` |
| 系统睡眠状态 | `Documentation/admin-guide/pm/sleep-states.rst:13` | 全局低功耗状态 |

### KUnit

| 符号 | 位置 | 说明 |
|---|---|---|
| `__kunit_test_suites` | `include/kunit/test.h:351` | 段名 |
| `kunit_test_suites()` / `kunit_test_suite()` | `include/kunit/test.h:370,374` | 注册 suite 到 `.kunit_test_suites` 段 |
| executor 参数 | `lib/kunit/executor.c:18,34,37,40` | action / filter_glob / filter / filter_action |
| `kunit_parse_glob_filter()` | `lib/kunit/executor.c:69` | `suite.test` 拆分 + glob |
| `kunit_exec_list_tests()` | `lib/kunit/executor.c:276` | `kunit.action=list` |
| KTAP 格式 | `Documentation/dev-tools/ktap.rst:120` | `ok 1 <name>` |
| KUnit 总览 | `Documentation/dev-tools/kunit/index.rst:42` | white-box；`kunit_tool` 在 `:46` |
| 手动运行 | `Documentation/dev-tools/kunit/run_manual.rst:48,56` | 模块加载时跑；debugfs results |
| `kunit_tool` 用法 | `Documentation/dev-tools/kunit/run_wrapper.rst:35,53` | `kunit.py run` / `--kunitconfig` |
| filter glob 格式 | `Documentation/dev-tools/kunit/running_tips.rst:28` | `"<suite_glob>[.test_glob]"` |
| `kunit.py` 参数 | `tools/testing/kunit/kunit.py:373` | 位置参数 `filter_glob` |
| MAINTAINERS | `MAINTAINERS:11485` | KUnit 维护者与列表 |
| 内核参数 `kunit.enable` | `Documentation/admin-guide/kernel-parameters.txt:2560` | 总开关 |

### 本项目源码

| 位置 | 内容 |
|---|---|
| `driver/sensor_char.c:1252` | `sensor_runtime_suspend()`（停 hrtimer→关中断） |
| `driver/sensor_char.c:1278` | `sensor_runtime_resume()`（开中断→启动 hrtimer） |
| `driver/sensor_char.c:1300` | `sensor_pm_ops`（`SET_RUNTIME_PM_OPS`） |
| `driver/sensor_char.c:1482` | probe 里的 7 步 PM 初始化 |
| `driver/sensor_char.c:1537` | remove 里 `pm_runtime_disable()` 在最前 |
| `driver/sensor_char.c:1589` | `driver.pm = &sensor_pm_ops` |
| `driver/sensor_char.c:513` | `open()`：`pm_runtime_resume_and_get()` |
| `driver/sensor_char.c:528` | `release()` 入口；`mark_last_busy`+`put_autosuspend` 在 `:549` |
| `driver/sensor_char.c:794` | `sensor_apply_interval()`：挂起时只记账（`:808`） |
| `driver/sensor_char.c:1148` | debugfs `regs`：取 PM 引用再读寄存器 |
| `driver/sensor_calc.h` | 三个纯逻辑函数（12 位补码/周期/环形下标） |
| `driver/sensor_kunit.c` | KUnit 套件 `sensor_calc`，5 个用例 |

---

## 附录 B：易错点速查

| # | 易错点 | 正确做法 |
|---|---|---|
| 1 | 读 `/sys/class/<类设备>/power/runtime_status` | 读**硬件设备**：`/sys/bus/i2c/devices/0-0048/power/` |
| 2 | 用 `control == auto` 判"PM 已启用" | 用 `runtime_status != unsupported` |
| 3 | `get_sync()` 失败不补 `put` | 用 `pm_runtime_resume_and_get()` |
| 4 | `put_autosuspend()` 前忘 `mark_last_busy()` | 两句成对写 |
| 5 | probe 里只 get 不 put | `get_noresume` + … + `put_autosuspend` 配平 |
| 6 | suspend 只改标志 | 停 hrtimer + 关中断（先定时器后中断） |
| 7 | remove 里最后才 `pm_runtime_disable()` | **最先** disable，再拆资源 |
| 8 | `set_active()` 放在 `enable()` 之后 | 必须在之前 |
| 9 | 挂起期间写参数顺手启动定时器 | 只记账，resume 时生效 |
| 10 | KUnit 匹配写成 `ok 1 - <名字>` | KTAP 是 `ok 1 <名字>`（空格） |
| 11 | 凭"dmesg 有 ok"判 KUnit 通过 | 逐条用例名 + 汇总 `pass:N fail:0` + `not ok`=0 |
| 12 | 用旧 dmesg 快照判"事件是否发生" | 每次都**当场重新取样** |
| 13 | 只验证"能挂起" | 同时验证"能恢复"（resume 后采样推进） |
| 14 | `interval` 判定在两个入口各写一份 | 抽成 `sensor_interval_valid()`（KUnit 可测） |

---

## 附录 C：本文标注"未验证"的结论清单

以下内容本文给出方法与预期，但**本项目没有实测**，读者不应引用为"已验证"：

1. 给模块传 `kunit.filter_glob` 参数过滤用例（§7.2 只演示了不传参数直接 `insmod`）。
2. §7.3 的四条反例实验的具体 FAIL 输出文本（方法与预期失败项已列出）。
3. 系统睡眠（`SET_SYSTEM_SLEEP_PM_OPS`）路径——本项目**未实现**，
   §1.1/Q11 引用的是内核文档，不是本项目实测。
4. `pm_runtime_irq_safe()` 下的行为（本项目未使用）。
5. `kunit.action=list` / `kunit.filter`（属性过滤）的实际输出。
6. §7.2 中手动 QEMU 序列（`init=/bin/sh`）的逐条输出——本文引用的是测试脚本
   `tests/phases/06-runtime-pm-kunit.sh` 在日志中留下的同源实测值，
   手动序列本身未逐条重跑。
