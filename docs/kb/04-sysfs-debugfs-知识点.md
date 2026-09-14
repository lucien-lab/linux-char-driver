# 阶段 04 知识点：sysfs 设备参数 + debugfs 运行统计 + 故障注入

> 面向目标：嵌入式 / Linux 驱动岗面试。读完应当能**讲清四种内核-用户态接口的定位差异、
> 从 `read()/write()` 一路追到驱动的 `show/store`、说清 sysfs 的 ABI 承诺为什么逼着你
> 把统计数字挪去 debugfs，并能被追问到"不这么写会怎样"**。
>
> **引用约定**
> - 内核源码引用来自本项目目标内核树 `~/kernel-build/linux-6.6.156`（Linux 6.6.156），
>   格式 `文件:行号`；行号在该内核树（以及 `kernel-src/linux-6.6.156`）上可 `grep -n` 直接复核。
> - 本项目源码引用格式 `路径:行号`，指本 worktree 的交付源码（`driver/sensor_char.c` 等）。
> - 所有"实测"结论都标了日志文件名与行号；`logs/` 是 gitignore 的，本 worktree 尚未同步，
>   引用的日志当前位于主 worktree `/Users/lucien/workspace/self-study/projects/linux-char-driver/logs/`。
> - 没跑过、没查到的东西标 **未验证**，不写"大概/应该"。
>
> **本文使用的日志**
> | 日志 | 内容 | 结论 |
> |---|---|---|
> | `logs/20260914-133749-main-04-sysfs-debugfs.log` | 阶段 04 测试（本轮最后一次全绿） | `[TEST:END] 04-sysfs-debugfs pass=42 fail=0` |
> | `logs/20260914-133755-main-03-ringbuffer-mmap.log` | 回归 03 | pass=40 fail=0 |
> | `logs/20260914-133834-main-02-i2c-driver.log` | 回归 02 | pass=19 fail=0 |
> | `logs/20260914-133807-main-01-io-models.log` | 回归 01 | pass=16 fail=0 |
> | `logs/20260914-133818-main-smoke.log` | 回归 smoke | pass=15 fail=0 |
> | `~/kernel-build/linux-6.6.156/.config` | 实验内核配置 | `CONFIG_DEBUG_FS=y`(:10453)、`# CONFIG_FAULT_INJECTION is not set`(:10695) |

---

## 0. 一句话心智模型

阶段 04 往驱动上加了两扇"给用户态看的窗口"，它们的技术本质都是
**把文件 I/O 转发成回调函数**：

> **sysfs = 给人（和脚本、工具）用的、受 ABI 保护的控制面板；
> debugfs = 给驱动作者自己用的、随时可改的示波器。**

两扇窗口的语法几乎一样（都是文件、都能 `cat`/`echo`），真正的区别是**承诺强度**：
sysfs 是内核与用户态之间"签了合同"的接口，debugfs 是内核明确声明"这份合同无效"的接口。
阶段 04 的所有设计取舍（哪些属性放哪边、为什么统计不进 sysfs、为什么 debugfs 创建失败不算错）
都是从这一条推出来的。

---

## 1. sysfs / procfs / debugfs / configfs 的定位差异与选择依据

### 1.1 四种接口的对照

| 维度 | procfs | sysfs | debugfs | configfs |
|---|---|---|---|---|
| 面向对象 | 进程信息（`Documentation/filesystems/proc.rst` 的标题就是 "/proc Filesystem"）；历史上也塞过驱动参数 | 内核对象（device/driver/class/bus/module） | 内核开发者自己 | 内核对象**的创建者** |
| 方向 | 内核 → 用户态（读多） | 内核 → 用户态 + 少量控制（读/写） | 内核 → 用户态 + 调试写 | **用户态 → 内核**（用户态请求创建/销毁对象） |
| 契约强度 | 一部分稳定（`/proc/sys` 是 sysctl ABI），大部分"能用但不承诺" | **稳定 ABI**（`Documentation/filesystems/sysfs.rst:419`） | **明确不是稳定 ABI**（`Documentation/filesystems/debugfs.rst:13-17`） | 语义同 sysfs（`Documentation/filesystems/configfs.rst:63` 说 one-value-per-file 与同样的 caveat） |
| 格式规则 | 自由文本 | one-value-per-file（`Documentation/filesystems/sysfs.rst:62`） | 无规则，"想放什么放什么" | one-value-per-file |
| 挂载点 | `/proc` | `/sys` | `/sys/kernel/debug` | `/sys/kernel/config` |
| 谁能写驱动接口 | `proc_create()` | `sysfs_create_group()` / `device_create_file()` | `debugfs_create_dir/file()` | `configfs_register_subsystem()` |
| 本阶段用途 | 不用 | 设备参数 + 设备状态单值 | 统计快照 / 寄存器现场 / 缓冲明细 | 不用（阶段 05 用它实例化 hrtimer trigger） |

一句话选择依据：

- **要给用户态长期依赖的控制/状态接口 → sysfs**（它承诺兼容）。
- **只给开发/调试期看的现场信息 → debugfs**（它承诺不兼容，所以可以随便改格式）。
- **要让用户态主动创建内核对象（例如 IIO 的软件触发器实例） → configfs**。configfs 是
  sysfs 的"逆操作"：sysfs 是内核对象在文件系统里的视图（对象在内核里创建），
  configfs 是内核对象的文件系统管理器（对象由用户态 `mkdir` 创建）。
  原文：`Documentation/filesystems/configfs.rst:16-17` "configfs is a ram-based filesystem
  that provides the converse of sysfs's functionality"，`:34` "the lifetime of the
  representation is completely driven by userspace"。
- **procfs 不要用于新接口**：`/proc/` 下塞驱动私有文件是历史做法，语义混乱且没有
  sysfs 的对象模型；新代码一律 sysfs/debugfs。（`/proc/sys` 的 sysctl 属于另一套 ABI，
  不在本阶段范围。）

### 1.2 sysfs 的 one-value-per-file 约定

`Documentation/filesystems/sysfs.rst:62`：

> Attributes should be ASCII text files, preferably with only one value per file.
> It is noted that it may not be efficient to contain only one value per file,
> so it is socially acceptable to express an array of values of the same type.

同一小节接着说，混类型、多行、花哨格式化是"heavily frowned upon"。
这条约定的工程意义：

1. **可组合**：脚本可以 `cat interval_ms` 直接拿到一个数，不需要解析格式；
   shell 里 `$((...))`、`awk '{print $1}'` 都能工作。
2. **可加字段不会破坏旧读者**（当你有多个单值文件时）：加一个新参数就加一个新文件，
   老文件语义不动。反过来，一个文件里塞 10 个字段，加第 11 个字段就是 ABI 变更。
3. **和 sysfs 的"每属性一文件"权限模型对齐**：权限是挂在文件上的，
   你能对 `interval_ms` 给 0644、对 `seq` 给 0444，各自独立；一行多字段的文件做不到。

本项目对应实现：4 个属性各自一个文件（`driver/sensor_char.c:1000-1007`）：

```c
static struct attribute *sensor_attrs[] = {
	&dev_attr_interval_ms.attr,   /* RW：设备参数 */
	&dev_attr_seq.attr,           /* RO：设备状态单值 */
	&dev_attr_i2c_errors.attr,    /* RO：设备状态单值 */
	&dev_attr_ring_capacity.attr, /* RO：设备状态单值 */
	NULL,
};
```

### 1.3 为什么统计数字不该放 sysfs（ABI 稳定性语义）

sysfs 的 ABI 承诺是有明文规定的。`Documentation/filesystems/sysfs.rst:419`：

> The sysfs directory structure and the attributes in each directory define an
> ABI between the kernel and user space. As for any ABI, it is important that
> this ABI is stable and properly documented. All new sysfs attributes must be
> documented in Documentation/ABI.

`Documentation/ABI/README` 进一步定义了四档稳定性（stable / testing / obsolete / removed），
`stable/` 这一档的承诺是"backward compatibility ... guaranteed for at least 2 years"。

把 `debugfs/stats` 那种一行 10 个字段的东西放进 sysfs，会发生什么：

```
open=1 read=0 irq=3 i2c_err=0 interval=20 dropped=0 ring_count=3 ring_capacity=85 seq=3 shm_writes=3
```

- 它**违反 one-value-per-file**；
- 它一旦发布就**冻结了整行格式**：将来想加一个 `suspended=` 字段，就是在用户态解析器
  背后改了 ABI；想删一个字段更不行；
- 它还**逼着所有使用者写脆弱的解析器**（按空格 split、按顺序取第 N 个字段），
  而内核无法承诺字段顺序永远不变。

所以统计快照放 debugfs。注意一个反直觉但重要的推论：
**"读起来方便"不是放 sysfs 的理由**——sysfs 是合同，debugfs 是便签。

反过来说，本项目放 sysfs 的三个计数器（`seq`/`i2c_errors`/`ring_capacity`）都是
**单值、语义稳定、有长期观测价值**的量：序号永远叫序号，错误数永远叫错误数，
容量不会换单位。它们放 sysfs 是合理的；而 `stats`/`ring`/`regs` 这种
"一次给一大坨、格式随时可能变"的东西放 debugfs。

### 1.4 debugfs 明确"不是稳定 ABI"的含义

`Documentation/filesystems/debugfs.rst:9-17`：

> Debugfs exists as a simple way for kernel developers to make information
> available to user space. Unlike /proc, which is only meant for information
> about a process, or sysfs, which has strict one-value-per-file rules,
> debugfs has no rules at all. Developers can put any information they want
> there. The debugfs filesystem is also intended to not serve as a stable
> ABI to user space; in theory, there are no stability constraints placed on
> files exported there.

这段话说清了三件事：

1. **无格式规则**：多字段、多行、二进制都行（本项目 `stats` 一行 10 个字段、
   `ring` 打印最近 8 条样本、`regs` 三行十六进制，都合法）。
2. **无兼容承诺**：内核可以在任何版本改文件名、改字段名、改格式、整体删掉，
   都不算破坏用户空间。所以**生产工具不应该依赖 debugfs**。
3. **但现实不是纯净的**：紧接着的一句是 "The real world is not always so simple, though"，
   `debugfs.rst` 的脚注指出，一旦某个 debugfs 接口被现实世界的工具依赖了，
   改它同样会挨骂。所以正确的表述是：**"可以不兼容"是内核给你的自由，
   不是让你随手乱改的许可证**。写接口时仍然要有设计。

另一条容易忽略的规则：debugfs API 是 **GPL-only** 导出的
（`Documentation/filesystems/debugfs.rst:26` "the debugfs API is exported GPL-only to modules"），
所以从模块里用 debugfs 必须 `MODULE_LICENSE("GPL")`。本项目模块就是 GPL。

### 1.5 本项目的目录布局与选择

```
/sys/class/sensor_char/sensor0/        ← sysfs（稳定 ABI，挂在字符设备 class 设备上）
├── interval_ms       RW 0644   采样周期（1..60000），写入立即生效
├── seq               RO 0444   当前最新样本序号
├── i2c_errors        RO 0444   累计 I2C 失败次数
└── ring_capacity     RO 0444   环形缓冲真实容量（样本数）

/sys/kernel/debug/sensor_char/         ← debugfs（调试通道，随时可改）
├── stats             RO 0444   一行 10 字段的运行统计一致快照
├── ring              RO 0444   缓冲状态 + 最近 8 条样本
└── regs              RO 0444   现场读寄存器 0x00~0x02（证明 regmap 通路活着）

/sys/kernel/debug/virt_i2c/            ← debugfs（虚拟控制器，阶段 02 起就有）
├── stats             RO 0444   总线统计
└── inject_error      WO 0200   故障注入：写 N → 接下来 N 次传输返回 -EIO
```

**为什么挂 sysfs 属性时用 `sd->char_dev->kobj` 而不是 i2c client 设备**
（`driver/sensor_char.c:1275`）：

```c
ret = sysfs_create_group(&sd->char_dev->kobj, &sensor_attr_group);
```

`sd->char_dev` 是 `device_create(...)` 创建的字符设备类设备（`driver/sensor_char.c:903-904`），
它对应 `/dev/sensor0` 与 `/sys/class/sensor_char/sensor0/`，用户态看到的就是
"这个传感器字符设备"，路径与设备节点一一对应。另一个可选点是 i2c client 设备
（`/sys/bus/i2c/devices/0-0048/`），但那里已经挂了 `driver` 符号链接、`name`、`modalias`
等 i2c 核心自己的属性，把驱动私有参数混进去会让"谁负责这个文件"变模糊。
两个位置都能工作；本项目的选择以"用户面对的对象"为准。

---

## 2. 属性机制（源码层）

### 2.1 DEVICE_ATTR_RW / DEVICE_ATTR_RO 展开成什么

这是面试最常被要求"你把它展开给我看"的一处。三层宏：

**第一层**（`include/linux/device.h:179-180, 197-198`）：

```c
#define DEVICE_ATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW(_name)

#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
```

**第二层**（`include/linux/sysfs.h:138, 115-118`）：

```c
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)

#define __ATTR_RO(_name) {						\
	.attr	= { .name = __stringify(_name), .mode = 0444 },		\
	.show	= _name##_show,						\
}
```

**第三层**（`include/linux/sysfs.h:101-106`）：

```c
#define __ATTR(_name, _mode, _show, _store) {				\
	.attr = {.name = __stringify(_name),				\
		 .mode = VERIFY_OCTAL_PERMISSIONS(_mode) },		\
	.show	= _show,						\
	.store	= _store,						\
}
```

所以 `DEVICE_ATTR_RW(interval_ms)` 展开后就是一个**全局变量定义**：

```c
struct device_attribute dev_attr_interval_ms = {
	.attr  = { .name = "interval_ms", .mode = 0644 },  /* VERIFY_OCTAL_PERMISSIONS 是编译期检查 */
	.show  = interval_ms_show,                          /* 名字是宏拼出来的，不是自动的 */
	.store = interval_ms_store,
};
```

`struct device_attribute` 本体（`include/linux/device.h:106-112`）：

```c
struct device_attribute {
	struct attribute	attr;	/* name / mode / owner */
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};
```

三个"被追问"的点：

- **`dev_attr_##_name` 这个变量名是宏拼的**，你在代码里写 `&dev_attr_interval_ms.attr`
  就是在引用那个全局变量；`interval_ms_show/store` 也必须是这个名字，否则编译不过。
- **`attr` 是结构体第一个成员**，所以 `struct attribute *` 和 `struct device_attribute *`
  可以靠 `container_of()` 互转——这正是内核通用层的分派手法
  （`drivers/base/core.c:2367` `#define to_dev_attr(_attr) container_of(_attr, struct device_attribute, attr)`）。
- **`RO` 版本只给 `.show`，`mode` 硬编码 0444；`RW` 给 `.show`+`.store`，`mode=0644`**。
  还有 `DEVICE_ATTR_WO`（0200，只有 store，本项目 `virt_i2c` 的 `inject_error` 用它；
  不过 `virt_i2c.c` 是手写 fops 而不是 DEVICE_ATTR，见 `driver/virt_i2c.c:322-327`）。
- 如果确实要自定义起始模式（例如 0600），用 `DEVICE_ATTR_RW_MODE(name, 0600)`
  （`include/linux/device.h:185-190`）。

### 2.2 kobject / attribute 模型与 attribute_group 注册

sysfs 的层次是 **kobject → attribute**：一个 kobject 对应一个目录，一组 attribute 对应该目录下的文件。
`struct kobject` 里有 `const struct kobj_type *ktype`（`include/linux/kobject.h:64-82`），
`ktype->sysfs_ops` 决定了这个 kobject 上的属性怎么被读写。
对 `struct device` 来说，ktype 是 `device_ktype`，它的 `sysfs_ops` 是
`dev_sysfs_ops`（`drivers/base/core.c:2399-2402`）：

```c
static const struct sysfs_ops dev_sysfs_ops = {
	.show	= dev_attr_show,
	.store	= dev_attr_store,
};
```

属性用 `struct attribute_group` 成组注册（`include/linux/sysfs.h:84-93`）：

```c
struct attribute_group {
	const char	*name;		/* NULL = 直接挂到 kobj 目录下 */
	umode_t		(*is_visible)(struct kobject *, struct attribute *, int);
	umode_t		(*is_bin_visible)(struct kobject *, struct bin_attribute *, int);
	struct attribute	**attrs;
	struct bin_attribute	**bin_attrs;
};
```

本项目的 group（`driver/sensor_char.c:1008-1010`）：

```c
static const struct attribute_group sensor_attr_group = {
	.attrs = sensor_attrs,	/* NULL 结尾的 attribute 指针数组 */
};
```

注册/注销（`driver/sensor_char.c:1275, 1371`）：

```c
sysfs_create_group(&sd->char_dev->kobj, &sensor_attr_group);
...
sysfs_remove_group(&sd->char_dev->kobj, &sensor_attr_group);
```

`sysfs_create_group()`（`fs/sysfs/group.c:175-179`）只是 `internal_create_group()` 的包装
（`fs/sysfs/group.c:107`），后者遍历 `grp->attrs` 逐个 `kernfs_create_file_ns()`，
组名非空时先建子目录。注意 `include/linux/sysfs.h:84-93` 里 group 是**可以嵌套子目录**的
（`.name` 非空），本项目 `.name = NULL`，所以 4 个属性直接落在
`/sys/class/sensor_char/sensor0/` 下。

生命周期规则（本项目严格照做）：

- `sysfs_create_group()` 必须在 kobject 已注册（`sd->char_dev` 已 `device_create()`）之后调用；
  `internal_create_group()` 里 `WARN_ON(!kobj || (!update && !kobj->sd))` 检查的就是这个
  （`fs/sysfs/group.c:113-114`）。
- `sysfs_remove_group()` 必须在 `device_destroy()` **之前**调用（`driver/sensor_char.c:1365-1372` 的注释）。
  否则属性回调里的 `dev_get_drvdata(dev)` 可能拿到已经释放的 `sd`，留下 use-after-free 窗口。
- 注册路径失败时要按反序清理：本项目 probe 失败路径里先 `sysfs_remove_group()` 再
  `device_destroy()`（`driver/sensor_char.c:1336-1342`）。

### 2.3 show / store 的调用链

把 `cat /sys/class/sensor_char/sensor0/interval_ms` 从系统调用追到驱动：

```
用户态 read(2)
 └─ vfs_read
     └─ kernfs_fop_read_iter()                      fs/kernfs/file.c:294
         └─ seq_read_iter()   （属性文件走 seq_file 路径）  fs/kernfs/file.c:297, fs/seq_file.c:171
             └─ sysfs_kf_seq_show()                 fs/sysfs/file.c:40
                 ├─ seq_get_buf()：保证缓冲区 >= PAGE_SIZE，并 memset 清零   fs/sysfs/file.c:47-54
                 └─ ops->show(kobj, kn->priv, buf)
                     └─ dev_attr_show()             drivers/base/core.c:2371
                         └─ dev_attr->show()        ← 即 interval_ms_show，driver/sensor_char.c:932
```

写路径：

```
用户态 write(2)
 └─ vfs_write
     └─ kernfs_fop_write_iter()                     fs/kernfs/file.c:311
         ├─ len = min_t(size_t, len, PAGE_SIZE)      fs/kernfs/file.c:322   ← 单次写被截到 4096
         ├─ kmalloc(len+1) + copy_from_iter() + buf[len]='\0'   fs/kernfs/file.c:329-337
         ├─ mutex_lock(&of->mutex)                   fs/kernfs/file.c:343   ← 同一打开文件的读写串行化
         └─ ops->write = sysfs_kf_write()            fs/sysfs/file.c:131
             └─ ops->store(kobj, kn->priv, buf, count)
                 └─ dev_attr_store()                 drivers/base/core.c:2387
                     └─ dev_attr->store()            ← 即 interval_ms_store，driver/sensor_char.c:945
```

这条链解释了几件常被问的事：

- **`buf` 一定是以 `'\0'` 结尾的内核缓冲**，`count` 是有效长度（不含结尾 0）。
  驱动里用 `kstrtoul(buf, 0, &ms)` 直接解析是合法的，不需要自己造字符串。
- **`echo 100 > interval_ms` 写进去的其实是 `"100\n"`**。`kstrtoul` 允许数字后面跟一个
  `'\n'` 再 `'\0'`（`lib/kstrtox.c:96-115` 的 `_kstrtoull`，`:108` 显式接受 `'\n'`；
  `kstrtoul` 本体是 `include/linux/kstrtox.h:30`）。所以 `echo` 不会因为换行而失败。
- **store 的返回值必须是 `count` 或负 errno**。返回 0 或短于 count 会被用户态当成"没写完"；
  本项目的 store 成功时 `return count`（`driver/sensor_char.c:963`），失败时
  `return ret`（`kstrtoul` 的错误码，如 `-EINVAL`）或 `-EINVAL`
  （`driver/sensor_char.c:956-960`）。
- **`sysfs_remove_group()` 和正在进行的读写之间有 kernfs 的 active reference 保护**，
  但驱动仍然应保证"remove 之后不再有回调进来"，所以清理顺序是
  "先摘接口（remove_group）→ 再销毁承载它的 kobject（device_destroy）"。

### 2.4 上下文：能否睡眠、4KB 缓冲限制、为什么 store 里不该耗时

**（1）缓冲上限是 4096 字节。**

- show 方向：`sysfs_kf_seq_show()` 拿到的一定是 `>= PAGE_SIZE` 的缓冲
  （`fs/sysfs/file.c:47-54`），并 `memset(buf, 0, PAGE_SIZE)`；随后
  `dev_attr_show()` 会检查 `ret >= PAGE_SIZE` 并打印
  `"dev_attr_show: %pS returned bad count"`（`drivers/base/core.c:2380-2383`）。
  也就是说 **show 最多只能产出 4095 字节有效内容**。这就是为什么规范写法是
  `sysfs_emit(buf, ...)` 而不是 `sprintf(buf, ...)`：
  `sysfs_emit()` 内部是 `vscnprintf(buf, PAGE_SIZE, fmt, args)`
  （`fs/sysfs/file.c:735-749`），天然不会写越界，而且当 `buf` 不是页起始地址时
  会 `WARN` 提醒你（属性回调拿到的 buf 一定是页起始）。
- store 方向：`kernfs_fop_write_iter()` 把单次 `write(2)` 的长度 clamp 到 `PAGE_SIZE`
  （`fs/kernfs/file.c:322`），所以 store 永远看不到超过 4096 字节的一次写。
  **需要写大块数据不是 sysfs 的用例**——那应该用 ioctl、write() 到设备节点、
  或者 bin_attribute（`bin_attrs`，`include/linux/sysfs.h:84-93`）。

**（2）show/store 在进程上下文执行，可以睡眠。**

调用链全部发生在 `read(2)/write(2)` 的系统调用上下文里（vfs → kernfs → sysfs → 驱动回调），
没有任何中断上下文/原子上下文约束。法定的两个同步点：

- 读路径：`seq_read_iter()` 持有 `m->lock`（`fs/seq_file.c:182`）；
- 写路径：`kernfs_fop_write_iter()` 持有 `of->mutex`（`fs/kernfs/file.c:343`）。

两者都是 mutex，**允许睡眠**。所以本项目可以直接在 `interval_ms_store()` 里
`sensor_apply_interval()`，而后者会 `mutex_lock(&sd->lock)` + `hrtimer_cancel()`
（`driver/sensor_char.c:756-769`）——这是合法的。判断"能不能睡"的标准永远是
**你处在什么上下文**，而不是"这是不是 sysfs"：同一份代码如果在中断里被调用就会炸，
所以 `sensor_apply_interval()` 只从 ioctl 与 sysfs store 两个进程上下文入口调用，
绝不从 `sensor_irq_thread()` 里调用。

**（3）为什么 store 里不该做耗时操作。**

即使能睡，也要短，理由是工程性的、可被追问的：

1. **用户态 write(2) 是同步的**。`echo 1 > interval_ms` 会一直阻塞到 store 返回；
   store 里做一次"同步等硬件转换完成 + 100ms delay"，用户态就卡 100ms，
   脚本里看起来像"写 sysfs 把系统写卡了"。
2. **`of->mutex` 把同一文件的读写串行化**（`fs/kernfs/file.c:343-357`）。
   耗时 store 会阻塞同一属性的并发读者/写者，进而影响别的进程。
3. **store 常常是"配置动作"，应该在配置完立刻返回**，让重活在后台线程/工作队列里做。
   本项目的 `sensor_apply_interval()` 里 `hrtimer_cancel()` 会等待正在跑的定时器回调结束
   （可能正在做一次 I2C 读），这是**有界**的、毫秒级的等待，属于可接受；
   但如果写成"在 store 里主动发起一次 I2C 事务并轮询完成"，就变成不可预期了。
4. **不要在 store 里做 `copy_to_user`/`copy_from_user`**：这里的 buf 已经是内核缓冲，
   用户态数据在 kernfs 层已经拷进来了，无此必要；而在持锁/原子区域做用户拷贝是错误模式。

### 2.5 本项目 4 个属性的语义与锁（逐一对照）

| 属性 | 宏 | 读 | 写 | 同步 |
|---|---|---|---|---|
| `interval_ms` | `DEVICE_ATTR_RW` | `mutex_lock` 下取 `sd->interval_ms`，`sysfs_emit` | `kstrtoul` → `sensor_interval_valid` → `sensor_apply_interval` | `sd->lock` 保护字段；hrtimer 重启在锁外 |
| `seq` | `DEVICE_ATTR_RO` | `READ_ONCE(sd->latest.seq)`，`sysfs_emit` | 无 | 无锁（见下） |
| `i2c_errors` | `DEVICE_ATTR_RO` | `READ_ONCE(sd->i2c_errors)`，`sysfs_emit` | 无 | 无锁 |
| `ring_capacity` | `DEVICE_ATTR_RO` | `sysfs_emit(kfifo_size()/sizeof(sample))` | 无 | 无锁（容量 probe 后不再变） |

**`kstrtoul` 而不是 `simple_strtoul`**（`driver/sensor_char.c:953-958`）：
`simple_strtoul("abc")` 返回 0 且不报错，于是"用户写错"会静默变成"用户要求 0ms"；
`kstrtoul` 返回 `-EINVAL`，错误可被用户态感知。实测：`echo abc > interval_ms`
被拒绝且值不变（log:296 `非数字被拒绝（值保持不变）`）。

**范围判定与生效动作各只有一份**（`driver/sensor_char.c:756-769`）：

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

这个抽取的直接动因是一个真实不一致：阶段 04 之前
`ioctl(SENSOR_IOC_SET_INTERVAL)` 的下限是 10ms，而 sysfs 契约要求 1ms，
**同一个设置走两个入口结果不同**。抽成共用函数后两个入口统一为 `1..60000`
（父 agent 裁定，见 `docs/11-阶段任务书.md` 阶段 04）。ioctl 侧只多两行
（`driver/sensor_char.c:800-802`）：

```c
if (!sensor_interval_valid(interval))
	return -EINVAL;
sensor_apply_interval(sd, interval);
```

测试直接断言"两入口一致"：写 `interval_ms=100` 后 sysfs 读回 100，且
`ioctl(GET_STATS).interval_ms` 也是 100（`tests/phases/04-sysfs-debugfs.sh:45-51`；
实测 log:288 `写入 100 后：sysfs=100  ioctl=100`）。

**为什么 `seq`/`i2c_errors` 用 `READ_ONCE()` 而不是加锁**：
它们是 `u32`，单次对齐读在 CPU 层面不会读到"半个值"；sysfs 属性的一次 read
本来就是一个瞬时快照，不需要与其他字段构成原子整体。用 `READ_ONCE()` 是为了
让编译器不要把它优化成多次读/寄存器缓存，并明确表达"这里存在有意为之的并发访问"。
需要"多个字段必须一致"的场景（`debugfs/stats`）才上 `sd->lock` 互斥。这个区分是
面试里很好的追问点："你为什么这里不加锁？"——答"单值快照不需要跨字段一致性，
加了反而制造无关的锁竞争；要一致性就整体加锁，两者不能混为一谈"。

**`ring_capacity` 为什么存在**（`driver/sensor_char.c:987-998`）：
`kfifo_alloc(&sd->ring, 64 * sizeof(struct sensor_sample), GFP_KERNEL)`
请求 `64 × 24 = 1536` 字节，但 kfifo 会把容量向上取整到 2 的幂 → 实际 2048 字节
→ **真实容量 85 个样本**（实测 `ring_capacity=85`，log:278）。用户态判断
"队列满没满、还能读多少"必须用真实容量，不能拿源码里的 64。
`kfifo_size()` 返回的是字节数，所以要除以 `sizeof(struct sensor_sample)`。

---

## 3. debugfs 接口

### 3.1 `debugfs_create_dir/file` 的返回值语义：ERR_PTR

本项目的创建代码（`driver/sensor_char.c:1286-1294`）：

```c
sd->dbg = debugfs_create_dir(DRV_NAME, NULL);   /* parent=NULL → /sys/kernel/debug/sensor_char */
if (IS_ERR(sd->dbg)) {
	dev_warn(dev, "debugfs unavailable: %ld\n", PTR_ERR(sd->dbg));
	sd->dbg = NULL;                          /* 后续 remove(NULL) 安全 */
} else {
	debugfs_create_file("stats", 0444, sd->dbg, sd, &sensor_dbg_stats_fops);
	debugfs_create_file("ring",  0444, sd->dbg, sd, &sensor_dbg_ring_fops);
	debugfs_create_file("regs",  0444, sd->dbg, sd, &sensor_dbg_regs_fops);
}
```

**为什么 debugfs 失败不是致命错误**：debugfs 是可选调试通道，不是设备功能的一部分。
设备参数与数据通路（sysfs + 字符设备）不依赖它。probe 应该照常成功，
只是把调试接口摘掉。所以处理方式是 `dev_warn` + 指针置 NULL，**不让 probe 失败**。
（这是父 agent 明确的裁定，见 `docs/11-阶段任务书.md` 阶段 04。）

**`ERR_PTR(-ENODEV)` 到底是什么语义**——这里要精确，不能含糊：

内核文档在 `debugfs_create_dir()`/`debugfs_create_file()` 的注释里写的是
"If debugfs is not enabled in the kernel, the value -%ENODEV will be returned"
（如 `fs/debugfs/inode.c:581, 475`）。"not enabled in the kernel" 指 **`CONFIG_DEBUG_FS=n`**：
此时 `include/linux/debugfs.h` 里的静态内联桩函数直接
`return ERR_PTR(-ENODEV);`（`include/linux/debugfs.h:187` 等一处，
各 create 系列桩返回值集中在 `:187/194/202/214/221/229`）。

如果 `CONFIG_DEBUG_FS=y` 但**运行期被禁用挂载**（cmdline `debugfs=no-mount`），
`debugfs_init()` 提前返回 `-EPERM`，`debugfs_registered` 保持 false
（`fs/debugfs/inode.c:899-915, 37`），于是 `start_creating()` 返回
`ERR_PTR(-ENOENT)`（`fs/debugfs/inode.c:341-350`，`:349` 是 `!debugfs_initialized()` 分支）。

还有一种常见误解要澄清：**"用户态没往 `/sys/kernel/debug` 挂 debugfs"并不会让
create 失败**。只要 `CONFIG_DEBUG_FS=y` 且内核正常启动，`start_creating()` 会
`simple_pin_fs(&debug_fs_type, ...)` 把 debugfs 在内核里挂起来
（`fs/debugfs/inode.c:353-357`），create 成功；只是用户态需要
`mount -t debugfs none /sys/kernel/debug` 才看得见。本实验环境的 initramfs 就是这么挂的
（`tests/runner/init.sh:25`）。所以"内核可能没挂 debugfs"这个说法，
严格说是"内核可能没编进 debugfs（→ -ENODEV）或被禁用了挂载（→ -ENOENT 或 -EPERM）"，
而不是"用户没 mount"。

驱动侧对这两种错误一视同仁：`IS_ERR()` 命中就 `dev_warn` + 置 NULL，
不管是 `-ENODEV`、`-ENOENT` 还是 `-EPERM`。这是正确的写法——**调试接口的失败
不应该影响主功能**，且具体错误码不改变处理策略。

（说明：实现记录 `docs/impl/04-sysfs-debugfs-实现记录.md` 把这条简化写成
"生产内核常常不挂 debugfs，`debugfs_create_dir()` 会返回 `ERR_PTR(-ENODEV)`"。
按上面 6.6 源码，`-ENODEV` 的准确触发条件是 `CONFIG_DEBUG_FS=n`；
`CONFIG_DEBUG_FS=y` 而"没挂"在多数情况下反而会成功。结论不变——**都非致命**——
但面试被追问时应当给出上面这个更精确的划分。）

### 3.2 `debugfs_remove_recursive(NULL)` 为什么安全

Linux 6.6 里 `debugfs_remove_recursive` **不是一个独立函数**：

```c
/* include/linux/debugfs.h:98 */
#define debugfs_remove_recursive debugfs_remove
```

它只是 `debugfs_remove()` 的别名。而 `debugfs_remove()` 的第一件事就是：

```c
/* fs/debugfs/inode.c:771-776 */
void debugfs_remove(struct dentry *dentry)
{
	if (IS_ERR_OR_NULL(dentry))
		return;
	...
}
```

`IS_ERR_OR_NULL()` 同时覆盖 NULL 和 `ERR_PTR(...)`，所以：

- `debugfs_remove_recursive(NULL)` 是空操作；
- `debugfs_remove_recursive(ERR_PTR(-ENODEV))` 也是空操作。

这就是为什么本项目清理路径可以无条件调用它、不需要 `if (sd->dbg)` 分支
（`driver/sensor_char.c:1337` probe 失败路径、`:1370` remove 路径）：

```c
debugfs_remove_recursive(sd->dbg);
```

注意它**不会**"少删"：`debugfs_remove()` 内部是
`simple_recursive_removal(dentry, remove_one)`（`fs/debugfs/inode.c:777`），
对目录是递归删除子树的，所以传目录就够，不需要逐个删文件。
另一个要点：**debugfs 文件不会随模块卸载自动消失**，必须显式 remove
（`fs/debugfs/inode.c:767-769` 的注释 "no automatic cleanup of files will happen
when a module is removed, you are responsible here"）。本项目在 probe 失败与
remove 两处都调了。

更妙的是 `IS_ERR_OR_NULL` 也让 `debugfs_create_file()` 的父目录参数可以传
`ERR_PTR`：`__debugfs_create_file()` → `start_creating()` 里
`if (IS_ERR(parent)) return parent;`（`fs/debugfs/inode.c:352-353`）。
不过本项目在 `IS_ERR(sd->dbg)` 时直接把 `sd->dbg` 置 NULL 后就跳过建文件了，
不依赖这个特性。

### 3.3 debugfs 的 full_proxy：文件正被 open 时 remove 也安全

`debugfs_create_file()` 传进去的 fops 不会直接被挂到 inode 上，而是被一层
proxy 包住（`fs/debugfs/inode.c:483-490`）：

```c
struct dentry *debugfs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops)
{
	return __debugfs_create_file(name, mode, parent, data,
				fops ? &debugfs_full_proxy_file_operations :
					&debugfs_noop_file_operations,
				fops);
}
```

proxy 做的事情是：每次操作前 `debugfs_file_get(dentry)` 拿一个引用
（`fs/debugfs/file.c:82`），操作完 `debugfs_file_put()`；
`open` 走 `full_proxy_open()`（`fs/debugfs/file.c:307`），
真正要调用的 fops 通过 `debugfs_real_fops()`（`fs/debugfs/file.c:50-65`）取回。
于是 `debugfs_remove()` 会等待已打开的引用释放再真正删除，避免"文件被删了但还有
read 在途 → use-after-free"。

**代价**：每次操作的函数调用多一层。如果驱动能保证 remove 与访问不会并发
（或有自己的同步），可以用 `debugfs_create_file_unsafe()` 跳过 proxy
（`fs/debugfs/inode.c:496-532`）。本项目的文件都很简单，用安全的
`debugfs_create_file()` 即可，代价可忽略。

另外两个每个 debugfs 文件都要配的成员（`driver/sensor_char.c:1108-1126`）：

- `.open = simple_open`：`fs/libfs.c:723`，作用只是 `file->private_data = inode->i_private`；
  因为 `debugfs_create_file(..., data = sd, ...)` 把 `sd` 放进了 `inode->i_private`，
  于是回调里可以 `file->private_data` 直接拿回 `sd`。
- `.llseek = default_llseek`：`fs/read_write.c:230`，给 `*ppos` 一个默认的推进实现
  （`simple_read_from_buffer` 依赖 `*ppos` 才能续读）。

### 3.4 `simple_read_from_buffer` 与 seq_file：如何规避 4KB 分页

先说本项目为什么**不需要** seq_file：三个 debugfs 文件的输出都很短
（`stats` 约 100 字节、`ring` 最多 `8 行样本 + 1 行头` 约 200 字节、
`regs` 三行约 30 字节），远小于一页，所以直接用
`simple_read_from_buffer()`（`fs/libfs.c:1114-1132`）：

```c
return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
```

它做四件事：处理负 `*ppos`；`pos >= available` 时返回 0（EOF）；
把 `count` 夹到 `available - pos`；`copy_to_user(to, from + pos, count)` 并推进 `*ppos`。
所以它的正确性依赖两点：**内容一次生成完整**、**内容不超过你的内核缓冲 `kbuf`**。
本项目的 handler 每次 read 都重新生成全部文本、内容固定，天然满足。

**如果内容可能超过一页，就必须换 seq_file。** 两种"分页"含义要分清：

1. **show 方向（sysfs 属性）本身已经是 seq_file**：`sysfs_kf_seq_show` 把回调结果
   塞进 seq_file 的缓冲，而 seq_file 的缓冲初值是 `PAGE_SIZE`
   （`fs/seq_file.c:210`），若单条记录装不下会**翻倍扩容**
   （`fs/seq_file.c:245` `m->buf = seq_buf_alloc(m->size <<= 1)`），
   再由 `seq_read_iter()`（`fs/seq_file.c:171-298`）按用户的 read 大小分批拷出，
   续读靠 `m->index/m->from/m->count` 记账。但 sysfs 属性仍然受
   `dev_attr_show` 的 `>= PAGE_SIZE` 警告约束（`drivers/base/core.c:2380-2383`），
   所以**属性的 show 输出不能超过 4095 字节**；超过就该考虑拆成多个属性，
   而不是硬塞。
2. **debugfs 手写 read 方向**：如果你想在一个文件里输出任意长的转储（例如把整张
   寄存器表、整个环形缓冲 dump 出来），用 `seq_open()` 注册
   `struct seq_operations {start,next,show,stop}`，让内核框架按记录逐条产出。
   这样：
   - 不需要自己准备一个能装下全部输出的内核缓冲；
   - 用户态 read 会被分成多次（每次一页以内），框架负责续读；
   - `show` 只需基于 `m->index` 幂等地产出"第 N 条记录"，不依赖"被调用一次"。

   这正是"规避 4KB 分页"的含义：**用 seq_file 把"一次给你全部"换成"按记录流式给你"**。
   反过来说，`simple_read_from_buffer` 方案里，一次 `read(2)` 返回的字节数
   受 `count`（用户缓冲）与 `available` 共同限制，用户态必须准备好"短读要循环"的
   习惯——`cat` 与 `read(2)` 库层会替你做。

   面试里可以补一句：`single_open()`（`fs/seq_file.c:572`）是 seq_file 的快捷方式，
   把整份输出写在一条"记录"里；它靠 seq 缓冲扩容也能超过一页，但整份输出必须能
   放进内存缓冲，且用户态要多次 read 才能拿完。真正需要**大而不定长**的输出时，
   用带 `start/next` 的多记录迭代写法。

### 3.5 为什么调试接口读共享区要沿用主数据通路的同步协议（本项目 `ring` 的坑）

`debugfs/ring` 要打印"最近 8 条样本"。样本有两个可能来源：

- **kfifo（read() 的数据源）**：不行。`kfifo_out()` 是**出队**，调试一次就把用户态
  还没读走的样本吃掉了——**调试行为改变了被观测对象**，用户态会莫名其妙少样本。
- **mmap 共享区（`sd->shm`）**：可以。它是"最近 N 条的只读快照"，读它不影响任何人的状态。

本项目选了共享区，并且**沿用主数据通路写共享区时用的那把锁**
（`driver/sensor_char.c:1059-1066`）：

```c
spin_lock(&sd->shm_lock);
total = sd->shm->count;
widx  = sd->shm->write_idx;
n = total > ARRAY_SIZE(snap) ? ARRAY_SIZE(snap) : total;
start = (widx + SENSOR_SHM_SAMPLES - n) % SENSOR_SHM_SAMPLES;
for (i = 0; i < n; i++)
	snap[i] = sd->shm->samples[(start + i) % SENSOR_SHM_SAMPLES];
spin_unlock(&sd->shm_lock);
/* 锁外格式化 + simple_read_from_buffer */
```

三个"必须这么做"的理由：

1. **同一份数据必须用同一套协议访问**。写方 `sensor_shm_publish()` 在
   `spin_lock(&sd->shm_lock)` 内先置 `seq` 为奇数、改 `samples/write_idx/count`、
   再置偶（`driver/sensor_char.c:300-331`）。内核侧读者只要也拿 `shm_lock`，
   就与写方互斥，**根本不需要看 seq**——seq 协议是给"无法持锁的用户态"用的。
   如果调试读者绕过锁直接读，就会与写方并发，可能拿到"count 已增、样本还没写"的
   自相矛盾快照。
2. **不能在持锁期间做耗时/可能睡眠的事情**。`shm_lock` 是 spinlock，
   而写方是中断线程（`sensor_irq_thread()`），持锁会直接增加中断延迟预算。
   所以只做"把 8 条样本 memcpy 到本地数组"（极短），
   格式化 `scnprintf()` 和 `copy_to_user`（可能睡眠）全部放到锁外。
   特别是 `copy_to_user` **绝不能在 spinlock 里做**。
3. **不能被"更方便"的接口带偏**。`kfifo` 有 `kfifo_peek`/`kfifo_to_user` 之类的
   只读接口，但共享区还存在 `sd->shm`，两条数据源（kfifo 与 shm）的
   `count/dropped` 语义不同；从 shm 取数、并复用写方同一把锁，语义最清晰。

实测证据（`logs/20260914-133749-main-04-sysfs-debugfs.log:325-328`）：

```
count=6 dropped=0 ring_count=6 ring_capacity=85 shm_writes=6 recent=6
sample[0] seq=1 temp_milli=24062
sample[1] seq=2 temp_milli=24187
sample[2] seq=3 temp_milli=24250
```

`stats` 走的是另一条路：它需要"一行里多个字段是同一时刻的一致快照"，
所以持 `sd->lock`（mutex）读取全部字段并格式化，**释放锁后**再
`simple_read_from_buffer()` 拷贝（`driver/sensor_char.c:1032-1041`）：

```c
mutex_lock(&sd->lock);
len = scnprintf(kbuf, sizeof(kbuf),
		"open=%u read=%u irq=%u i2c_err=%u interval=%lu dropped=%u ring_count=%u ring_capacity=%u seq=%u shm_writes=%u\n",
		...);
mutex_unlock(&sd->lock);
return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
```

这里也体现了同一条纪律：**持锁期间只做有界、不睡眠的工作；用户拷贝在锁外**。

### 3.6 `regs`：为什么它不只是"看一眼寄存器"

`debugfs/regs` 用 `regmap_read()` 现场读 `0x00~0x02`
（`driver/sensor_char.c:1096-1103`）：

```c
for (reg = SENSOR_REG_TEMP; reg <= SENSOR_REG_CONFIG; reg++) {
	ret = regmap_read(sd->regmap, reg, &val);
	if (ret)
		len += scnprintf(kbuf + len, sizeof(kbuf) - len,
				 "%02x: <read error %d>\n", reg, ret);
	else
		len += scnprintf(kbuf + len, sizeof(kbuf) - len,
				 "%02x: %04x\n", reg, val);
}
```

它跑的是**与中断线程采集样本完全相同**的寄存器访问路径
（`regmap_read` → regmap_i2c 后端 → `i2c_smbus_read_word_data` → 总线 `smbus_xfer`），
所以它是"regmap 通路真的在工作"最直接的证据，而不是从别处推断。
它还有一个证据价值：`regmap` 的**字节序配置对不对会直接体现在数值上**。
实测（log:314-316）：

```
00: 0188     ← 温度 raw = 0x0188 = 392 → 392/16 = 24.5 ℃（芯片模拟区间 24.0~26.0）
01: 0fa5     ← 湿度 raw = 0x0fa5 = 4005 → 40.05 %RH
02: 0001     ← probe 写入的"连续转换使能"bit0
```

若 `val_format_endian` 配错（regmap 默认 BIG），`0x0188` 会被 swab 成 `0x8801`
→ 数值离谱，这个文件立刻能看出来。**失败时必须打印真实错误码**
（`<read error %d>`）而不是伪造一个 0——否则看日志的人会以为总线正常，
这正是"证据必须真实"的纪律。

---

## 4. 故障注入在驱动测试中的作用

### 4.1 核心命题：没跑过的错误处理等于没写

驱动里 `if (regmap_read() != 0) { sd->i2c_errors++; ... }` 这行代码，
在正常硬件上**永远不会被执行**——因为硬件不会无缘无故 NACK。
于是：

- 编译器不检查这条路径；
- 单元测试跑不到（没有注入就没法构造失败）；
- code review 也只看得出"写了错误处理"，看不出它对不对。

唯一的办法是**制造故障**，让这条路径真的跑起来，并**观测它的后果**
（计数是否精确增长、后续是否恢复、用户态是否还正常）。
本项目阶段 04 用 `virt_i2c` 的注入接口做这件事。

### 4.2 本项目注入点的实现

虚拟控制器暴露一个只写 debugfs 文件 `/sys/kernel/debug/virt_i2c/inject_error`
（`driver/virt_i2c.c:322-327` 的 fops，`:291-310` 的写回调）：

```c
static ssize_t virt_i2c_inject_write(struct file *filp, const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	...
	if (kstrtou32(kbuf, 0, &n))
		return -EINVAL;
	mutex_lock(&chip->lock);
	chip->injected_left = n;
	mutex_unlock(&chip->lock);
	dev_info(chip->dev, "fault injection armed: next %u transfer(s) will fail\n", n);
	return len;
}
```

注入逻辑在**每一次 SMBus 传输的入口**、`chip->lock` 保护下递减
（`driver/virt_i2c.c:167-175`）：

```c
	mutex_lock(&chip->lock);
	chip->transfers++;

	if (chip->injected_left) {
		chip->injected_left--;
		chip->errors++;
		mutex_unlock(&chip->lock);
		dev_info(chip->dev, "injected error: addr=0x%02x reg=0x%02x (left=%u)\n",
			 addr, reg, chip->injected_left);
		return -EIO;
	}
```

上层驱动的错误处理在 `sensor_irq_thread()`
（`driver/sensor_char.c:354-360`）：

```c
	ret = regmap_read(sd->regmap, SENSOR_REG_TEMP, &raw);
	if (ret) {
		sd->i2c_errors++;
		dev_warn_ratelimited(&sd->client->dev, "regmap read failed: %d\n", ret);
		mutex_unlock(&sd->lock);
		return IRQ_HANDLED;
	}
```

### 4.3 为什么是"精确 +N"：记账与顺序

注入 3 次的实测链路（log:334-343）：

```
[    0.841412] virt_i2c virt-i2c: fault injection armed: next 3 transfer(s) will fail
[    0.850688] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=2)
[    0.850884] sensor_char 0-0048: regmap read failed: -5
[    0.871188] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=1)
[    0.871412] sensor_char 0-0048: regmap read failed: -5
[    0.893174] virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=0)
[    0.893400] sensor_char 0-0048: regmap read failed: -5
      · 注入 3 次：i2c_errors 0 → 3（增量 3）
```

"精确 +N"成立的技术前提：

1. **注入计数在总线锁内递减**，所以并发注入/传输不会多扣少扣。
2. **失败发生在底层总线，所以它自然冒泡到驱动**：`smbus_xfer` 返回 `-EIO` →
   `i2c_smbus_read_word_data()` 返回 `-EIO` → `regmap_read()` 返回 `-EIO` →
   驱动的错误分支执行 `sd->i2c_errors++`。整条链没有一处吞掉错误。
3. **驱动是这批注入唯一的常规 I2C 使用者**：采样线程每 `interval_ms` 发起一次
   `regmap_read`，所以"接下来 3 次传输"都会落在采样路径上
   （实测注入前把周期设成 20ms 以便快速观察，`tests/phases/04-sysfs-debugfs.sh:72`）。

**顺序陷阱（写测试时真实踩到）**：注入的是"接下来 N 次传输"，
而读 `debugfs/regs` 自己也会发起 3 次传输（`0x00/0x01/0x02`）。
如果"注入"和"检查 `i2c_errors`"之间读了 `regs`，这 3 次失败会被 `regs` 消耗，
`i2c_errors` 的增量就不再是 3，结论直接错。所以测试脚本的顺序是
**取基准 → 注入 → sleep 1 → 读 `i2c_errors`**，中间不碰任何会发起 I2C 事务的接口
（`tests/phases/04-sysfs-debugfs.sh:116-121`）：

```sh
ie_before=$(cat $SYS/i2c_errors)
echo 3 > /sys/kernel/debug/virt_i2c/inject_error
sleep 1
ie_after=$(cat $SYS/i2c_errors)
delta=$((ie_after - ie_before))
```

"注入后系统仍正常"也做了断言：注入耗尽后采样序号继续增长
（log:343 `恢复期样本序号：107 → 158`），且 `/bin/sensor_test` 退出码 0
（log 检查项 `sensor_test 退出码为 0`）。这证明错误处理没有把设备搞死——
`i2c_errors++` 之后 `return IRQ_HANDLED`，下一次定时器照常触发。

### 4.4 与内核自带 fault-injection 框架的关系

内核有一套通用故障注入框架，核心是 `should_fail()`
（`include/linux/fault-inject.h:51-52`、`lib/fault-inject.c:103,161`），
属性用 `fault_create_debugfs_attr()` 导出到 debugfs
（`include/linux/fault-inject.h:56-57`），支持
`interval,probability,space,times` 等参数（`lib/fault-inject.c:21-34`）。
三个最常用的注入器：

| 注入器 | 配置项 | 注入点 | 源码 |
|---|---|---|---|
| `failslab` | `CONFIG_FAILSLAB` | `kmalloc`/slab 分配失败 | `mm/failslab.c` |
| `fail_page_alloc` | `CONFIG_FAIL_PAGE_ALLOC` | 页分配器失败 | `mm/fail_page_alloc.c:24-42`（`__should_fail_alloc_page`） |
| `fail_function` | `CONFIG_FAIL_FUNCTION` + `CONFIG_FUNCTION_ERROR_INJECTION` | 让带 `ALLOW_ERROR_INJECTION` 标注的函数返回错误 | `kernel/fail_function.c:169-181`（kprobe 入口）、`:279`（`within_error_injection_list`） |

它们的共同前提是 `CONFIG_FAULT_INJECTION`（`lib/Kconfig.debug:1928`）。

**关键实测事实**：本项目的实验内核**没有开**这套框架
（`~/kernel-build/linux-6.6.156/.config:10695` = `# CONFIG_FAULT_INJECTION is not set`），
因此 `/sys/kernel/debug/fail_function`、`fail_page_alloc` 这些接口在这个内核上
**不存在**，本项目的测试不能用它们。（所以 **未验证**：本项目环境下这些注入器的行为。）

两者的关系可以这样讲：

- **层次不同**：内核框架注入的是**内核基础设施**的失败（内存分配、被标注的内核函数），
  面向"通用错误路径 fuzzing"；本项目的注入点选在**设备/总线边界**
  （虚拟芯片对特定寄存器访问返回 `-EIO`），面向"这个驱动的 I2C 错误处理"。
- **互补**：想验证"驱动在 `devm_kzalloc` 失败时是否泄漏已申请的中断"，
  该用 `failslab`；想验证"传感器 NACK/超时后驱动是否正确计数并恢复"，用本项目的注入。
- **可控性**：本项目注入是"精确 N 次"的确定性计数，适合写**断言**
  （`i2c_errors` 精确 +N）；内核框架更偏概率/系统性，适合找偶发路径。
- **`fail_function` 的启示**：它的机制是 kprobe 拦截函数入口并强制返回错误
  （`kernel/fail_function.c:169-181`），本质上和"在 `smbus_xfer` 里提前 return -EIO"
  是同一个思想，只是它拦截的是内核 C 函数而不是设备总线。反过来，本项目做不到
  "让任意内核函数失败"，因为它没有 kprobe 注入器；需要那种能力时必须在
  `CONFIG_FAULT_INJECTION=y` 的内核上测。

**面试里最好的答法**：先说"错误处理必须能被执行到才有意义"，再说
"内核有通用框架，但它是基础设施级的；设备级错误（NACK、超时、CRC）更适合在
总线/设备模型里注入，因为它能精确控制'错几次'并且不依赖内核配置"，
最后补一句"我实验的内核没开 `CONFIG_FAULT_INJECTION`，所以驱动自带的注入点
是唯一可用的手段"——这体现你真的看过配置，而不是背概念。

---

## 5. 面试问答

### Q1：sysfs 和 debugfs 的核心区别是什么？

**答**：三条。①**承诺**：sysfs 是稳定 ABI，格式与语义不能随便改，新属性必须写进
`Documentation/ABI`（`Documentation/filesystems/sysfs.rst:419`）；debugfs 明确
"not serve as a stable ABI"，内核不保证兼容（`Documentation/filesystems/debugfs.rst:15`）。
②**规则**：sysfs 要 one-value-per-file、ASCII、不多行不多字段；debugfs 无规则。
③**面向对象**：sysfs 是对内核对象（device/driver/class/bus）的视图，走
kobject/attribute 模型；debugfs 是开发者自己组织的目录树，不要求对象模型。
选择上：要用户态长期依赖的参数/状态放 sysfs，纯调试信息放 debugfs。

### Q2：为什么统计数字不要放 sysfs？

**答**：① 违反 one-value-per-file：一行 10 个字段的 dump 文件不符合约定。
② **一旦发布就冻结格式**：sysfs 是 ABI，加/删/改字段都是破坏用户态，
而统计的口味恰恰是"经常想加个新指标"。③ 文档化成本：每个 sysfs 属性都要在
`Documentation/ABI` 里登记并承诺兼容，调试信息不配这个成本。
所以本项目把 `Open/read/irq/i2c_err/...` 一行汇总放 `debugfs/stats`，
把单值且稳定的 `seq`/`i2c_errors`/`ring_capacity` 放 sysfs。
**追问："那用户态工具要用统计怎么办？"** 答：用 debugfs、接受它可能变；
真要做成接口，就得设计成多个 sysfs 单值属性并登记 ABI（本项目的
`i2c_errors` 就是这个思路的示范）。

### Q3：sysfs 的 store 里能不能睡眠？

**答**：能。store 在 `read(2)/write(2)` 的进程上下文里被调用
（`vfs_write → kernfs_fop_write_iter → sysfs_kf_write → dev_attr_store → 你的 store`，
`fs/kernfs/file.c:311`、`fs/sysfs/file.c:131`、`drivers/base/core.c:2387`），
不是中断上下文，所以允许睡眠（本项目 store 里就 `mutex_lock` + `hrtimer_cancel`）。
但有两条约束：① 写路径持有 `of->mutex`（`fs/kernfs/file.c:343`），
读路径持有 seq_file 的 `m->lock`（`fs/seq_file.c:182`），同一文件的访问被串行化；
② 用户态 `write(2)` 会同步阻塞，所以**不能做无界耗时操作**——
该交给工作队列/后台线程。另外，**能不能睡取决于上下文，不取决于"这是不是 sysfs"**：
同一段代码被别处从中断上下文调用就会炸。

### Q4：debugfs 为什么不用管 ABI？现实中真的可以随便改吗？

**答**：内核文档明确说 debugfs "is intended to not serve as a stable ABI，
in theory there are no stability constraints"（`Documentation/filesystems/debugfs.rst:15`），
所以加字段、改格式、删文件都不算破坏用户态。但文档下一句就补了
"The real world is not always so simple"——现实里一旦某个 debugfs 接口被
生产工具用了，改它一样会破坏别人。所以准确说法是：**内核给了你"不必承诺"的自由，
但设计时仍要当成"可能要维护很久"来写**。另外 debugfs API 是 GPL-only 导出
（`Documentation/filesystems/debugfs.rst:26`），模块必须 GPL。

### Q5：驱动里怎么做"在线调参"？

**答**：分两种。①**加载期参数**：`module_param()`，只在 `insmod` 时生效
（除非用 `module_param_call` 写 set 回调，但它在任意写时机被调用、上下文受限，
不适合需要加锁/重启定时器的操作），而且改一次要 `rmmod/insmod`，不是"在线"。
②**运行期接口**：sysfs 属性 RW。本项目 `interval_ms` 就是：`store` 里
`kstrtoul` 解析 → 范围校验 → `sensor_apply_interval()` 里 `hrtimer_cancel()` +
`hrtimer_start()` 让新周期立即生效（`driver/sensor_char.c:756-769`）。
要点：**范围判定与生效动作各只有一份**、返回 `count`、非法值返回 `-EINVAL`
且不改状态、生效动作要有界。

### Q6：驱动有两个入口（ioctl 与 sysfs）能配同一个参数，怎么保证它们不打架？

**答**：把"判定"和"生效"抽成**单一实现**，两个入口都调它们。
本项目抽出 `sensor_interval_valid()` + `sensor_apply_interval()`
（`driver/sensor_char.c:756-769`），ioctl（`:800-802`）与 sysfs store（`:956-962`）
都调用。这是被一个真实 bug 逼出来的：抽取前 ioctl 下限 10ms、sysfs 契约 1ms，
"同一个设置走 A 接口成功、走 B 接口失败"。测试里加一条
"写 sysfs 后用 ioctl 读回来必须一致"来固化（`tests/phases/04-sysfs-debugfs.sh:50-51`，
实测 `sysfs=100 ioctl=100`）。**追问："还有哪些多入口不一致的坑？"**
答：读路径的"同一份数据用不同锁/不同计算式"、错误码不一致
（一个返回 `-EINVAL` 一个返回 `-ENOTTY`）、以及 ABI 结构体两边各自定义导致漂移
（本项目用 `driver/sensor_ioctl.h` 单一头文件避免）。

### Q7：怎么验证驱动的错误处理路径是对的？

**答**：**故障注入**。没有注入，`if (ret) { ... }` 分支永远跑不到。
本项目在虚拟 I2C 控制器里加 `/sys/kernel/debug/virt_i2c/inject_error`，
写 N → 接下来 N 次 SMBus 传输返回 `-EIO`（`driver/virt_i2c.c:167-175`）。
验证四件事：① 失败确实传到驱动（`regmap read failed: -5`）；
② 计数精确（`i2c_errors` 0→3，增量 3）；③ 错误处理没破坏后续（采样序号继续增长）；
④ 用户态通路仍正常（`sensor_test` 退出码 0）。
**追问："为什么不用内核的 fail_function？"** 答：那套需要
`CONFIG_FAULT_INJECTION`（本实验内核没开，`.config:10695`），且它注入的是内核函数、
不是设备总线错误；设备级错误在设备模型里注入更精确、更可控、不依赖内核配置。

### Q8：show/store 的缓冲区有多大？超了会怎样？

**答**：都是 `PAGE_SIZE`（4096）。读方向：`sysfs_kf_seq_show` 保证传给 `show` 的
`buf` 至少一页并清零（`fs/sysfs/file.c:47-54`），`dev_attr_show` 检测
`ret >= PAGE_SIZE` 并打印 bad count 警告（`drivers/base/core.c:2380-2383`），
所以 `show` 最多产出 4095 字节，规范写法是 `sysfs_emit()`（内部 `vscnprintf(buf, PAGE_SIZE, ...)`，
`fs/sysfs/file.c:735`）。写方向：`kernfs_fop_write_iter` 把单次写 clamp 到一页
（`fs/kernfs/file.c:322`），`buf` 保证 NUL 结尾。**结论**：sysfs 不是传大块数据的地方，
要传大块用 ioctl / 设备节点 write / bin_attribute / seq_file。
**追问："debugfs 也一样吗？"** 答：debugfs 手写 read 时缓冲是你自己的，
所以是你自己定大小；要输出超过一页又不想自己管缓冲，用 seq_file 由框架分页
（`fs/seq_file.c:210,245`）。本项目三个文件都远小于一页，所以用
`simple_read_from_buffer()`（`fs/libfs.c:1114`）。

### Q9：debugfs 文件正在被读的时候模块被卸载，会 UAF 吗？

**答**：用 `debugfs_create_file()`（而非 `_unsafe`）创建的不会。
它给 inode 装的是 `debugfs_full_proxy_file_operations`（`fs/debugfs/inode.c:483-490`），
每次操作先 `debugfs_file_get()` 拿引用（`fs/debugfs/file.c:82`），
`debugfs_remove()` 会等引用释放。若用 `debugfs_create_file_unsafe()`
就跳过这层保护，需要驱动自己保证不并发。
另外 `debugfs_remove_recursive()` 在 6.6 是 `debugfs_remove` 的别名
（`include/linux/debugfs.h:98`），对 NULL/ERR_PTR 安全（`fs/debugfs/inode.c:771-773`），
所以清理路径可以无条件调用——本项目就是这么做的。

### Q10：你的 debugfs/ring 为什么不去读 kfifo？

**答**：**调试接口不能改变被观测状态**。kfifo 的读是出队（`kfifo_out`），
调试读一次就吃掉用户态还没读的样本，观测行为本身改变了系统行为。
所以改成从 mmap 共享区取"最近 N 条只读快照"，并且**用与写方相同的锁**
（`sd->shm_lock`）保证快照不自相矛盾（`driver/sensor_char.c:1059-1066`）。
持锁期间只做 8 条样本的 memcpy（spinlock，越短越好），
格式化与 `copy_to_user` 都放到锁外（可能睡眠）。
**追问："那怎么保证读到的不是'写了一半'的数据？"** 答：内核读者与写者互斥，
不需要 seq；用户态读者无法持锁，才必须用共享区里那个用户可见的 seq 协议
（阶段 03 的内容）。

### Q11（附加）：为什么 `stats` 用 mutex 而 `seq`/`i2c_errors` 不加锁？

**答**：一致性需求不同。`stats` 一行里 10 个字段必须来自同一时刻（否则读出的
`irq` 与 `dropped` 可能不匹配，看起来像 bug），所以整体持 `sd->lock`。
`seq`/`i2c_errors` 是单值 `u32`，sysfs 读本来就是瞬时快照，
不需要跨字段一致性；用 `READ_ONCE()` 表达"有意为之的并发读"即可，
加锁只会引入无关的锁竞争。**能不能说清"为什么不用锁"比"会不会加锁"更值钱。**

---

## 6. 亲手验证（QEMU 内可复现命令与预期输出）

### 6.1 运行方式

本工作目录在 macOS 上，构建/运行走项目脚本（QEMU 是 TCG + cortex-a72，不是 HVF）：

```bash
# 本 lane（LANE=04 → 构建目录 ~/lab-04）的完整流程
limactl shell dev bash -c 'LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/13-vm-fast-cycle.sh'
LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/20-macos-gen-dtb.sh    # 新 worktree 首次
LC_ALL=C LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/21-macos-sync-artifacts.sh
LC_ALL=C LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/22-macos-run-test.sh 04-sysfs-debugfs 300
```

> **本文的实测值来自已跑过的日志，不是本次重新跑的**：本次任务是只读分析，
> 未构建、未重新执行 QEMU。下面"预期输出"全部可在
> `logs/20260914-133749-main-04-sysfs-debugfs.log` 里按行号复核。

### 6.2 QEMU 内的命令与输出（逐条）

这些命令就是 `tests/phases/04-sysfs-debugfs.sh` 在 QEMU（busybox sh）里执行的命令，
`init.sh` 已 `mount -t debugfs none /sys/kernel/debug`（`tests/runner/init.sh:25`），
模块加载顺序由 `driver/modules.load` 决定：`virt_i2c.ko` → `sensor_char.ko`。

**(1) 看创建是否成功**（log:263-268）：

```sh
cat /proc/kmsg 2>/dev/null | grep -E 'sensor_char|virt_i2c'   # 或 dmesg
```
预期：
```
sensor_char 0-0048: probe: i2c client addr=0x48 interval=500ms
sensor_char 0-0048: user interfaces ready: sysfs=/sys/class/sensor_char/sensor0, debugfs=/sys/kernel/debug/sensor_char
```

**(2) 读 sysfs 四个属性**（log:278）：

```sh
SYS=/sys/class/sensor_char/sensor0
cat $SYS/interval_ms $SYS/seq $SYS/i2c_errors $SYS/ring_capacity
```
预期：`500`、`0`、`0`、`85`（`ring_capacity=85` 是 kfifo 向上取整后的真实容量）。

**(3) 写 sysfs 并确认立即生效 + 与 ioctl 一致**（log:285-288）：

```sh
echo 100 > $SYS/interval_ms
cat $SYS/interval_ms            # → 100
/bin/sensor_stat | grep interval # → interval=100（ioctl(GET_STATS) 同一份数据）
```
预期 dmesg：`sensor_char 0-0048: interval -> 100 ms`

**(4) 边界与非法值**（log:292-300）：

```sh
echo 1     > $SYS/interval_ms; cat $SYS/interval_ms   # → 1     （下界接受）
echo 60000 > $SYS/interval_ms; cat $SYS/interval_ms   # → 60000 （上界接受）
echo 0     > $SYS/interval_ms; cat $SYS/interval_ms   # → 60000（仍不变）
echo 60001 > $SYS/interval_ms; cat $SYS/interval_ms   # → 60000（仍不变）
echo abc   > $SYS/interval_ms; cat $SYS/interval_ms   # → 60000（仍不变）
```
预期 `echo 0 > ...` 会打印类似 `sh: write error: Invalid argument`
（busybox：`nvalid` 是测试脚本 grep 的线索，`tests/phases/04-sysfs-debugfs.sh:63`）。
最后把周期设回可观测值：`echo 20 > $SYS/interval_ms`。

**(5) 读 debugfs 三个文件**（log:305, 314-316, 325-328）：

```sh
DBG=/sys/kernel/debug/sensor_char
cat $DBG/stats
cat $DBG/regs
cat $DBG/ring
```
预期（内容随运行时刻变化，字段名与量级必须一致）：
```
# stats
open=1 read=0 irq=3 i2c_err=0 interval=20 dropped=0 ring_count=3 ring_capacity=85 seq=3 shm_writes=3
# regs
00: 0188        ← 24.5 ℃（392/16），落在芯片模拟区间 24.0~26.0
01: 0fa5        ← 40.05 %RH
02: 0001        ← 连续转换使能
# ring（前 4 行）
count=6 dropped=0 ring_count=6 ring_capacity=85 shm_writes=6 recent=6
sample[0] seq=1 temp_milli=24062
sample[1] seq=2 temp_milli=24187
sample[2] seq=3 temp_milli=24250
```
注意：**读 `regs` 会发起 3 次 I2C 事务**，所以要在故障注入之前读，或注入之后重新取基准。

**(6) 故障注入看计数**（log:334-343）：

```sh
ie_before=$(cat $SYS/i2c_errors)                       # → 0
echo 3 > /sys/kernel/debug/virt_i2c/inject_error       # 接下来 3 次传输失败
sleep 1
ie_after=$(cat $SYS/i2c_errors)                        # → 3（增量 3）
cat /sys/kernel/debug/virt_i2c/stats                   # transfers/errors/injected_left
seq1=$(cat $SYS/seq); sleep 1; seq2=$(cat $SYS/seq)    # 注入后仍在推进
```
预期 dmesg：
```
virt_i2c virt-i2c: fault injection armed: next 3 transfer(s) will fail
virt_i2c virt-i2c: injected error: addr=0x48 reg=0x00 (left=2)
sensor_char 0-0048: regmap read failed: -5
...（共 3 轮，left 递减到 0）
```

**(7) 注入后用户态通路仍正常、无内核告警**（log 检查项）：

```sh
/bin/sensor_test                       # 退出码 0，输出含"测试结束"
dmesg | grep -E 'WARNING:|Call trace:|BUG:'   # 期望无输出
```

### 6.3 复现整批（本工作目录的 lane 04 命令）

```bash
LC_ALL=C LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/22-macos-run-test.sh 04-sysfs-debugfs 300
# 期望结尾：[TEST:END] 04-sysfs-debugfs pass=42 fail=0
# 回归：
LC_ALL=C LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/22-macos-run-test.sh 03-ringbuffer-mmap 40
LC_ALL=C LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/22-macos-run-test.sh 02-i2c-driver 19
LC_ALL=C LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/22-macos-run-test.sh 01-io-models 16
LC_ALL=C LANE=04 bash /Users/lucien/workspace/self-study/projects/wt-04/scripts/22-macos-run-test.sh smoke 15
```

**未验证**：本次没有在 lane 04 重新跑以上命令（任务要求只读分析、不构建），
各阶段 PASS 数来自主 worktree 同源代码的日志（见文首日志表）。

---

## 7. 证据索引与遗留

### 7.1 本阶段"结论 ← 证据"索引

| 结论 | 证据 |
|---|---|
| sysfs 四属性齐全、初值 500/0/0/85 | `logs/20260914-133749-main-04-sysfs-debugfs.log:278` |
| 写 100 立即生效，sysfs 与 ioctl 一致 | 同 log:285-288 |
| 边界 1/60000 接受，0/60001/abc 拒绝且值不变 | 同 log:292-300 |
| debugfs 三文件齐全、`stats` 字段完整 | 同 log:302-312 |
| `regs` 现场读寄存器：`00: 0188 / 01: 0fa5 / 02: 0001` | 同 log:314-316 |
| `ring` 打印最近样本（seq/temp_milli） | 同 log:325-328 |
| 注入 3 次 → `i2c_errors` 精确 +3 | 同 log:334-342 |
| 注入耗尽后采样恢复（seq 107→158） | 同 log:343 |
| dmesg 无 WARNING/Call trace/BUG | 同 log 检查项（测试体之后取样） |
| 04/03/02/01/smoke = 42/40/19/16/15 全绿 | 文首日志表 |
| 实验内核 `CONFIG_DEBUG_FS=y`、`CONFIG_FAULT_INJECTION` 未开 | `~/kernel-build/linux-6.6.156/.config:10453, 10695` |

### 7.2 遗留风险（与实现记录一致）

1. `debugfs/ring` 只打印最近 8 条样本、样本来自共享区（受 `count` 上限 32 限制）；
   要看完整历史用 `read()` 出队——这是**刻意**的（调试接口不改变被观测状态）。
2. `interval_ms` 下限放宽到 1ms 后，用户态可以设到 1ms 使 kfifo 快速溢出
   （`dropped` 增长）。这是阶段 03 定下的"丢新 + 计数"预期行为，不是缺陷。
3. sysfs 属性挂在 class 设备上，`rmmod` 后 `/sys/class/sensor_char/` 整个消失
   （class 也销毁）。若将来做多实例驱动，需要重新考虑该路径的稳定性。
4. 注入是"接下来 N 次传输"的全局计数，任何 I2C 使用者都会消耗它；
   测试必须遵守"注入与检查之间不碰 I2C"的顺序纪律（§4.3）。
5. 本文 §3.1 修正了实现记录里"没挂 debugfs → `-ENODEV`"的简化表述：
   6.6 源码里 `-ENODEV` 对应 `CONFIG_DEBUG_FS=n`，
   `CONFIG_DEBUG_FS=y` 而挂载被禁用时是 `-ENOENT`/`-EPERM`；
   驱动对三者一视同仁（非致命）。代码结论不变。

---

## 附：本文引用到的内核源码（可 `grep -n` 复核）

```
include/linux/device.h:106         struct device_attribute
include/linux/device.h:156         DEVICE_ATTR
include/linux/device.h:179         DEVICE_ATTR_RW
include/linux/device.h:185         DEVICE_ATTR_ADMIN_RW / _MODE
include/linux/device.h:197         DEVICE_ATTR_RO
include/linux/sysfs.h:30           struct attribute
include/linux/sysfs.h:84           struct attribute_group
include/linux/sysfs.h:101          __ATTR
include/linux/sysfs.h:115          __ATTR_RO
include/linux/sysfs.h:138          __ATTR_RW
include/linux/sysfs.h:254          struct sysfs_ops
include/linux/kobject.h:64         struct kobject（含 ktype）
include/linux/kstrtox.h:30         kstrtoul
lib/kstrtox.c:96,108,132           _kstrtoull / '\n' 容忍 / kstrtoull
fs/sysfs/file.c:26                 sysfs_file_ops（取 kobj->ktype->sysfs_ops）
fs/sysfs/file.c:40                 sysfs_kf_seq_show（属性读路径）
fs/sysfs/file.c:101                sysfs_kf_read（PREALLOC 属性路径）
fs/sysfs/file.c:131                sysfs_kf_write（属性写路径）
fs/sysfs/file.c:735                sysfs_emit
fs/sysfs/group.c:107               internal_create_group
fs/sysfs/group.c:175               sysfs_create_group
fs/sysfs/group.c:273               sysfs_remove_group
fs/kernfs/file.c:294,297           kernfs_fop_read_iter → seq_read_iter
fs/kernfs/file.c:311,322,343       kernfs_fop_write_iter（PAGE_SIZE clamp / of->mutex）
drivers/base/core.c:2371           dev_attr_show（含 PAGE_SIZE 警告）
drivers/base/core.c:2387           dev_attr_store
drivers/base/core.c:2399           dev_sysfs_ops
fs/seq_file.c:171                  seq_read_iter
fs/seq_file.c:210,245              seq_file 缓冲 PAGE_SIZE 起始 / 翻倍扩容
fs/seq_file.c:572                  single_open
fs/libfs.c:723                     simple_open
fs/libfs.c:1114                    simple_read_from_buffer
fs/read_write.c:230                default_llseek
fs/debugfs/inode.c:341,349         start_creating（-ENOENT 分支）
fs/debugfs/inode.c:353               simple_pin_fs（内核内挂载）
fs/debugfs/inode.c:475,581         debugfs_create_file/dir 文档（-ENODEV 说明）
fs/debugfs/inode.c:483              debugfs_create_file（proxy fops）
fs/debugfs/inode.c:496              debugfs_create_file_unsafe
fs/debugfs/inode.c:589              debugfs_create_dir
fs/debugfs/inode.c:768,771,773,777  debugfs_remove（NULL/ERR 安全 + 递归删除）
fs/debugfs/inode.c:899,37,914       debugfs_init / debugfs_registered
include/linux/debugfs.h:98          #define debugfs_remove_recursive debugfs_remove
include/linux/debugfs.h:187 等      !CONFIG_DEBUG_FS 桩返回 ERR_PTR(-ENODEV)
fs/debugfs/file.c:50,65             debugfs_real_fops
fs/debugfs/file.c:82               debugfs_file_get
fs/debugfs/file.c:307               full_proxy_open
fs/debugfs/file.c:370               debugfs_full_proxy_file_operations
include/linux/fault-inject.h:17     struct fault_attr
include/linux/fault-inject.h:51,56  should_fail_ex / fault_create_debugfs_attr
lib/fault-inject.c:21,103,161       注入参数解析 / should_fail_ex / should_fail
mm/fail_page_alloc.c:24,42          __should_fail_alloc_page
kernel/fail_function.c:169,279      fei_kprobe_handler / within_error_injection_list
lib/Kconfig.debug:1928,1942,1985    FAULT_INJECTION / FAIL_PAGE_ALLOC / FAIL_FUNCTION
Documentation/filesystems/sysfs.rst:62,419
Documentation/filesystems/debugfs.rst:13,15,26
Documentation/filesystems/configfs.rst:16,34,63
Documentation/ABI/README           四档稳定性
```
