# 阶段 06 知识点：sysfs / debugfs / configfs 接口机制、故障注入与 KUnit

> 面向目标：嵌入式 / Linux 驱动岗面试。读完应当能讲清「四套内核—用户接口怎么选」「sysfs 属性从宏展开到
> `show()/store()` 被调用的完整链路」「debugfs 的 ERR_PTR 与 NULL 安全性」「故障注入怎么证明错误处理真被执行」，
> 并能被追问到"为什么这么写、不这么写会怎样"。
>
> 本文所有内核源码引用都来自本项目的目标内核树 `~/kernel-build/linux-6.6.156`（Linux 6.6.156，aarch64），
> 格式为 `文件:行号`。所有驱动行为与实测值都来自**已归档的 QEMU 串口日志**：
>
> - 阶段 06 主日志：`logs/20260914-135029-main-06-runtime-pm-kunit.log`（`pass=31 fail=0`，第 356 行）
> - 阶段 04 主日志：`logs/20260914-135048-main-04-sysfs-debugfs.log`（`pass=42 fail=0`，第 359 行）
> - 回归：`logs/20260914-135054/135106/135111/135122-*.log`（03=40 / 02=19 / 01=16 / smoke=15）
>
> 本 lane 为**只读分析**：没有重新构建、没有重新跑 QEMU。第 7 节的"预期输出"逐条标注了它取自哪份日志的第几行；
> 任何本环境未实测过的结论都显式标注「未验证」。

---

## 0. 一句话心智模型

**这四套接口的区别不是"文件长什么样"，而是"内核愿意为用户态付出多少兼容成本"。**

| 接口 | 一句话定位 | ABI 承诺 |
|---|---|---|
| procfs | 进程/内核的**历史遗留**文本视图（`/proc/<pid>`、`/proc/sys`） | 混装：既有永久稳定的 sysctl，也有大量 obsolete/removed 条目 |
| sysfs | **内核对象的属性视图**（kobject → 文件），严格 one-value-per-file | **稳定 ABI**：发布即承诺不破坏 |
| debugfs | 内核开发者的**调试通道**（无格式规则、默认 root-only） | **明确声明不是稳定 ABI** |
| configfs | **用户态用 `mkdir/rmdir` 创建/销毁内核对象**（对象生命周期由用户态驱动） | 与 sysfs 同级的 ABI 纪律，但对象方向相反 |

写驱动时选哪个接口，本质是回答一个问题：
**"这份信息将来能不能改格式、能不能删？"** 不能改 → sysfs（要走 ABI 文档流程）；能改 → debugfs。
本项目就是按这条线切的：**4 个稳定单值属性放 sysfs，3 个调试/统计文件放 debugfs**
（`driver/sensor_char.c:1045-1055`、`driver/sensor_char.c:1429-1431`）。

---

## 1. 四套接口的定位差异与选择依据

### 1.1 对照表（面试可以直接背这张表）

| 维度 | procfs | sysfs | debugfs | configfs |
|---|---|---|---|---|
| 挂载点 | `/proc` | `/sys` | `/sys/kernel/debug` | `/sys/kernel/config` |
| 服务对象 | 进程信息、内核统计、sysctl | **内核对象**（设备/驱动/总线/类） | 内核开发者 | 需要用户态创建对象的子系统（如 target、null_blk） |
| 内容模型 | 自由文本 | **一个文件一个值**（`Documentation/filesystems/sysfs.rst:62-70`） | **没有任何规则**（`Documentation/filesystems/debugfs.rst:11-13`） | 属性文件类似 sysfs；另支持二进制属性（`Documentation/filesystems/configfs.rst:51-70`） |
| 对象由谁创建 | 内核 | 内核（设备出现就出现） | 内核 | **用户态 `mkdir(2)`**（`Documentation/filesystems/configfs.rst:30-36`） |
| ABI | 历史包袱：新旧混装 | 稳定（`Documentation/ABI/README:12-18`） | **不承诺**（`Documentation/filesystems/debugfs.rst:14-18`） | 视子系统而定，但同样要按接口文档管理 |
| 默认可见性 | 进程可见部分信息 | 全用户可读 | **root only**（`Documentation/filesystems/debugfs.rst:25-27`） | root |
| 模块可用性 | — | 是 | 是，但 **GPL-only**（`Documentation/filesystems/debugfs.rst:28`） | 是 |
| 典型误用 | 把驱动参数塞进 `/proc` | 把统计 dump 塞进 sysfs | 把生产接口放进 debugfs | 把"内核发现的设备"放进 configfs |

对照本项目：`tests/runner/init.sh:21-26` 在 initramfs 里挂了 `proc / sysfs / devtmpfs / debugfs / configfs`，
这是能观测到下面所有现象的**前提**——debugfs 没挂载时，`/sys/kernel/debug/sensor_char/` 根本不存在。

### 1.2 sysfs 的 one-value-per-file 约定

内核文档的原话（`Documentation/filesystems/sysfs.rst:62-70`）：

> Attributes should be ASCII text files, preferably with only one value per file.
> It is noted that it may not be efficient to contain only one value per file, so it is
> socially acceptable to express an array of values of the same type.
> Mixing types, expressing multiple lines of data, and doing fancy formatting of data is
> heavily frowned upon.

拆成三条可执行的规则：

1. **一个文件尽量一个值**：`interval_ms` 里只有周期（`driver/sensor_char.c:977-987` 输出 `"%lu\n"`）；
   同类型数组可以接受（例如多个通道的温度列在一起），但**不能混类型**。
2. **不要多行、不要花哨格式**：一旦 `show()` 输出多行，用户态解析就要写状态机，而这条格式将来永远不能改。
3. **读写用同一个语义**：内核文档要求 `show()` 与 `store()` 操作"同一个缓冲区表示"
   （`Documentation/filesystems/sysfs.rst:223-227`），所以 `echo 100 > interval_ms` 之后
   `cat interval_ms` 必须读回 `100`——本项目实测：`logs/20260914-135048-main-04-sysfs-debugfs.log:290`
   `[CHECK:PASS] sysfs 读回新值=100`。

为什么内核要这么"死板"？因为 sysfs 文件被 **udev / systemd / 各种脚本**直接读，
每个文件的格式就是一份**给全世界上亿台机器用的 API**。一个文件的格式越简单，兼容成本越低。

### 1.3 为什么统计数字不该放 sysfs（ABI 稳定性语义）

这是一个"结论正确、但面试官会追问推理链"的问题。完整推理链有 4 环，每一环都有内核证据：

**第 1 环：sysfs 是稳定 ABI，发布即冻结。**
`Documentation/ABI/README:12-18` 定义四级稳定性，其中 `stable/` 的原文是：

> This directory documents the interfaces that the developer has defined to be stable.
> Userspace programs are free to use these interfaces with no restrictions, and backward
> compatibility for them will be guaranteed for at least 2 years. Most interfaces (like
> syscalls) are expected to never change and always be available.

所以"加一个字段"就不是本地改动，而是**一次 ABI 变更**：要么新增文件（可以接受），
要么改已有文件格式（不可以）。统计信息恰恰是最容易"想加一个字段"的东西
（今天统计 `irq`，明天想加 `irq_deferred`）。

**第 2 环：统计天然违反 one-value-per-file。**
本项目的 `debugfs/stats` 一行有 **10 个字段**（`driver/sensor_char.c:1070-1093`），
实测 `open=1 read=0 irq=3 i2c_err=0 interval=20 dropped=0 ring_count=3 ring_capacity=85 seq=3 shm_writes=3`
（`logs/20260914-135048-main-04-sysfs-debugfs.log:306`）。
这种"一行 10 个字段"的东西放进 sysfs，等于发布一个**永远不能改格式的 dump 接口**
（内核文档对此的评价是 "heavily frowned upon"）。

**第 3 环：统计的语义会随驱动内部实现漂移。**
`irq_count` 的定义可能从"定时器触发次数"变成"有效采样次数"；`ring_capacity`（本项目 = 85）
是 `kfifo` 向上取整到 2 的幂之后的实际值（`driver/sensor_char.c:1036-1043`），
换一种缓冲实现它就会变。**内部实现的细节不该被外部接口冻住**。

**第 4 环：内核的整体政策是"不提供稳定内部 API，但必须不破坏用户空间"。**
`Documentation/admin-guide/sysfs-rules.rst:4-7` 原文：

> The kernel-exported sysfs exports internal kernel implementation details and depends on
> internal kernel structures and layout. It is agreed upon by the kernel developers that
> the Linux kernel does not provide a stable internal API.

于是结论是：**sysfs 里的每个文件都是把内部实现细节"卖出去了"**。
卖稳定的、必要的（设备参数、设备状态）；统计这种"随时会变"的，卖给 debugfs——
那里明确声明"改了不算破坏"。

> 面试加分点：内核**确实**有把统计放进 sysfs 的先例，例如
> `/sys/class/net/<iface>/statistics/*`，而且它有一份专门的 ABI 文档
> （`Documentation/ABI/testing/sysfs-class-net-statistics`，最早条目 Date: April 2005）。
> 那是**有意接受 ABI 成本**（网卡统计是用户态监控的刚需，全世界的工具都在用）。
> 所以规则不是"统计绝对不能进 sysfs"，而是"只在你愿意把它当永久 ABI 时才进"。

### 1.4 debugfs「不是稳定 ABI」的准确含义

原文（`Documentation/filesystems/debugfs.rst:14-18`）：

> The debugfs filesystem is also intended to not serve as a stable ABI to user space; in theory,
> there are no stability constraints placed on files exported there. The real world is not always
> so simple, though [1]_; even debugfs interfaces are best designed with the idea that they will
> need to be maintained forever.

这句话有三层含义，答面试时必须把三层都说到：

1. **内核不做承诺**：增加、删除、改格式、改文件名都不需要走 ABI 流程，不需要公告期。
   所以**任何产品功能都不能依赖 debugfs**（发行版/生产内核可能根本不挂载，或者通过
   挂载选项把权限收紧到 root，见 `Documentation/filesystems/debugfs.rst:25-27`）。
2. **"不承诺"不等于"可以乱写"**：文档自己就补了一句"现实中 even debugfs 接口也最好按永久维护来设计"。
   同感于本项目：`debugfs/regs` 读寄存器失败时**必须打印真实错误码**而不是假值
   （`driver/sensor_char.c:1156-1159` 输出 `"%02x: <read error %d>\n"`），
   否则"看现场"的人会被假数据误导——调试接口的可信度就是它的全部价值。
3. **6.6 里还有内核级的开关**：`CONFIG_DEBUG_FS_ALLOW_ALL / _NONE / _RESTRICT` 决定 debugfs
   能不能挂载、能不能给模块用（`lib/Kconfig.debug:647-667`，实现见
   `fs/debugfs/inode.c:38` 的 `debugfs_allow` 与 `fs/debugfs/inode.c:889-903`）。
   也就是说"我的模块里用了 debugfs 就一定能用"是错的：API 层面就可能被拒（返回 `-EPERM`，
   `fs/debugfs/inode.c:346-347`）。

**一句话对照**：sysfs 是"签了合同的接口"，debugfs 是"贴在实验室墙上的临时说明"。

### 1.5 procfs 与 configfs 的边界

- **procfs**：`/proc/<pid>`（进程视角）+ `/proc/sys`（sysctl 开关）+ 一堆历史统计。
  新增驱动接口放这里是**社区明确不鼓励**的做法：`/proc` 里的接口删不掉，
  所以内核用 `Documentation/ABI/obsolete/`（标注待删除）与 `Documentation/ABI/removed/`
  （已删除清单）两个目录来管理这种历史包袱（见 `Documentation/ABI/README:34-41`）。
  设备参数有专门的 sysfs，没有理由再加 `/proc` 文件。
- **configfs**：与 sysfs 的关键差别是**对象由谁创建**。sysfs 里对象是内核发现设备后自动出现的
  （`Documentation/filesystems/configfs.rst:21-27`）；configfs 里对象由用户态 `mkdir(2)` 创建、
  `rmdir(2)` 销毁（`Documentation/filesystems/configfs.rst:30-36`）。
  适合"用户态要批量造虚拟对象"的子系统（网络 target、null_blk 等）。
  configfs 的属性继承 sysfs 的纪律：**一个属性文件里不要塞多个值**
  （`Documentation/filesystems/configfs.rst:57`、`:61-63`），且同样有 PAGE_SIZE 上限。
  本项目只用到了"挂载 configfs"（`tests/runner/init.sh:26`），没有新增 configfs 子系统。

### 1.6 本项目的实际选择

```c
/* driver/sensor_char.c:1045-1055 —— 稳定 ABI：4 个单值属性 */
static struct attribute *sensor_attrs[] = {
	&dev_attr_interval_ms.attr,   /* RW：采样周期 */
	&dev_attr_seq.attr,           /* RO：最新样本序号 */
	&dev_attr_i2c_errors.attr,    /* RO：I2C 累计失败次数（故障注入的观察口） */
	&dev_attr_ring_capacity.attr, /* RO：kfifo 真实容量（85，不是请求的 64） */
	NULL,
};
static const struct attribute_group sensor_attr_group = { .attrs = sensor_attrs };
```

```c
/* driver/sensor_char.c:1429-1431 —— 调试通道：统计 / 现场寄存器 / 缓冲明细 */
debugfs_create_file("stats", 0444, sd->dbg, sd, &sensor_dbg_stats_fops);
debugfs_create_file("ring",  0444, sd->dbg, sd, &sensor_dbg_ring_fops);
debugfs_create_file("regs",  0444, sd->dbg, sd, &sensor_dbg_regs_fops);
```

挂载点选择：属性挂在**字符设备类设备**的 kobject 上
（`sd->char_dev` → `/sys/class/sensor_char/sensor0/`，`driver/sensor_char.c:1413`），
因为用户面对的是"这个传感器字符设备"，class 路径与 `/dev/sensor0` 一一对应；
而 Runtime PM 的状态挂在**硬件设备**（i2c client `0-0048`）上——这个差异在第 5.3 节展开，是阶段 06 的实测坑。

---

## 2. sysfs 属性机制（源码层）

### 2.1 `DEVICE_ATTR_RW` / `DEVICE_ATTR_RO` 逐层展开

驱动里写的是这一行（`driver/sensor_char.c:1010`）：

```c
static DEVICE_ATTR_RW(interval_ms);
```

展开过程是**三层宏**，每一层都可以在面试里点出文件行号：

**第 1 层** `include/linux/device.h:179-180`：

```c
#define DEVICE_ATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
```

**第 2 层** `include/linux/sysfs.h:138`：

```c
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)
```

**第 3 层** `include/linux/sysfs.h:101-107`：

```c
#define __ATTR(_name, _mode, _show, _store) {				\
	.attr = {.name = __stringify(_name),				\
		 .mode = VERIFY_OCTAL_PERMISSIONS(_mode) },		\
	.show	= _show,						\
	.store	= _store,						\
}
```

所以最终等价于（手工展开）：

```c
static struct device_attribute dev_attr_interval_ms = {
	.attr  = { .name = "interval_ms", .mode = 0644 },
	.show  = interval_ms_show,      /* 名字由宏拼出来：_name##_show */
	.store = interval_ms_store,
};
```

`DEVICE_ATTR_RO` 走的是另一条路（`include/linux/device.h:197` → `include/linux/sysfs.h:115-119`）：

```c
#define __ATTR_RO(_name) {						\
	.attr	= { .name = __stringify(_name), .mode = 0444 },		\
	.show	= _name##_show,						\
}
```

差异有两处值得记：

1. **RO 不给 `.store` 赋值**（隐式 NULL），因此内核不会为它创建可写通道；
   注册时内核按"有没有 show / 有没有 store"挑选 kernfs 操作集
   （`fs/sysfs/file.c:274-288`：rw / ro / wo 三种 `kernfs_ops`）。
2. **RO 的模式 0444 是写死的**，不经 `VERIFY_OCTAL_PERMISSIONS()` 校验；
   RW 是 0644 且要过校验（该宏会拒绝"可写但没有写者"这类明显错误）。

`struct device_attribute` 本身（`include/linux/device.h:106-112`）：

```c
struct device_attribute {
	struct attribute	attr;                 /* name + mode */
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};
```

注意 `attr` 是**第一个成员**：`to_dev_attr()` 可以零成本 `container_of` 回来
（`drivers/base/core.c:2373`），这正是"通用 `struct attribute` + 具体子类"的经典写法。

### 2.2 kobject / attribute / attribute_group / sysfs_ops 模型

四个结构体的分工：

| 结构 | 定义位置 | 作用 |
|---|---|---|
| `struct attribute` | `include/linux/sysfs.h:30-38` | 最小属性：`name` + `mode` |
| `struct device_attribute` | `include/linux/device.h:106-112` | 设备属性 = `attribute` + `show/store` |
| `struct attribute_group` | `include/linux/sysfs.h:84-92` | 一批属性的集合（可选 `name` → 生成子目录，`include/linux/sysfs.h:63-65`） |
| `struct sysfs_ops` | `include/linux/sysfs.h:254-257` | **kobject 级**的 show/store 分发函数 |
| `struct kobj_type` | `include/linux/kobject.h:69` 字段 + `drivers/base/core.c:2541-2544` | kobject 的类型：`release` + `sysfs_ops` |

关键的"桥"是 `sysfs_ops`：sysfs 只认识 kobject 和 attribute，**不认识 `struct device`**。
设备类型把这两者接起来（`drivers/base/core.c:2399-2402`）：

```c
static const struct sysfs_ops dev_sysfs_ops = {
	.show	= dev_attr_show,
	.store	= dev_attr_store,
};
```

```c
/* drivers/base/core.c:2371-2385 */
static ssize_t dev_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct device_attribute *dev_attr = to_dev_attr(attr);   /* container_of */
	struct device *dev = kobj_to_dev(kobj);
	ssize_t ret = -EIO;

	if (dev_attr->show)
		ret = dev_attr->show(dev, dev_attr, buf);
	...
}
```

也就是说：**`dev_attr_show` 负责把 (kobj, attr) 翻译成 (dev, dev_attr)**，
再调用我们写的 `interval_ms_show()`。

注册路径（本项目 `driver/sensor_char.c:1413`）：

```text
sysfs_create_group(&sd->char_dev->kobj, &sensor_attr_group)
  └─ fs/sysfs/group.c:175 sysfs_create_group()
       └─ fs/sysfs/group.c:107 internal_create_group()
            └─ fs/sysfs/group.c:64  sysfs_add_file_mode_ns(parent, *attr, mode, ...)
                 └─ fs/sysfs/file.c:258 sysfs_add_file_mode_ns()
                      ├─ fs/sysfs/file.c:263  sysfs_ops = kobj->ktype->sysfs_ops   ← 没有 ktype 就 WARN
                      ├─ fs/sysfs/file.c:274-288 按 show/store 选 kernfs_ops
                      └─ fs/sysfs/file.c:299 kernfs_create_file_ns(...)
```

一个容易踩的点：`fs/sysfs/file.c:268-272` 的 `WARN(!sysfs_ops, ...)`
——"每个带属性的 kobject 必须有 ktype"。`device_create()` 出来的 kobject 自带 `device_ktype`
（`drivers/base/core.c:3154`），所以这条警告不会命中；但手写裸 kobject 时忘了 `kobj_type` 就会。

### 2.3 `show()` / `store()` 的完整调用链

**读** `cat /sys/class/sensor_char/sensor0/interval_ms`：

```text
vfs_read
 └─ kernfs_fop_read_iter()                       fs/kernfs/file.c:294
      │  （KERNFS_HAS_SEQ_SHOW → 走 seq_file 路径）
      └─ seq_read_iter()                         fs/seq_file.c:171
           └─ sysfs_kf_seq_show()                fs/sysfs/file.c:40
                ├─ buf = seq_get_buf(sf,&buf)    fs/sysfs/file.c:52
                ├─ count = ops->show(kobj, of->kn->priv, buf)   fs/sysfs/file.c:59
                │      └─ dev_attr_show()        drivers/base/core.c:2371
                │           └─ interval_ms_show() driver/sensor_char.c:977
                │                └─ sysfs_emit(buf, "%lu\n", ms)  fs/sysfs/file.c:735
                └─ seq_commit(sf, count)         fs/sysfs/file.c:73
```

**写** `echo 100 > /sys/class/sensor_char/sensor0/interval_ms`：

```text
vfs_write
 └─ kernfs_fop_write_iter()                      fs/kernfs/file.c:311
      ├─ len = min(len, PAGE_SIZE)               fs/kernfs/file.c:322
      ├─ copy_from_iter + buf[len] = '\0'        fs/kernfs/file.c:337（保证字符串终止）
      ├─ mutex_lock(&of->mutex)                  fs/kernfs/file.c:343（同一文件的写被串行化）
      └─ sysfs_kf_write()                        fs/sysfs/file.c:131
           └─ ops->store(kobj, priv, buf, count) fs/sysfs/file.c:140
                └─ dev_attr_store()              drivers/base/core.c:2387
                     └─ interval_ms_store()      driver/sensor_char.c:990
                          ├─ kstrtoul(buf, 0, &ms)          lib/kstrtox.c:181
                          ├─ sensor_interval_valid(ms)      driver/sensor_calc.h:74
                          └─ sensor_apply_interval(sd, ms)  driver/sensor_char.c:794
```

两个可直接引用的内核保证：

- **`store()` 拿到的字符串一定以 `\0` 结尾**（内核替我们补，`fs/kernfs/file.c:337`），
  所以 `sysfs_streq()` 之类的函数是安全的（文档同述：`Documentation/filesystems/sysfs.rst:225-227`）。
- **`store()` 必须返回消耗的字节数**；全部接受就返回入参 `count`
  （`Documentation/filesystems/sysfs.rst:249-250`）。本项目的实现就是
  `return count;`（`driver/sensor_char.c:1008`），失败则返回负 errno（`-EINVAL`）。
  返回 `count` 不是"礼貌"，而是让用户态 `echo` 不报错的前提。

### 2.4 上下文：能否睡眠？单缓冲为什么是 4096？

**（1）缓冲区：一个固定的 PAGE_SIZE 缓冲，`show()` 一次调用必须填完。**

`Documentation/filesystems/sysfs.rst:209-217` 原文：

> sysfs allocates a buffer of size (PAGE_SIZE) and passes it to the method.
> sysfs will call the method exactly once for each read or write.

`Documentation/filesystems/sysfs.rst:240-241` 补充：

> - The buffer will always be PAGE_SIZE bytes in length. On x86, this is 4096.

本实验环境：`CONFIG_ARM64_4K_PAGES=y`、`CONFIG_ARM64_PAGE_SHIFT=12`
⇒ **PAGE_SIZE = 4096 字节**（`~/kernel-build/linux-6.6.156/.config`）。
所以"sysfs 单次缓冲 4096 字节"在 x86 与 aarch64(4K页) 上都是 4096。

`show()` 若返回 ≥ PAGE_SIZE，内核会**打印 bad count 警告并强行截断到 PAGE_SIZE-1**：

```c
/* fs/sysfs/file.c:67-72（seq_file 路径） */
if (count >= (ssize_t)PAGE_SIZE) {
	printk("fill_read_buffer: %pS returned bad count\n", ops->show);
	count = PAGE_SIZE - 1;      /* Try to struggle along */
}
```

`drivers/base/core.c:2380-2382` 在 `dev_attr_show()` 里对设备属性**再检查一遍**。
**结论：`show()` 里绝不能堆输出**——想输出大内容就换 seq_file（见 3.2），
或者干脆换 debugfs + seq_file。

**写侧同样受限**：`kernfs_fop_write_iter()` 把长度截到 `PAGE_SIZE`
（`fs/kernfs/file.c:318-322`），并且内核明确**不支持部分写**
（`fs/kernfs/file.c:297-305` 的注释："We expect the entire buffer to come on the first write."）。
所以"往一个 sysfs 文件写 > 4KB 的内容"实际上只会处理第一页。
（本项目没有实测这种超长写的行为，标注**「未验证」**；上面是源码层面的结论。）

**（2）能不能睡眠？——能，而且本项目的 store 就在睡眠。**

原因看链路：`write()` 走 `kernfs_fop_write_iter()`，它持有的 `of->mutex` 是**mutex（可睡眠）**
（`fs/kernfs/file.c:311-360` 区间），整条路径运行在**发起 `write(2)` 的那条进程上下文**里，
没有中断上下文、没有自旋锁。所以：

```c
/* driver/sensor_char.c:990-1008：本项目 store 里实际发生了这些"可睡眠"动作 */
ret = kstrtoul(buf, 0, &ms);           /* 纯解析 */
sensor_apply_interval(sd, ms);         /* 内部：mutex_lock + hrtimer_cancel + dev_info */
```

`mutex_lock()` 会睡眠，`hrtimer_cancel()` 会等待回调结束——都合法。
更硬的证据：`debugfs/regs` 的 read 回调里直接发起 I2C 事务
（`driver/sensor_char.c:1148` 的 `pm_runtime_resume_and_get()`、`1154-1162` 的 `regmap_read()`），
I2C 传输在内核里就是会睡眠的操作，实测能正常工作（`logs/20260914-135048-...:318-320`）。

**但是"能睡眠"≠"可以随便睡"**，三条约束：

1. **不能睡太久**：`store()` 是同步调用，它的耗时 100% 变成用户态 `write()` 的延迟；
   `udev`/启动脚本/监控进程在读属性时会一起卡住。
2. **不能"等外部事件"**：如果 `store()` 里等一个"要用户态配合才能完成"的事件，就形成自锁；
   同理不能在 `store()` 里等一个只能在别处被唤醒的 completion。
3. **不能持有自旋锁**（会 `might_sleep` 报警），也**不能从原子上下文调用**这套属性。
   另外 `show()` 可能被周期性重复调用（每次 `read(2)` 都重新调用一次，文档
   `Documentation/filesystems/sysfs.rst:219-222`），所以必须**幂等、无副作用**。

> 一句话答法："sysfs 的 show/store 跑在调用者的进程上下文、持的是 mutex，所以可以睡眠；
> 但它们是同步接口，睡多久就等于用户态卡多久，所以只做校验+状态更新，耗时动作要么挪到
> workqueue，要么设计成'记账 + 稍后生效'。"

### 2.5 为什么 `store()` 里不该做耗时操作（本项目的正例与反例）

"不该做耗时操作"有 4 个具体理由，最好能各配一个证据：

| 理由 | 机制 | 本项目的处理 |
|---|---|---|
| 延迟直接转嫁给用户态 | `store` 是 `write(2)` 的同步路径 | `store` 只做解析+转交（`driver/sensor_char.c:990-1008`） |
| 阻塞其它写者 | `kernfs_fop_write_iter` 持 `of->mutex`（`fs/kernfs/file.c:311+`） | — |
| 与 Runtime PM 相互作用可能死锁 | 在 suspend/resume 路径上同步等待设备唤醒 | **挂起时只"记账"，绝不唤醒设备**（`driver/sensor_char.c:808-811`） |
| 属性是"参数通道"，不是"执行通道" | 用户态拿不到进度与错误细节 | 真正的执行交给 hrtimer / 中断线程 / workqueue |

第 3 条本项目有直接实测（这是阶段 06 的一个亮点）：

```text
# logs/20260914-135029-main-06-runtime-pm-kunit.log:303
[    9.835857] sensor_char 0-0048: interval -> 200 ms (deferred: device runtime-suspended)
# 同日志 :304-305
[CHECK:PASS] 挂起期间写 interval_ms 不会把设备唤醒
[CHECK:PASS] 驱动记录了被延迟的周期设置
```

代码逻辑（`driver/sensor_char.c:794-812`）：`sensor_apply_interval()` 发现
`READ_ONCE(sd->suspended)` 为真就**只更新 `interval_ms` 并打日志，直接 return**，
不 `hrtimer_start()`；等 `runtime_resume()` 时按最新值启动
（`driver/sensor_char.c:1278-1299`，实测 `logs/20260914-135029-...:307`
`runtime resume: sampling restarted (interval=200ms)`）。
这样做保证"`runtime_status` 说挂起"和"实际没采样"永远一致——否则出现**假省电**。

### 2.6 本项目属性的实现要点（可当模板背）

```c
/* RO 计数器：单次对齐读即可，不需要锁（driver/sensor_char.c:1013-1029） */
static ssize_t i2c_errors_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sensor_dev *sd = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", READ_ONCE(sd->i2c_errors));
}
static DEVICE_ATTR_RO(i2c_errors);
```

- **`dev_get_drvdata(dev)` 为什么能拿到 `sd`**：`device_create(sensor_class, NULL, sd->devt, sd, ...)`
  的第 4 个参数就是 drvdata（`driver/sensor_char.c:952-953`）。
- **为什么用 `sysfs_emit()` 而不是 `sprintf`**：`sysfs_emit()` 内部用 `vscnprintf(buf, PAGE_SIZE, ...)`
  并在缓冲区不对齐时 `WARN`（`fs/sysfs/file.c:735-747`），它**永远不会写超过一页**，
  也不会返回 ≥ PAGE_SIZE 的值。用 `sprintf` 则是字面意义上的缓冲区溢出风险。
- **为什么 `interval_ms_show` 要加锁而计数器不用**：`interval_ms` 是 `unsigned long`，
  但它的写入受 `sd->lock` 保护、且与"重启定时器"这一动作有语义耦合；计数器是 `u32`，
  `READ_ONCE` 给出的是**瞬时快照**，语义上不需要与别的字段一致。
- **非法输入必须被拒且"值不变"**：`kstrtoul("abc")` 返回 `-EINVAL`（`lib/kstrtox.c:181`），
  而不是像 `simple_strtoul` 那样默默返回 0。实测：`0` / `60001` / `abc` 全部被拒且
  `interval_ms` 保持 `60000`（`logs/20260914-135048-...:297-300`）。

---

## 3. debugfs 接口（源码层）

### 3.1 `debugfs_create_file()` 与 full proxy

```c
/* include/linux/debugfs.h:75-77 */
struct dentry *debugfs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops);
```

- `data` 会存进 inode 的 `i_private`（`Documentation/filesystems/debugfs.rst:53-55`），
  配合 `.open = simple_open` 就能在回调里用 `file->private_data` 拿回 `sd`
  （`driver/sensor_char.c:1170-1189`）。
- 注册实现在 `fs/debugfs/inode.c:483`（`debugfs_create_dir()` 在 `:589`）。

**容易被追问的细节：调用者拿到的 `fops` 不是内核真正用的 `fops`。**
`debugfs_create_file()` 会把传入的 fops 包一层 full proxy
（`fs/debugfs/inode.c:485-491` 传 `&debugfs_full_proxy_file_operations`，
代理实现在 `fs/debugfs/file.c:307-371`）。代理做的事：

1. 打开时把真实 fops 记在 inode 上（`fs/debugfs/file.c:318` + `debugfs_real_fops()`，`:50`）；
2. 每次 read/write 前检查文件是否**已被删除**——已删除就返回 `-EIO`
   （`fs/debugfs/file.c:78` 的注释）——避免"文件没了还在调旧 fops"的 UAF；
3. 在 `release` 里正确归还。

对"文件已删除/模块已卸载"这件事特别敏感的场景（例如高频路径）可以用
`debugfs_create_file_unsafe()` 跳过代理（`include/linux/debugfs.h:78`，
实现 `fs/debugfs/inode.c:522-530`）——代价是安全责任转移给调用者。
本项目**没有**用它（3 个文件都用安全的 `debugfs_create_file`）。

### 3.2 `seq_file`：如何规避 4KB 分页问题

先说清问题：**sysfs 那种"给你一个 PAGE_SIZE 缓冲，一次调用填完"的模式无法输出超过一页的内容**
（第 2.4 节）。debugfs 虽然不用一个只读一次，但如果你用 `simple_read_from_buffer()`
拷一个你自己准备的固定 `kbuf`，**输出一样会被 kbuf 大小截断**——这是隐式截断，
比报错更危险。

两条正确路线：

**路线 A（本项目的选择）——内容保证远小于一页，用固定缓冲 + `simple_read_from_buffer()`。**

```c
/* driver/sensor_char.c:1070-1093（节选）：stats 一次生成 320 字节以内 */
char kbuf[320];
int len;
mutex_lock(&sd->lock);
len = scnprintf(kbuf, sizeof(kbuf), "open=%u read=%u irq=%u ...\n", ...);
mutex_unlock(&sd->lock);
return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
```

- `simple_read_from_buffer()`（`fs/libfs.c:1114-1133`）负责推进 `*ppos`，
  因此 `cat`、`read`、分段读都能正常工作，且必须在 fops 里配 `.llseek = default_llseek`
  （`driver/sensor_char.c:1174`）。
- 三个文件的缓冲分别是 320 / 640 / 256 字节（`driver/sensor_char.c:1074,1101,1137`），
  都远小于一页。**代价**：缓冲大小就是硬上限，写代码时必须自己确认格式化输出不会溢出
  （`scnprintf` 系列保证不溢出，但会**静默截断**）。

**路线 B（内容可能超过一页）——`seq_file`。**

`seq_file` 解决分页问题的机制在 `seq_read_iter()`（`fs/seq_file.c:171-298`）：

1. 首次读时按 `PAGE_SIZE` 分配缓冲：`m->buf = seq_buf_alloc(m->size = PAGE_SIZE)`（`fs/seq_file.c:199-202`）；
2. 调用 `->show()` 往缓冲里写，写溢出（`seq_has_overflowed`）就把
   `m->size <<= 1` **翻倍扩容**并重新生成（`fs/seq_file.c:236-242`）；
3. 每次 `read(2)` 只把"当前能装下"的部分拷给用户态，剩余部分留在缓冲里，
   下次 `read` 继续（`fs/seq_file.c:206-214, 289-294`），`*ppos`/`m->read_pos` 负责对账。

于是**用户态可以分多次 `read()` 拿到任意长的输出**，内核侧的内存按需增长。

最省事的入口是 `single_open()`（`fs/seq_file.c:572-591`）：

```c
static int sensor_dbg_dump_show(struct seq_file *s, void *v)
{
	struct sensor_dev *sd = s->private;
	seq_printf(s, "seq=%u temp=%d\n", ...);
	return 0;
}
static int sensor_dbg_dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, sensor_dbg_dump_show, inode->i_private);
}
```

配套 `.release = single_release`（`fs/seq_file.c:611`）、`.llseek = seq_lseek`。
内核里还有 `DEFINE_SHOW_ATTRIBUTE()`（`include/linux/seq_file.h:196`）这种一步生成
"sysfs 设备属性 + seq_file"的宏，适合调试用的只读属性。
**本项目的 3 个 debugfs 文件都没有超过一页，所以没有用 seq_file**——
这是设计选择不是遗漏；`regs` 只打印 3 个寄存器（`driver/sensor_char.c:1154-1162`），
`ring` 只打印最近 8 条样本（`driver/sensor_char.c:1103-1112`）。

> 面试口径："debugfs 里超过一页的输出必须用 seq_file，否则要么被截断（固定缓冲），
> 要么返回 bad count（sysfs 语义）。seq_file 的缓冲是会自动翻倍的，并且 `read()` 可以分多次取。"

### 3.3 `ERR_PTR(-ENODEV)` 的语义：内核可能根本没有 debugfs

`debugfs_create_*()` 返回的是 `struct dentry *`，失败时是 **ERR_PTR**。文档
（`Documentation/filesystems/debugfs.rst:41-43`、`:59`）只提两种：

> An ERR_PTR(-ERROR) return value indicates that something went wrong.
> If ERR_PTR(-ENODEV) is returned, that is an indication that the kernel has been
> built without debugfs support and none of the functions described below will work.

**代码里实际有三种错误码**，这是面试里能显出"看过源码"的地方：

| 返回 | 条件 | 位置 |
|---|---|---|
| `ERR_PTR(-ENODEV)` | `CONFIG_DEBUG_FS=n`（编译期没有 debugfs，走 inline 桩） | `include/linux/debugfs.h:190-242` |
| `ERR_PTR(-ENOENT)` | 编了 debugfs 但**没初始化/没挂载** | `fs/debugfs/inode.c:349-350`（`start_creating()`） |
| `ERR_PTR(-EPERM)` | 内核策略禁止模块用 debugfs API（`debugfs_allow`） | `fs/debugfs/inode.c:346-347` |

所以**正确姿势是 `IS_ERR()` + `PTR_ERR()` 打印，不要写 `== ERR_PTR(-ENODEV)`**。

另外两个"可以省事"的保证：

- 传进去的 `parent` 如果是 error 指针，创建函数会**原样返回**（`fs/debugfs/inode.c:354-355`），
  所以"目录创建失败后面三个文件创建都失败"是自动的，不需要额外分支；
- 内核明确说**调用者可以忽略这些错误**（`fs/debugfs/inode.c:478-481`）：
  "it's expected that most callers should _ignore_ the errors returned by this function...
  Drivers should generally work fine even if debugfs fails to init anyway."

本项目的处理（`driver/sensor_char.c:1420-1432`）：

```c
	/* debugfs 创建失败不当作致命错误：它只在 CONFIG_DEBUG_FS 且实际挂载了
	 * debugfs 时才可用（生产内核常常不挂），属于可选调试通道。
	 * 失败时置 NULL：后续 debugfs_remove_recursive(NULL) 是安全的。 */
	sd->dbg = debugfs_create_dir(DRV_NAME, NULL);
	if (IS_ERR(sd->dbg)) {
		dev_warn(dev, "debugfs unavailable: %ld\n", PTR_ERR(sd->dbg));
		sd->dbg = NULL;                    /* 关键：归一到 NULL */
	} else {
		debugfs_create_file("stats", 0444, sd->dbg, sd, &sensor_dbg_stats_fops);
		...
	}
```

**为什么归一成 NULL 而不是保留 error 指针**：清理路径只有一条
（`debugfs_remove_recursive(sd->dbg)`），传 NULL 是安全的，不需要写
"是 ERR 就跳过、是 NULL 也跳过"的分支。这是"把错误状态归一化"的典型手法。

### 3.4 `debugfs_remove_recursive(NULL)` 的安全性

**结论：安全，源码级原因有三条。**

1. **`NULL` 与 error 指针都被吞掉**：

```c
/* fs/debugfs/inode.c:771-779 */
void debugfs_remove(struct dentry *dentry)
{
	if (IS_ERR_OR_NULL(dentry))
		return;
	simple_pin_fs(&debug_fs_type, &debugfs_mount, &debugfs_mount_count);
	simple_recursive_removal(dentry, remove_one);     /* ← 递归删除 */
	simple_release_fs(&debugfs_mount, &debugfs_mount_count);
}
```

   文档同述："The dentry value can be NULL or an error value, in which case nothing will be removed."
   （`Documentation/filesystems/debugfs.rst:238-239`）

2. **6.6 里 `remove` 与 `remove_recursive` 已经是同一个函数**：
   `#define debugfs_remove_recursive debugfs_remove`（`include/linux/debugfs.h:98`），
   也就是"传目录 dentry 就是递归删除"（旧的两次 API 语义合并了）。
   `CONFIG_DEBUG_FS=n` 时两者都是空的 inline 桩（`include/linux/debugfs.h:232-240`）。
3. **必须显式清理**：debugfs **不会**自动清理（`Documentation/filesystems/debugfs.rst:229-234`），
   模块卸载不删就会留下指向已卸载代码的 dentry——所以本项目在两条路径上都删：
   - probe 失败的回滚路径：`err_remove_sysfs:` 标签（`driver/sensor_char.c:1500-1502`）
   - 正常 remove：`driver/sensor_char.c:1543-1548`

**顺序也有讲究**：先摘用户可见接口，再销毁 kobject / 字符设备
（`driver/sensor_char.c:1540-1548` 的注释）。原因是 sysfs 属性挂在
`sd->char_dev->kobj` 上，若先 `device_destroy()`，回调里的 `sd` 就可能已经失效；
而 debugfs 传 NULL 无害，所以两个摘除动作放在一起、在销毁对象之前。

### 3.5 本项目的三个 debugfs 文件（设计要点）

| 文件 | 内容 | 设计要点 |
|---|---|---|
| `stats` | 一行 10 个统计字段 | 持 `sd->lock` 生成**同一时刻的一致快照**，锁外拷贝（`driver/sensor_char.c:1070-1093`） |
| `ring` | 缓冲状态 + 最近 8 条样本 | 样本从 **mmap 共享区**取，不去动 kfifo 索引（`driver/sensor_char.c:1095-1130`） |
| `regs` | regmap 现场读 0x00~0x02 | 走与采样**完全相同**的 regmap→i2c 通路；失败打印错误码；先取 PM 引用（`:1132-1168`） |

`ring` 的设计理由值得单独讲：如果用 `read()` 去 kfifo 里取样本，
**"看"的动作就改变了被观测对象**（消费了用户态的数据）。共享区只是一块只读快照
（`spin_lock(&sd->shm_lock)` 下只做 `memcpy` 到本地数组，`scnprintf` 与
`copy_to_user` 都在锁外），不干扰任何人。实测输出
（`logs/20260914-135048-...:326-329`）：

```text
count=6 dropped=0 ring_count=6 ring_capacity=85 shm_writes=6 recent=6
sample[0] seq=1 temp_milli=24062
sample[1] seq=2 temp_milli=24187
sample[2] seq=3 temp_milli=24250
```

---

## 4. 故障注入（fault injection）在驱动测试中的作用

### 4.1 为什么必须能制造故障

一句话：**没被执行过的错误处理代码 = 没写。**

驱动里的错误分支（regmap 返回 `-EIO` 之后做什么、计数怎么加、状态机怎么恢复）
在正常硬件上**永远不会走到**。而它恰恰是最容易写错的地方（少一次 unlock、
忘了复位状态、错误变成"设备死掉"）。故障注入解决三件事：

1. **证明错误分支真的被检查**（否则可能被 `if` 写错/被优化掉/被上层吞掉）；
2. **证明恢复路径可达**（错误之后业务还能继续，而不是一次错误永久卡死）；
3. **让计数/统计可复现**（"i2c_errors 增加了 3"这种断言只有在能精确注入 3 次时才成立）。

### 4.2 本项目怎么注入：在 `smbus_xfer` 里注入

注入点选在**虚拟 I2C 控制器的传输函数**里（`driver/virt_i2c.c:163-175`）：

```c
	mutex_lock(&chip->lock);
	chip->transfers++;

	/* 故障注入：模拟"总线干扰/芯片无应答"。
	 * 放在锁内递减，保证并发注入时次数精确。 */
	if (chip->injected_left) {
		chip->injected_left--;
		chip->errors++;
		mutex_unlock(&chip->lock);
		dev_info(chip->dev, "injected error: addr=0x%02x reg=0x%02x (left=%u)\n",
			 addr, reg, chip->injected_left);
		return -EIO;
	}
```

控制接口是一对 debugfs 文件（`driver/virt_i2c.c:372-374`）：

```text
/sys/kernel/debug/virt_i2c/inject_error   写 N → 接下来 N 次传输返回 -EIO（0200，只写）
/sys/kernel/debug/virt_i2c/stats          读统计（transfers/errors/injected_left/...）
```

写接口实现见 `driver/virt_i2c.c:291-312`（`kstrtou32` 解析 + 持锁赋值 + `dev_info`）。

**为什么这个注入点选得好**：它位于"总线语义层"，因此一次注入能同时验证整条链路的容错——

```text
virt_i2c inject → -EIO → i2c core → regmap（regmap_read 返回 -EIO）
   → sensor_char 的错误处理：sd->i2c_errors++ + dev_warn_ratelimited
   → 采样线程继续跑（恢复）
```

实测链路（`logs/20260914-135048-main-04-sysfs-debugfs.log:336-346`）：

```text
[    0.883358] virt_i2c virt-i2c: fault injection armed: next 3 transfer(s) will fail
[    0.889731] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=2)
[    0.889907] sensor_char 0-0048: regmap read failed: -5        ← -EIO 传到驱动
[    0.910307] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=1)
[    0.910466] sensor_char 0-0048: regmap read failed: -5
[    0.929593] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=0)
[    0.929770] sensor_char 0-0048: regmap read failed: -5
...
[CHECK:PASS] 注入 3 次后 i2c_errors 增量 >= 3（实测 3）
[CHECK:PASS] 注入结束后采样仍在推进（序号增长，实测 107 → 158）
```

驱动侧的计数与告警在 `driver/sensor_char.c:366-371`（`dev_warn_ratelimited(... "regmap read failed: %d\n", ret)`）。
注意用了 `ratelimited`：故障注入会让错误**显著变多**，不限速的 `dev_warn` 会自己把日志刷爆，
把真正要看的信息冲掉。

**注入验证的三件事**（面试可以直接背）：①错误被检查到了（计数增长）；
②错误没把设备弄死（恢复后采样继续推进）；③错误没把用户态通路弄坏
（`sensor_test` 退出码 0，`logs/20260914-135048-...:352-353`）。

### 4.3 与内核自带 fault-injection 框架的关系

**内核框架长什么样**（`Documentation/fault-injection/fault-injection.rst:1-60`）：
`lib/fault-inject.c` 提供统一的注入判据
`should_fail()` / `should_fail_ex()`（`lib/fault-inject.c:161` / `:103`），
参数结构 `struct fault_attr`（`include/linux/fault-inject.h:17-33`：
`probability` / `interval` / `times` / `space` / `task_filter` / `require-start/end` / `stacktrace-depth` …），
默认值在 `FAULT_ATTR_INITIALIZER`（`include/linux/fault-inject.h:39-47`）。
`CONFIG_FAULT_INJECTION_DEBUG_FS=y` 时，`fault_create_debugfs_attr()`
（`lib/fault-inject.c:211-244`）会在 `/sys/kernel/debug/fail*/` 下挂出
`probability`、`times`、`interval`、`verbose`、`task-filter`、`stacktrace-depth` 等旋钮。
已实现的注入器包括 `failslab`（kmalloc 失败）、`fail_page_alloc`、
`fail_usercopy`、`fail_futex`、`fail_make_request`、`fail_function` 等
（`Documentation/fault-injection/fault-injection.rst:11-47`）。
其中 `fail_function` 靠**白名单宏**工作：只有被 `ALLOW_ERROR_INJECTION()` 标注的函数才能注入返回值
（`include/asm-generic/error-injection.h:27`，运行时检查 `kernel/fail_function.c:279`
的 `within_error_injection_list()`），控制文件在 `/sys/kernel/debug/fail_function/inject`。

**本项目与它的关系，要说清三点**（否则容易答成"重复造轮子"）：

1. **本实验内核根本没开这个框架**：实测 `~/kernel-build/linux-6.6.156/.config` 里
   `# CONFIG_FAULT_INJECTION is not set`（`CONFIG_KUNIT=y`、`CONFIG_DEBUG_FS=y`、
   `CONFIG_CONFIGFS_FS=y` 则都开着）。所以走内核框架不是"选择问题"，是"本环境没有"。
2. **即使打开，它注入不到"第 N 次 I2C 传输"这一层**。
   框架的注入点是**通用层**（slab 分配、页分配、usercopy、被标注白名单的函数返回值）。
   实测 `grep ALLOW_ERROR_INJECTION drivers/i2c/*.c` 在 6.6.156 树上**没有任何匹配**，
   也就是说 i2c 核心的传输函数不在 `fail_function` 的白名单里，
   拿不到"接下来 3 次总线读失败"这种精确的、设备语义级的注入。
3. **两者是互补的、层次不同**：
   - 内核框架：验证**内核通用代码**在资源紧张时的容错（"分配失败你处理了吗"），
     特点是概率化、可全局打开、面向整个内核；
   - 设备侧注入（本项目 `/sys/kernel/debug/virt_i2c/inject_error`）：验证**这台设备的驱动**
     在总线/芯片故障时的行为，特点是**确定性**（写 N 就是 N 次）、可精确断言计数、能复现。
   真机上还有第三类手段：拔线/断电/用 GPIO 把总线拉死、给不存在的从机地址发事务
   （本项目 `driver/virt_i2c.c:149-154` 对不存在的地址返回 `-ENXIO`，
   等价于"总线上没这颗芯片"）。

> 面试口径："内核有通用的 fault-injection 框架（failslab/fail_function 等），
> 但它的注入点在通用层，且需要函数被 `ALLOW_ERROR_INJECTION()` 标注。
> 要精确控制'第 N 次总线传输失败'，得在驱动的设备语义层自己开注入点——
> 我的虚拟 I2C 控制器就是干这个的，写一个数字进 debugfs，接下来 N 次传输返回 -EIO。"

### 4.4 注入类测试的两个顺序陷阱（都踩过）

**陷阱 1：读接口会消耗注入次数。**
注入的是"接下来 N 次传输"，而 `cat /sys/kernel/debug/sensor_char/regs`
本身就要发起 3 次传输（`driver/sensor_char.c:1154-1162`）——如果在注入与断言之间读它，
注入次数会被吃掉，`i2c_errors` 的增量就不再是 3，**结论直接错**。
测试脚本因此明确写了顺序约束（`tests/phases/04-sysfs-debugfs.sh:121-122`）：
"先取基准 → 注入 → 等待 → 检查，中间不碰任何会发起 I2C 事务的接口"。

**陷阱 2（阶段 06 引入 Runtime PM 后新增）：后台采样不再"永远在跑"。**
阶段 06 之前设备一直采样，所以阶段 04 的注入段可以"不打开设备"、
靠周期性 I2C 读去消耗注入次数。启用 Runtime PM 后，无人持有时设备 1 秒内自动挂起并**停止采样**，
该段实际测到的是"挂起后不采样"，与它想验证的错误处理路径无关。
修法是**把隐含前提写出来**：注入段显式持有设备引用
（`tests/phases/04-sysfs-debugfs.sh:116-120` 的 `exec 3<>/dev/sensor0`，段末归还）。
注意性质：这**不是放宽检查**（42 项检查一个都没减，实测 `logs/20260914-135048-...:359`
仍是 `pass=42 fail=0`），而是补上了"后台采样在跑"这个前提。
同一个坑还波及"注入结束后采样仍在推进"这条检查（实测 107 → 158 就是修好之后的数字）。

---

## 5. 与 Runtime PM、KUnit 的交叉考点（阶段 06 的另外两条主线）

### 5.1 KUnit：为什么它必须跟"接口层"一起讲

KUnit 跑在**内核里**，测的是**真正会被驱动执行的代码**。本项目把纯逻辑抽到
`driver/sensor_calc.h` 里的 `static inline` 函数（`sensor_raw_to_milli()` `:51`、
`sensor_interval_valid()` `:74`、`sensor_fifo_next()` `:92`），
**驱动与 `sensor_kunit.ko` include 同一份实现**——不存在"测试一份、跑另一份"的漂移。
测试套件在 `driver/sensor_kunit.c:96-115` 用 `kunit_test_suite()` 注册
（宏定义 `include/kunit/test.h:370-374`：模块形式在 **insmod 时**运行，
内建形式在启动时由 executor 运行）。

输出格式是 **KTAP v1**：用例行 `ok <序号> <用例名>`（`lib/kunit/test.c:221-225`
的 `kunit_print_ok_not_ok()`），汇总行 `# <suite>: pass:x fail:y skip:z total:t`
（`lib/kunit/test.c:105`）。实测（`logs/20260914-135029-main-06-runtime-pm-kunit.log:332-342`）：

```text
[   16.000858]     # Subtest: sensor_calc
[   16.002475]     ok 1 sensor_raw_to_milli_boundaries
[   16.002782]     ok 2 sensor_raw_to_milli_chip_range
[   16.003166]     ok 3 sensor_raw_to_milli_ignores_high_bits
[   16.003546]     ok 4 sensor_interval_valid_bounds
[   16.003956]     ok 5 sensor_fifo_next_wraps
[   16.004094] # sensor_calc: pass:5 fail:0 skip:0 total:5
[   16.004373] ok 1 sensor_calc
```

**判定必须两条一起看**（否则会被"dmesg 里恰好有 ok 字样"蒙混）：
①逐条用例名出现（证明**这一版**用例真的跑了，`tests/phases/06-runtime-pm-kunit.sh:146-151`）；
②套件汇总 `pass:5 fail:0` 且 `not ok` 计数为 0（`:152-153`）。
另外注意：**6.6 的用例行没有 `-` 分隔符**（不是 TAP 13 的 `ok 1 - name`），
第一版测试按 `ok 1 - <名字>` 匹配，5 条全假失败——这类"格式假设"是写测试最常见的坑。

**KUnit 能测什么、不能测什么**（诚实边界，比"我写了单元测试"更能说服面试官）：
能测补码边界、换算系数、参数端点、下标回绕；**不能**测 regmap/I2C 传输、
中断上下文约束、Runtime PM 时序、并发正确性——那些只能在 QEMU 集成测试（或 KCSAN）里验。

### 5.2 Runtime PM 的状态属性为什么挂在硬件设备上

实测（`logs/20260914-135029-main-06-runtime-pm-kunit.log:268-270`）：

```text
[    0.640675] sensor_char 0-0048: user interfaces ready: sysfs=/sys/class/sensor_char/sensor0, debugfs=/sys/kernel/debug/sensor_char
[    0.642255] sensor_char 0-0048: runtime PM enabled (control=auto autosuspend=1000ms)
```

Runtime PM 的属性路径是 `/sys/bus/i2c/devices/0-0048/power/`（**硬件设备**），
而不是 `/sys/class/sensor_char/sensor0/power/`。原因：

- runtime PM 描述"**硬件是否上电/是否在工作**"，它属于 `struct device` 的电源管理核心状态；
  字符设备 class 节点是 `device_create()` 造出来的**纯软件接口**，
  没有 `pm_runtime_enable()`，其 `power/runtime_status` 恒为 `unsupported`
  ——源码依据：`drivers/base/power/sysfs.c:155-159`，`disable_depth` 非 0 就输出 `"unsupported"`。
- **弱检查的坑**：`power/control` 的默认值对所有设备就是 `auto`
  （`drivers/base/power/sysfs.c:24-26`、`:262`），所以"control == auto"几乎恒真，
  不能作为"驱动启用了 PM"的判据。阶段 06 的判定升级为
  **`runtime_status` 不是 `unsupported`**（`tests/phases/06-runtime-pm-kunit.sh:50-54`，
  实测 `logs/20260914-135029-...:279`）。

实测的状态时间线（`logs/20260914-135029-...:281-311`）：

| 时刻 | 事件 | 日志行 |
|---|---|---|
| 1.646s | 无人使用 1 秒后自动挂起，采样停止 | `:281` `runtime suspend: sampling stopped (irq_count=2 seq=2)` |
| 3.697s | `open()` 取引用 → 恢复采样 | `:285` `runtime resume: sampling restarted (interval=500ms)` |
| 5.756s | `close()` 后 1 秒挂起 | `:293` `runtime suspend: ... (irq_count=6 seq=6)` |
| 挂起 2 秒内 | `seq` 与 `irq` **完全冻结** | `:299` `挂起 2 秒：seq 6 -> 6，irq 6 -> 6` |
| 9.835s | 挂起期间写 `interval_ms` 只记账 | `:303` `interval -> 200 ms (deferred: device runtime-suspended)` |
| 10.892s | 重新 open 按新周期恢复 | `:307` `runtime resume: sampling restarted (interval=200ms)` |
| 14.098s | 反复开关 3 轮后仍能挂起（引用计数配平） | `:322-324` |
| 累计 | `runtime_suspended_time = 9076ms` | `:326` |

**讲清"真省电 vs 假省电"**：只有当 `runtime_status=suspended` 时
`hrtimer` 真的停了、中断真的关了（`driver/sensor_char.c:1268-1271`
的 `hrtimer_cancel()` + `disable_irq()`），`seq/irq` 才会冻结。
只把状态标成 suspended 而采样照跑，是最典型的**假省电**，
测试项"挂起期间样本序号冻结"就是专门抓它的。

### 5.3 debugfs 也要遵守电源管理（"看现场不能破坏现场"）

`debugfs/regs` 会发起 I2C 事务，而挂起时真机上的芯片总线接口可能是关的
（读到 0 或读到无意义值）。所以该回调先 `pm_runtime_resume_and_get()`
（`driver/sensor_char.c:1148`）、读完 `pm_runtime_mark_last_busy()` +
`pm_runtime_put_autosuspend()` 归还（`:1164-1165`）。
`pm_runtime_resume_and_get()` 的语义值得记：失败时它自己把引用退回去
（`include/linux/pm_runtime.h:432-443` 里失败分支调 `pm_runtime_put_noidle()`），
而老的 `pm_runtime_get_sync()` 失败时会留下一个引用 → 设备再也挂不下去
（经典泄漏，`include/linux/pm_runtime.h:415-422` 的注释就是在劝你用前者）。

---

## 6. 面试问答（12 问）

### Q1. sysfs 和 debugfs 有什么区别？

四维回答（缺一维都会被打断）：

| 维度 | sysfs | debugfs |
|---|---|---|
| ABI 承诺 | 稳定（`Documentation/ABI/README:12-18`） | 明确不承诺（`Documentation/filesystems/debugfs.rst:14-18`） |
| 内容规则 | 一个文件一个值，不许花哨（`sysfs.rst:62-70`） | 没有规则，"想放什么放什么"（`debugfs.rst:11-13`） |
| 可见性 | 普通用户可读 | 默认 root-only，可能整个被策略禁用（`debugfs.rst:25-27`、`fs/debugfs/inode.c:346`） |
| 生命周期 | 与 kobject 绑定，由内核对象驱动 | 手工创建/删除，**没有自动清理**（`debugfs.rst:229-234`） |

一句话总结：**sysfs 是产品接口，debugfs 是调试接口**。判断标准是
"用户态的脚本/工具将来能不能依赖它"。

### Q2. 为什么不要用 sysfs 传统计数字？

因为一旦发布就是**永久 ABI**（`Documentation/ABI/README:12-18`：stable 保证至少 2 年兼容，
大多数接口永不改变），而统计有两个天生属性：**字段会增长**、**语义会漂移**
（`irq_count` 的定义、`ring_capacity` 依赖 kfifo 的取整行为等）。
再加上 one-value-per-file 的约定（`sysfs.rst:62-70`），一行 10 个字段的 dump
放进 sysfs 就等于冻结一个"永远不能改格式"的文本协议。
**反例也要会说**：`/sys/class/net/*/statistics/*` 确实在 sysfs 里，而且有专门的 ABI 文档
（`Documentation/ABI/testing/sysfs-class-net-statistics`）——那是**有意接受 ABI 成本**的例外。
所以规则是"统计默认放 debugfs，除非你愿意把它当永久 ABI 并写文档"。

### Q3. sysfs 的 `store()` 里能不能睡眠？

**能。** 走在 `kernfs_fop_write_iter()`（`fs/kernfs/file.c:311-360`）里，
持的是 `of->mutex`（mutex，可睡眠），上下文是发起 `write(2)` 的进程，
没有中断上下文、没有自旋锁。本项目 `interval_ms_store()` 里就有
`kstrtoul` → `sensor_interval_valid` → `sensor_apply_interval`
（内部 `mutex_lock` + `hrtimer_cancel`），实测可用
（`logs/20260914-135048-...:290-300`）；`debugfs/regs` 的读回调里还会发起 I2C 事务
（一定睡眠）。
**但不能久睡、不能等外部事件**：这是同步接口，睡多久用户态就卡多久，
而且持有 `of->mutex` 会阻塞其它写者；在系统 suspend/resume 路径上被调用的属性还可能死锁。
所以设计上要"只记账、稍后生效"（本项目挂起期间改周期就是只记账，
`driver/sensor_char.c:808-811`，实测 `:303`）。

### Q4. debugfs 为什么可以不管 ABI？

因为内核**明确声明**了这一点（`Documentation/filesystems/debugfs.rst:14-18`）：
"intended to not serve as a stable ABI... in theory, there are no stability constraints
placed on files exported there." 加字段、改格式、删文件都不算破坏用户空间。
**但要补三句**：① 生产环境可能根本不挂 debugfs（或只给 root），所以产品功能不能依赖它；
② 文档自己也说实践中仍应"按永久维护来设计"（同段后半句）；
③ 6.6 起 API 层还受 `CONFIG_DEBUG_FS_ALLOW_*` 策略限制
（`lib/Kconfig.debug:647-667`、`fs/debugfs/inode.c:38,346`），可能直接拿不到创建权限。

### Q5. 怎么做驱动的"在线调参"？

标准答案是 **sysfs 单值可写属性 + 集中校验 + 集中生效**，具体五步：

1. 定义 RW 属性：`DEVICE_ATTR_RW(interval_ms)`（`driver/sensor_char.c:1010`）；
2. 解析用 `kstrtoul()`（`lib/kstrtox.c:181`）而不是 `simple_strtoul()`——后者解析失败返回 0，
   会把"用户写错"变成"用户要求 0"（`driver/sensor_char.c:995-1000` 的注释）；
3. 范围校验集中一份：`sensor_interval_valid()`（`driver/sensor_calc.h:74`）；
4. 生效动作集中一份：`sensor_apply_interval()`（`driver/sensor_char.c:794`），
   它负责与锁、定时器、PM 状态打交道；
5. 失败要返回负 errno 且**不改动旧值**，成功返回 `count`。
   实测：`echo 60000` 接受、`echo 60001`/`0`/`abc` 拒绝且值保持
   （`logs/20260914-135048-...:294-300`）。

补充点：参数多且要求"原子地一次性生效"时，单值 sysfs 就不合适了——
那属于 ioctl 的领域（结构化、可带版本号、一次调用完成），或者 configfs；
`module_param` 只能在加载时改，做不到在线。

### Q6. 同一个设置有两个入口（ioctl 和 sysfs），怎么保证不漂移？

**单一定义来源 + 测试断言一致**：

- 判定只有一份：`sensor_interval_valid()`（`driver/sensor_calc.h:74`），
  ioctl 调它（`driver/sensor_char.c:845`）、sysfs 也调它（`:1004`）；
- 生效只有一份：`sensor_apply_interval()`（`driver/sensor_char.c:794`），两个入口都调；
- 边界由 KUnit 覆盖：`sensor_interval_valid_bounds`（`driver/sensor_kunit.c:75-86`）；
- 集成测试断言两入口一致：写 sysfs 后读 ioctl 的 `GET_STATS` 必须是同一个值
  （`tests/phases/04-sysfs-debugfs.sh:55-58`，实测
  `logs/20260914-135048-...:290-291` 两条同时 PASS）。

**为什么这件事重要**：本项目的真实教训就是"ioctl 下限 10ms、sysfs 下限 1ms"——
同一个设置走 A 接口成功、走 B 接口失败，是排查成本最高的一类 bug（`docs/impl/04-sysfs-debugfs-实现记录.md` 第四节）。

### Q7. sysfs 的 4KB 限制到底是什么？

两句话：**读**——`show()` 拿到的是一个 `PAGE_SIZE` 缓冲（本环境 4096 字节），
内核**每次 read 只调一次** `show()`，必须一次性填完；返回 ≥ PAGE_SIZE 会被打印
`bad count` 并截断到 `PAGE_SIZE-1`（`fs/sysfs/file.c:67-72`、
`drivers/base/core.c:2380-2382`；文档 `sysfs.rst:209-217`、`:240-241`）。
**写**——`kernfs_fop_write_iter()` 把长度截到 `PAGE_SIZE`
（`fs/kernfs/file.c:318-322`），且内核不支持分次写（`fs/kernfs/file.c:297-305`），
所以单次 `write()` 最多处理一页。
超过一页的输出只能换 seq_file（或换 debugfs）。

### Q8. 什么时候必须用 seq_file？

输出**可能超过一页**、或者内容需要**按记录遍历**（一条一条生成、可能很多条）时。
例如打印几百个寄存器、几百条样本历史。`seq_file` 的缓冲按需翻倍
（`fs/seq_file.c:236-242` 的 `m->size <<= 1`），且 `read()` 可以分多次取完
（`fs/seq_file.c:206-214, 289-294`）。最省的写法是 `single_open()`
（`fs/seq_file.c:572`）+ `seq_printf` + `single_release`。
本项目的 debugfs 文件都在 640 字节以内，所以用固定缓冲 + `simple_read_from_buffer()`
（`fs/libfs.c:1114`）就够——**不是所有 debugfs 文件都需要 seq_file**。

### Q9. `debugfs_create_dir()` 返回 `ERR_PTR(-ENODEV)`，要不要让 probe 失败？

**不要让 probe 失败。** debugfs 是可选调试通道：内核明确说"大多数调用者应该忽略这些错误"
（`fs/debugfs/inode.c:478-481`），而且生产内核可能 `CONFIG_DEBUG_FS=n`（返回 `-ENODEV`，
`include/linux/debugfs.h:190-242`）、没初始化（`-ENOENT`，`fs/debugfs/inode.c:349-350`）
或被策略禁用（`-EPERM`，`:346-347`）。
正确做法：`dev_warn()` 打印 `PTR_ERR()`，把指针归一成 `NULL`，清理路径统一
（本项目 `driver/sensor_char.c:1424-1432`）；
`debugfs_remove(NULL/ERR)` 是安全的（`fs/debugfs/inode.c:771-774` 的
`IS_ERR_OR_NULL` 检查）。
**不要写 `== ERR_PTR(-ENODEV)`**——错误码有三种。

### Q10. 没有硬件怎么做驱动错误处理的测试？

故障注入。按"注入点在哪一层"分三类：

1. **设备/总线语义层**（本项目）：在虚拟 I2C 控制器的 `smbus_xfer` 里
   实现"接下来 N 次传输返回 -EIO"（`driver/virt_i2c.c:163-175`），
   debugfs 控制（`driver/virt_i2c.c:291-312, 372-374`）。确定性、可精确断言。
2. **内核通用层**：内核自带 fault-injection 框架（`should_fail()`，
   `lib/fault-inject.c:161`；`failslab`/`fail_page_alloc`/`fail_function` 等，
   `Documentation/fault-injection/fault-injection.rst:11-47`），
   用来验证"内存分配失败/copy_from_user 失败"这类容错。`fail_function` 需要函数被
   `ALLOW_ERROR_INJECTION()` 标注（`include/asm-generic/error-injection.h:27`）。
3. **真机手段**：拔线、断电、用 GPIO 拉死总线、访问不存在的从机地址让总线 NACK
   （本项目 `-ENXIO` 分支，`driver/virt_i2c.c:149-154`）。

无论哪一类，判定都要看三件事：**错误被检查到**（计数/日志）、**设备能恢复**
（后续采样继续推进，实测 107 → 158）、**用户态通路没被弄坏**（`sensor_test` 退出码 0）。

### Q11. KUnit 和用户态单元测试有什么区别？为什么把函数抽成 `static inline` 头文件？

KUnit 跑在**目标内核里**、用内核的类型和宏、测的是**真正编译进驱动的那份代码**，
不需要为了可测而导出符号或拆库；输出是 KTAP，可被 `kunit.py`/CI 解析
（`include/kunit/test.h:370-374`、`lib/kunit/test.c:198-230`）。
抽成 `static inline` 头文件（`driver/sensor_calc.h`）而不是单独 `.c`：
①编译后内联，驱动零额外开销；②不必为一个函数多编一个模块、不用维护
`EXPORT_SYMBOL` 与模块依赖；③**驱动与测试 include 同一份实现**，杜绝
"测试一份、跑另一份"的漂移。约束是：只能放不依赖内核子系统、无副作用的函数。

### Q12. 怎么判断 Runtime PM 是"真省电"而不是"假省电"？

看两件事是否一致：**状态**（`power/runtime_status` 变 `suspended`）和
**行为**（数据源真的停了）。本项目用"挂起 2 秒内 `seq`/`irq` 计数必须冻结"来证明后者
（`tests/phases/06-runtime-pm-kunit.sh:89-96`，实测
`logs/20260914-135029-...:299-301`：`seq 6 -> 6`、`irq 6 -> 6`）。
"假省电"的具体形态是：只 `WRITE_ONCE(suspended,true)` 而不
`hrtimer_cancel()`+`disable_irq()`，状态显示挂起、实际还在跑。
另外两个配平检查：**引用计数**（反复开关后仍能挂起，`:322-324`）
与**累计挂起时间**（`runtime_suspended_time = 9076ms`，`:326`）——
只 `get` 不 `put` 的泄漏不会报错，只会"永远不挂起"，是最难发现的一类 bug。

---

## 7. 亲手验证：QEMU 内可复现命令与预期输出

> **取证说明（务必先读）**：本 lane 是只读分析，**没有重新构建、没有重新跑 QEMU**。
> 下面每一条"预期输出"都标注了它出自哪份已归档日志的第几行；括号里是原始日志行号。
> 未在本环境实测过的结论已在文中显式标注「未验证」。
> 要自己重跑，用本 lane 的构建/测试命令（见 7.1）。

### 7.1 构建与运行（本 lane 的隔离参数）

```bash
# ① 虚拟机内编译模块 + 打包 initramfs（构建目录 ~/lab-06，与其它 lane 隔离）
limactl shell dev bash -c 'LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/13-vm-fast-cycle.sh'
# ② 产物拷回宿主
LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/21-macos-sync-artifacts.sh
# ③ 跑阶段 06（31 项检查）
LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 06-runtime-pm-kunit 300
# ④ 回归
LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 03-ringbuffer-mmap 40
LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 02-i2c-driver 19
LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh 01-io-models 16
LANE=06 bash /Users/lucien/workspace/self-study/projects/wt-06/scripts/22-macos-run-test.sh smoke 15
```

预期收尾标记（`logs/20260914-135029-main-06-runtime-pm-kunit.log:356`）：

```text
[TEST:END] 06-runtime-pm-kunit pass=31 fail=0
```

### 7.2 QEMU 内可粘贴的验证序列

以下命令都在 QEMU 串口的 shell 里执行（initramfs 已由 `tests/runner/init.sh:21-26`
挂好 proc/sys/devtmpfs/debugfs/configfs，并按 `driver/modules.load`
加载 `virt_i2c.ko` → `sensor_char.ko`）。

**（1）确认接口挂载点存在**

```sh
ls -d /sys/class/sensor_char/sensor0 /sys/kernel/debug/sensor_char /sys/kernel/debug/virt_i2c
```

预期（驱动 probe 时打印的路径，`logs/20260914-135029-...:268`）：

```text
[    0.640675] sensor_char 0-0048: user interfaces ready: sysfs=/sys/class/sensor_char/sensor0, debugfs=/sys/kernel/debug/sensor_char
```

**（2）读 sysfs 四个属性（one-value-per-file）**

```sh
SYS=/sys/class/sensor_char/sensor0
cat $SYS/interval_ms; cat $SYS/seq; cat $SYS/i2c_errors; cat $SYS/ring_capacity
```

预期（初值，`logs/20260914-135048-main-04-sysfs-debugfs.log:279`）：

```text
读到的值：interval_ms=500 seq=0 i2c_errors=0 ring_capacity=85
```

`ring_capacity=85` 而不是 64 是**刻意暴露**的真实容量：`kfifo` 把请求的 1536 字节
向上取整到 2 的幂（2048）后是 85 个样本（`driver/sensor_char.c:1036-1043`；
判定见 `:284` `[CHECK:PASS] ring_capacity 反映 kfifo 实际容量（85 >= 64）`）。

**（3）写 sysfs 并读回（在线调参 + 边界/非法值）**

```sh
echo 100 > $SYS/interval_ms;  cat $SYS/interval_ms      # 期望 100
echo 1 > $SYS/interval_ms;    cat $SYS/interval_ms      # 期望 1（下界接受）
echo 60000 > $SYS/interval_ms; cat $SYS/interval_ms     # 期望 60000（上界接受）
echo 0 > $SYS/interval_ms      # 期望报错，且值保持 60000
echo 60001 > $SYS/interval_ms  # 期望报错，且值保持 60000
echo abc > $SYS/interval_ms    # 期望报错，且值保持 60000
echo 20 > $SYS/interval_ms     # 收尾：调快，便于后面看采样推进
```

预期（`logs/20260914-135048-...:290-300`）：

```text
[CHECK:PASS] sysfs 读回新值=100
[CHECK:PASS] ioctl(GET_STATS) 读到同一值（两入口一致）
[CHECK:PASS] 下界 1ms 被接受
[CHECK:PASS] 上界 60000ms 被接受
[CHECK:PASS] 0ms 被拒绝（值保持不变）
[CHECK:PASS] 0ms 写入报错信息包含 Invalid 或 EINVAL 线索
[CHECK:PASS] 60001ms 被拒绝（值保持不变）
[CHECK:PASS] 非数字被拒绝（值保持不变）
```

内核侧会打印 `interval -> 100 ms` / `interval -> 20 ms`
（`driver/sensor_char.c:815`；实测 `logs/20260914-135048-...:301`）。

**（4）读 debugfs 三个文件**

```sh
DBG=/sys/kernel/debug/sensor_char
cat $DBG/stats
cat $DBG/regs
cat $DBG/ring
```

预期（`logs/20260914-135048-...:306, 318-320, 326-329`）：

```text
open=1 read=0 irq=3 i2c_err=0 interval=20 dropped=0 ring_count=3 ring_capacity=85 seq=3 shm_writes=3
```

```text
00: 0188        ← 温度 raw = 392 → 392/16 = 24.5 ℃，落在芯片模拟区间 24.0~26.0
01: 0fa5        ← 湿度 raw = 4005 → 40.05 %RH
02: 0001        ← probe 写入的"连续转换使能"位
```

```text
count=6 dropped=0 ring_count=6 ring_capacity=85 shm_writes=6 recent=6
sample[0] seq=1 temp_milli=24062
sample[1] seq=2 temp_milli=24187
sample[2] seq=3 temp_milli=24250
```

`regs` 的数值语义可校验（温度 raw 必须落在 12 位量级、配置寄存器必须是 `0001`），
所以它能抓住字节序/换算类错误（判定见 `tests/phases/04-sysfs-debugfs.sh:85-95`）。

**（5）故障注入并观察错误计数（本阶段最关键的一条）**

```sh
SYS=/sys/class/sensor_char/sensor0
exec 3<>/dev/sensor0                       # 关键：持有引用，保证后台采样在跑（阶段 06 起必须）
before=$(cat $SYS/i2c_errors)
echo 3 > /sys/kernel/debug/virt_i2c/inject_error
sleep 1
after=$(cat $SYS/i2c_errors)
echo "i2c_errors $before -> $after"
seq1=$(cat $SYS/seq); sleep 1; seq2=$(cat $SYS/seq)
echo "恢复期样本序号：$seq1 -> $seq2"
exec 3<&-; exec 3>&-
```

预期内核日志（`logs/20260914-135048-main-04-sysfs-debugfs.log:336-342`）：

```text
[    0.883358] virt_i2c virt-i2c: fault injection armed: next 3 transfer(s) will fail
[    0.889731] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=2)
[    0.889907] sensor_char 0-0048: regmap read failed: -5
[    0.910307] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=1)
[    0.910466] sensor_char 0-0048: regmap read failed: -5
[    0.929593] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=0)
[    0.929770] sensor_char 0-0048: regmap read failed: -5
```

预期检查结论（`:344-346`）：

```text
[CHECK:PASS] 注入 3 次后 i2c_errors 增量 >= 3（实测 3）
      · 恢复期样本序号：107 → 158
[CHECK:PASS] 注入结束后采样仍在推进（序号增长，实测 107 → 158）
```

**注意注入顺序**：不要在注入与读取 `i2c_errors` 之间 `cat $DBG/stats`
（前一步也发起 I2C 事务，会消耗注入次数）；`debugfs/regs` 会一次消耗 3 次，绝对不能碰。

**（6）Runtime PM 四步验证（阶段 06 主项）**

```sh
PWR=$(ls -d /sys/bus/i2c/devices/*-0048)/power
sleep 3;      cat $PWR/runtime_status            # 期望 suspended（无人持有）
exec 3<>/dev/sensor0; sleep 1; cat $PWR/runtime_status   # 期望 active
exec 3<&-; exec 3>&-; sleep 3; cat $PWR/runtime_status   # 期望 suspended
echo 200 > $SYS/interval_ms                      # 挂起期间写：只记账，不唤醒
sleep 1; cat $PWR/runtime_status                 # 期望仍是 suspended
exec 3<>/dev/sensor0; sleep 1
cat $SYS/interval_ms                             # 期望 200（挂起期间的设置已生效）
cat $PWR/runtime_suspended_time                  # 期望 > 0（核心记录了挂起时长）
exec 3<&-; exec 3>&-
```

预期（`logs/20260914-135029-main-06-runtime-pm-kunit.log:279, 289, 295, 303-305, 311, 326`）：

```text
[CHECK:PASS] runtime PM 已启用（runtime_status=active，不是 unsupported）
[CHECK:PASS] 打开期间保持 active
[CHECK:PASS] 关闭后自动挂起
[    9.835857] sensor_char 0-0048: interval -> 200 ms (deferred: device runtime-suspended)
[CHECK:PASS] 挂起期间写 interval_ms 不会把设备唤醒
[CHECK:PASS] 挂起期间设置的周期已生效（200ms）
      · runtime_suspended_time = 9076ms
```

并且 `cat $DBG/stats` 的 `irq=` 在挂起 2 秒内必须不变
（实测 `挂起 2 秒：seq 6 -> 6，irq 6 -> 6`，`:299`）——这就是"真省电"的判据。

**（7）KUnit 单元测试**

```sh
insmod /lib/modules/$(uname -r)/sensor_kunit.ko
sleep 1; dmesg | tail -20
lsmod | grep sensor_kunit
```

预期（`logs/20260914-135029-...:332-342`）：

```text
    # Subtest: sensor_calc
    1..5
    ok 1 sensor_raw_to_milli_boundaries
    ok 2 sensor_raw_to_milli_chip_range
    ok 3 sensor_raw_to_milli_ignores_high_bits
    ok 4 sensor_interval_valid_bounds
    ok 5 sensor_fifo_next_wraps
# sensor_calc: pass:5 fail:0 skip:0 total:5
ok 1 sensor_calc
```

判定标准：**用例名逐条出现**（证明这一版用例真的跑了）**且** `pass:5 fail:0`、
`not ok` 计数为 0（`tests/phases/06-runtime-pm-kunit.sh:148-157`）。

---

## 8. 未验证项与边界（诚实清单）

1. **内核自带 fault-injection 框架未在本环境验证**：本内核
   `# CONFIG_FAULT_INJECTION is not set`，`/sys/kernel/debug/fail*/` 不存在。
   本文关于该框架的描述来自内核源码与文档（`lib/fault-inject.c`、
   `Documentation/fault-injection/fault-injection.rst`），**未在本 QEMU 环境实测**。
2. **`debugfs` 完全未挂载时 probe 的行为未实测**：本文对 `IS_ERR(sd->dbg)`
   分支的描述来自源码（`fs/debugfs/inode.c:349-350`、`include/linux/debugfs.h:190-242`），
   本项目的实测运行中 debugfs 始终是挂载的（`tests/runner/init.sh:25`）。
   「未验证」。
3. **`seq_file` 路径未在本项目实测**：本项目 3 个 debugfs 文件都用固定缓冲 +
   `simple_read_from_buffer()`（`driver/sensor_char.c:1070-1168`），
   没有走 `seq_read_iter()`。第 3.2 节关于 seq_file 扩容机制的分析来自源码
   （`fs/seq_file.c:171-298`）。
4. **单次 `write(2)` 超过一页（>4096 字节）的实际截断行为未实测**：
   结论来自源码 `fs/kernfs/file.c:318-322`。正常使用不会有这种写法。
5. **sysfs 属性在系统级 suspend/resume 路径上的行为未验证**：本项目只实现了
   Runtime PM（`SET_RUNTIME_PM_OPS`，`driver/sensor_char.c:1300-1302`），
   没有 `SET_SYSTEM_SLEEP_PM_OPS`（`docs/impl/06-runtime-pm-kunit-实现记录.md` 第六节已记录）。
   "在 suspend 路径里做长睡眠可能死锁"是通用经验判断，**未在本环境构造复现**。
6. **`/proc` 新增接口"不被鼓励"这一条，本文引用的是 ABI 目录的管理流程
   （`Documentation/ABI/README:34-41`：obsolete/removed 两级）与社区惯例**，
   没有找到一句逐字的内核文档禁令；引用时按"管理流程"讲，不要声称"文档明文禁止"。

---

## 附录：本文引用的内核源码与文档清单（按文件）

| 文件 | 行号 | 内容 |
|---|---|---|
| `include/linux/sysfs.h` | 30-38 | `struct attribute` |
| | 62-92 | `struct attribute_group`（63-65：`name` → 子目录） |
| | 101-140 | `__ATTR` / `__ATTR_RO` / `__ATTR_RW` / `__ATTR_NULL` |
| | 254-257 | `struct sysfs_ops` |
| | 357 | `sysfs_emit()` 声明 |
| `include/linux/device.h` | 106-112 | `struct device_attribute` |
| | 156-157, 179-180, 197 | `DEVICE_ATTR` / `DEVICE_ATTR_RW` / `DEVICE_ATTR_RO` |
| `include/linux/kobject.h` | 69 | `struct kobject.ktype` |
| `fs/sysfs/file.c` | 26-33 | `sysfs_file_ops()` |
| | 40-77 | `sysfs_kf_seq_show()`（66-72：bad count 截断） |
| | 101-141 | `sysfs_kf_read()` / `sysfs_kf_write()` |
| | 258-299 | `sysfs_add_file_mode_ns()`（274-288：按 show/store 选 ops） |
| | 735-748 | `sysfs_emit()` 实现 |
| `fs/kernfs/file.c` | 239-290 | `kernfs_file_read_iter()` |
| | 294-299 | `kernfs_fop_read_iter()` |
| | 297-305 | 写路径注释：不支持部分写 |
| | 311-360 | `kernfs_fop_write_iter()`（318-322：截到 PAGE_SIZE；337：补 `\0`） |
| `fs/sysfs/group.c` | 64, 107, 175 | 属性组注册链路 |
| `drivers/base/core.c` | 2371-2385 | `dev_attr_show()`（2380-2382：bad count） |
| | 2387-2402 | `dev_attr_store()` / `dev_sysfs_ops` |
| | 2541-2544 | `device_ktype`（`.sysfs_ops = &dev_sysfs_ops`） |
| `lib/kstrtox.c` | 181-194 | `_kstrtoul()` |
| `include/linux/debugfs.h` | 75-98 | `debugfs_create_file()` / `create_dir()` / `remove*` |
| | 190-242 | `CONFIG_DEBUG_FS=n` 时的桩（返回 `ERR_PTR(-ENODEV)`） |
| `fs/debugfs/inode.c` | 38, 889-903 | `debugfs_allow` 与策略 |
| | 341-355 | `start_creating()`（346：`-EPERM`；349-350：`-ENOENT`；354-355：parent 是 error 直接返回） |
| | 478-491 | `debugfs_create_file()` 及"可忽略错误"的 NOTE |
| | 522-530 | `debugfs_create_file_unsafe()` |
| | 589-596 | `debugfs_create_dir()` |
| | 771-779 | `debugfs_remove()`（`IS_ERR_OR_NULL` + `simple_recursive_removal`） |
| `fs/debugfs/file.c` | 50-65 | `debugfs_real_fops()` |
| | 78 | 文件已删除时返回 `-EIO` |
| | 269-273, 307-371 | full proxy open/release 与代理 fops |
| `fs/seq_file.c` | 57-88 | `seq_open()` |
| | 171-298 | `seq_read_iter()`（199-202：初始 PAGE_SIZE；236-242：翻倍扩容；289-294：分段拷贝） |
| | 572-618 | `single_open()` / `single_open_size()` / `single_release()` |
| `include/linux/seq_file.h` | 196 | `DEFINE_SHOW_ATTRIBUTE()` |
| `fs/libfs.c` | 1114-1133 | `simple_read_from_buffer()` |
| `lib/fault-inject.c` | 18-40 | `setup_fault_attr()` |
| | 103-163 | `should_fail_ex()` / `should_fail()` |
| | 211-244 | `fault_create_debugfs_attr()`（`/sys/kernel/debug/fail*/` 旋钮） |
| `include/linux/fault-inject.h` | 17-47 | `struct fault_attr` / `FAULT_ATTR_INITIALIZER` |
| `include/asm-generic/error-injection.h` | 27-37 | `ALLOW_ERROR_INJECTION()` |
| `kernel/fail_function.c` | 279, 325 | 白名单检查与 `injectable` 符号链接 |
| `include/kunit/test.h` | 160-162, 370-374 | `KUNIT_CASE` / `kunit_test_suites` / `kunit_test_suite` |
| `lib/kunit/test.c` | 105, 198-230 | `# <suite>: pass:...` 汇总行与 `ok/not ok` 打印 |
| `include/linux/pm_runtime.h` | 415-443 | `pm_runtime_get_sync()` 与 `pm_runtime_resume_and_get()` |
| `drivers/base/power/runtime.c` | 1158, 1528, 1758 | `__pm_runtime_resume()` / `pm_runtime_enable()` / `set_autosuspend_delay()` |
| `drivers/base/power/sysfs.c` | 24-26, 98-99, 150-175, 262 | `control` 默认 `auto`；`runtime_status_show()`（`disable_depth` → `unsupported`） |
| `lib/Kconfig.debug` | 647-667 | `CONFIG_DEBUG_FS_ALLOW_ALL/_NONE/_RESTRICT` |
| `Documentation/filesystems/sysfs.rst` | 62-70, 209-222, 223-227, 240-241, 249-258 | one-value-per-file；PAGE_SIZE 缓冲；返回值语义；对象被 pin 住 |
| `Documentation/filesystems/debugfs.rst` | 11-18, 25-28, 41-43, 45-59, 229-247 | 无规则/非稳定 ABI/root-only/GPL-only/ERR_PTR/无自动清理 |
| `Documentation/filesystems/configfs.rst` | 17-37, 51-70 | 内核创建 vs 用户态 `mkdir`；属性纪律 |
| `Documentation/ABI/README` | 12-41 | 四级稳定性（stable = 至少 2 年兼容） |
| `Documentation/admin-guide/sysfs-rules.rst` | 4-7 | "内核不提供稳定内部 API" |
| `Documentation/fault-injection/fault-injection.rst` | 1-60, 86-131, 185-215 | 注入器清单与旋钮 |
| `Documentation/ABI/testing/sysfs-class-net-statistics` | 1-10 | sysfs 传统计的官方先例 |
