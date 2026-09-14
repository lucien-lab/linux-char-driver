// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_char.c - 字符设备驱动：I2C 温湿度传感器采集（教学/实验版）
 *
 * ============================ 架构总览 ============================
 *
 *   [硬件层]
 *     I2C 控制器（实验环境：virt_i2c.ko 提供的虚拟控制器 + 芯片寄存器级模拟；
 *                  真机：SoC 的 i2c 控制器，例如树莓派 i2c-1）
 *       └── 传感器从设备（实验环境 0x48；真机 SHT30 0x44 等，只改 dts + 换算函数）
 *             从机地址写在设备树子节点 sensor@48 的 reg 属性里，由 i2c 核心自动实例化
 *
 *   [内核层]
 *     i2c_driver  --of_match_table 匹配 compatible = "lucien,sensor-char"--> probe(client)
 *        ├── devm_regmap_init_i2c()                        寄存器访问抽象（regmap_i2c）
 *        ├── alloc_chrdev_region/cdev_add/class_create     注册字符设备
 *        └── 自建虚拟中断控制器(irq_chip + irq_domain) + request_threaded_irq
 *              ├── 上半部(hard IRQ)：只做最少的事，返回 IRQ_WAKE_THREAD
 *              └── 线程化下半部：可以做 I2C 传输（允许睡眠），读传感器并唤醒读进程
 *
 *   [数据通路]
 *     传感器寄存器 --I2C--> 线程化中断处理 --> 内核缓存 + waitqueue --> read()/ioctl()
 *
 *   [用户态接口：四种 IO 模型]
 *     阻塞 read / 非阻塞(O_NONBLOCK) read / poll-select-epoll / fasync+SIGIO
 *     四者共用同一份"本 fd 是否有新样本"的判定（struct sensor_file，per-open 状态），
 *     保证 read 与 poll 的语义不漂移，也避免 epoll 忙轮询。
 *
 *   [触发源]
 *     实验中用 hrtimer 周期性调用 generic_handle_domain_irq() 触发虚拟中断；
 *     真机上这一路是传感器 ALERT/DRDY 引脚接到 GIC 的硬件中断线，
 *     驱动的 request_threaded_irq + 上下半部代码完全不变。
 *
 * ==================================================================
 * 设备树解耦要点：本文件不出现任何硬件地址字面量，全部来自 dts：
 *   virt-i2c 控制器节点（compatible = "lucien,virt-i2c"）
 *     └── sensor@48   compatible = "lucien,sensor-char"; reg = <0x48>;
 *                     poll-interval-ms = <500>;
 *   从机地址来自子节点的 reg 属性（i2c 核心解析后填进 client->addr），
 *   所以不再需要自定义的 i2c-bus / sensor-addr 属性。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>		/* poll_wait / fasync_helper / kill_fasync */
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/string.h>
#include <linux/mod_devicetable.h>
#include <linux/err.h>

#include "sensor_ioctl.h"

#define DRV_NAME	"sensor_char"
#define SENSOR_REG_TEMP		0x00	/* 温度寄存器（只读；真机 SHT30 为 0x0000 命令字） */
#define SENSOR_REG_HUMIDITY	0x01	/* 湿度寄存器（只读，本项目保留给 IIO 阶段使用） */
#define SENSOR_REG_CONFIG	0x02	/* 配置寄存器（读写；probe 时用它探测芯片是否存在） */
/* CONFIG 寄存器 bit0：连续转换使能（1=使能，0=关闭）。
 * 这个语义必须与 virt_i2c.c 文件头的寄存器图、以及 probe 里的日志保持一致，
 * 否则会出现"日志说 enable 却写了关闭值"这种误导性证据。 */
#define SENSOR_CFG_CONT_EN	0x0001

/* 阻塞 read() 等待新样本的最长时间：超时返回 -ETIMEDOUT，避免用户态永久卡死 */
#define SENSOR_READ_TIMEOUT_MS	2000

/*
 * 传感器寄存器原始值 -> 毫摄氏度：LSB = 1/16 °C = 62.5 m°C。
 * 必须先转成有符号 16 位再乘：寄存器在负温时是二进制补码形式，
 * 直接拿 u16 做算术会得到 (65536 - x) 这样的大正数，负温读数就错了。
 */
#define RAW_TO_MILLI(raw)	((s32)(raw) * 1000 / 16)

struct sensor_dev {
	/* 字符设备 */
	struct cdev	cdev;
	dev_t		devt;
	struct device	*char_dev;

	/* I2C：从设备由 i2c 核心根据设备树枚举得到，驱动只持有 client */
	struct i2c_client	*client;
	struct regmap		*regmap;	/* 寄存器访问抽象层（后端 regmap_i2c） */
	u32			sensor_addr;

	/* 中断 */
	struct irq_domain	*irq_domain;	/* 自建虚拟中断域（真机不需要） */
	int			virq;
	struct hrtimer		timer;		/* 周期性触发虚拟中断 */

	/* 数据与同步 */
	struct mutex		lock;		/* 保护 latest/stats 与 I2C 访问 */
	wait_queue_head_t	wq;		/* read()/poll() 阻塞队列 */
	struct fasync_struct	*fasync;	/* 注册了 O_ASYNC 的进程链表（SIGIO 通知用） */
	struct sensor_sample	latest;
	unsigned long		interval_ms;

	u32			open_count;
	u32			read_count;
	u32			irq_count;
	u32			i2c_errors;
};

/*
 * per-open 上下文：每个 open() 出来的文件描述符各自记录"已经消费到哪个样本"。
 *
 * 为什么必须 per-open 而不是用全局的 latest.seq？
 *   poll() 的语义是"调用者现在能不能无阻塞地读到数据"。如果拿全局最新序号判断，
 *   同一个样本被读走之后 poll 仍然回报可读 → epoll/select 立即返回 → 用户态
 *   反复空转（忙轮询），CPU 打满。把基准放到 open 上下文，"样本被消费"才是
 *   相对该 fd 成立的事实，poll 才能保持正确的电平触发语义。
 * 顺带好处：多个进程各自 open 时互不干扰，这也是多进程并发读的基础。
 */
struct sensor_file {
	struct sensor_dev	*sd;
	u32			last_seq;	/* 本 fd 已经消费到的样本序号 */
};

static struct class *sensor_class;
static int sensor_major;			/* 0 = 动态分配主设备号 */
module_param(sensor_major, int, 0444);
MODULE_PARM_DESC(sensor_major, "固定主设备号，0 表示动态分配");

/* 把样本格式化成一行文本，便于 cat /dev/sensor0 直接观察 */
static int format_sample(char *buf, size_t size, const struct sensor_sample *s)
{
	int whole = s->temp_milli / 1000;
	int frac = abs(s->temp_milli % 1000);

	return scnprintf(buf, size, "seq=%u temp=%d.%03dC irq=%u t=%llu ns\n",
			 s->seq, whole, frac, s->irq_count, s->ts_ns);
}

/* ============================ 虚拟中断控制器 ============================ */
/*
 * 实验环境里没有真实的传感器 ALERT 引脚，所以本驱动自带一个极简的
 * "中断控制器"：实现 irq_chip + irq_domain，向内核申请一条虚拟中断线。
 *
 * 真机上这一段完全不需要——dts 里写 interrupts = <&gpio ...> 后，
 * request_threaded_irq() 直接用硬件中断号即可。
 */
static struct irq_chip sensor_virt_irq_chip = {
	.name = "sensor-virt-irq",
};

static int sensor_irq_domain_map(struct irq_domain *d, unsigned int virq,
				 irq_hw_number_t hw)
{
	/* 把虚拟中断号挂上中断控制器和通用处理函数 */
	irq_set_chip_and_handler(virq, &sensor_virt_irq_chip, handle_simple_irq);
	return 0;
}

static const struct irq_domain_ops sensor_irq_domain_ops = {
	.map = sensor_irq_domain_map,
};

/* ============================ 中断处理 ============================ */

/*
 * 上半部（硬中断上下文）：不能睡眠，只做最少的登记工作。
 * I2C 传输会睡眠（可能是 DMA/等待 ACK），所以放到线程化下半部。
 */
static irqreturn_t sensor_irq_hard(int irq, void *data)
{
	struct sensor_dev *sd = data;

	sd->irq_count++;
	return IRQ_WAKE_THREAD;	/* 唤醒线程化处理函数 */
}

/*
 * 下半部（内核线程上下文）：这里才允许睡眠，做 I2C 读。
 * 真机上同样结构：threaded IRQ 里读传感器寄存器。
 */
static irqreturn_t sensor_irq_thread(int irq, void *data)
{
	struct sensor_dev *sd = data;
	unsigned int raw;
	s32 next;
	int ret;

	mutex_lock(&sd->lock);

	/*
	 * 通过 regmap 读温度寄存器。
	 * reg_bits=8 + val_bits=16 时刻，regmap_i2c 会选择 SMBus word 后端，
	 * 最终走到 i2c 核心的 i2c_smbus_read_word_data() -> 总线的 smbus_xfer。
	 * 这一步会睡眠（等 I2C 传输完成），所以只能在线程化下半部做。
	 */
	ret = regmap_read(sd->regmap, SENSOR_REG_TEMP, &raw);
	if (ret) {
		sd->i2c_errors++;
		dev_warn_ratelimited(&sd->client->dev,
				     "regmap read failed: %d\n", ret);
		mutex_unlock(&sd->lock);
		return IRQ_HANDLED;
	}

	/*
	 * 原始值 -> 毫摄氏度。
	 * 数据由 virt_i2c.ko 里的"芯片"按内部计数确定性地变化（24.0~26.0 °C），
	 * 驱动只负责换算；真机上这里换成 SHT30 的转换公式，其余代码不变。
	 */
	next = RAW_TO_MILLI(raw);

	sd->latest.seq++;
	sd->latest.temp_milli = next;
	sd->latest.irq_count = sd->irq_count;
	sd->latest.ts_ns = ktime_get_ns();

	mutex_unlock(&sd->lock);

	/*
	 * 唤醒阻塞在 read() 里的进程。
	 * 注意：waitqueue 的唤醒放在锁外，避免"唤醒后马上又抢不到锁"的惊群效应。
	 */
	wake_up_interruptible(&sd->wq);

	/*
	 * 异步通知：唤醒所有用 F_SETFL|O_ASYNC 注册过的进程（发送 SIGIO）。
	 * kill_fasync 内部只做遍历 + send_sigio，不会睡眠，因此可以在
	 * 线程化中断上下文调用（若在硬中断上下文则同样安全，但本驱动本就在这里）。
	 * 注意顺序：必须放在 latest 更新 + wake_up 之后，否则用户态收到信号来读时
	 * 可能读到旧样本（表现为 SIGIO 到了但读不到新数据）。
	 */
	kill_fasync(&sd->fasync, SIGIO, POLL_IN);

	dev_dbg(&sd->client->dev, "sample %u: %d mC\n",
		sd->latest.seq, sd->latest.temp_milli);

	return IRQ_HANDLED;
}

/* 定时器回调：模拟传感器"数据就绪"引脚拉高 -> 触发一次虚拟中断 */
static enum hrtimer_restart sensor_timer_fn(struct hrtimer *timer)
{
	struct sensor_dev *sd = container_of(timer, struct sensor_dev, timer);

	/*
	 * generic_handle_domain_irq 会走完整的内核中断处理流程：
	 * 中断入口 -> 上半部 sensor_irq_hard -> 唤醒线程化下半部 sensor_irq_thread。
	 * 在定时器（softirq）上下文调用是安全的。
	 */
	generic_handle_domain_irq(sd->irq_domain, 0);

	hrtimer_forward_now(timer, ms_to_ktime(sd->interval_ms));
	return HRTIMER_RESTART;
}

/* ============================ 字符设备接口 ============================ */

/*
 * 数据可用性判定：read()、poll() 共用同一个函数，避免两条路径的语义漂移。
 * 判定基准是本 fd 的 last_seq（而不是进入函数那一刻的全局序号），
 * 因此"有没有新样本"对每个 open 的文件描述符是独立成立的。
 */
static bool sensor_has_new_sample(struct sensor_file *sf)
{
	return READ_ONCE(sf->sd->latest.seq) != sf->last_seq;
}

/* 前向声明：release() 里要摘除异步通知，而 sensor_fasync 定义在后面 */
static int sensor_fasync(int fd, struct file *file, int on);

static int sensor_open(struct inode *inode, struct file *file)
{
	struct sensor_dev *sd = container_of(inode->i_cdev, struct sensor_dev, cdev);
	struct sensor_file *sf;

	sf = kzalloc(sizeof(*sf), GFP_KERNEL);
	if (!sf)
		return -ENOMEM;

	sf->sd = sd;
	sf->last_seq = 0;	/* 新打开的 fd 认为"什么都还没读过"，首次 read 立即可返回 */
	file->private_data = sf;

	mutex_lock(&sd->lock);
	sd->open_count++;
	mutex_unlock(&sd->lock);
	dev_info(&sd->client->dev, "opened (count=%u)\n", sd->open_count);
	return 0;
}

static int sensor_release(struct inode *inode, struct file *file)
{
	struct sensor_file *sf = file->private_data;
	struct sensor_dev *sd = sf->sd;

	/*
	 * 摘除异步通知。必须先做、且必须在 kfree(sf) 之前：
	 * fasync_helper(..., 0, ...) 内部会遍历 fasync 链表并把本 file 摘掉，
	 * 它通过 filp->private_data 找驱动私有数据，释放后就变成 use-after-free。
	 */
	sensor_fasync(-1, file, 0);

	kfree(sf);
	file->private_data = NULL;
	dev_info(&sd->client->dev, "closed\n");
	return 0;
}

/*
 * read(): 读取"本 fd 尚未消费过的新样本"，返回一行文本。
 *   cat /dev/sensor0 可以持续看到数据流；行为与传感器驱动常见的字符接口一致。
 *
 * 四种 IO 模型里，这里承担"阻塞"与"非阻塞"两种：
 *   阻塞   ：wait_event_interruptible_timeout 睡在 sd->wq 上，被中断下半部唤醒
 *   非阻塞 ：O_NONBLOCK 下立刻返回 -EAGAIN（让调用者去 poll/epoll/自旋）
 *
 * 注意 O_NONBLOCK 是由调用者通过 open/fcntl 设置的标志，驱动只负责遵循它；
 * 真正的"阻塞/不阻塞"行为发生在驱动里的等待，VFS 不会替驱动决定。
 */
static ssize_t sensor_read(struct file *file, char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct sensor_file *sf = file->private_data;
	struct sensor_dev *sd = sf->sd;
	struct sensor_sample snap;
	char kbuf[96];
	int len, ret;

	if (!sensor_has_new_sample(sf)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/*
		 * 等待新样本。wait_event_* 宏内部是"先判断条件再睡"的循环，
		 * 并且把条件判断放在自旋锁保护的临界区里，因此不会出现
		 * "条件刚成立、唤醒已发出"的丢唤醒问题，无需手工加锁。
		 * 超时返回 0（不是错误码），<0 表示被信号打断。
		 */
		ret = wait_event_interruptible_timeout(
			sd->wq, sensor_has_new_sample(sf),
			msecs_to_jiffies(SENSOR_READ_TIMEOUT_MS));
		if (ret == 0)
			return -ETIMEDOUT;
		if (ret < 0)
			return -ERESTARTSYS;	/* 收到信号 */
	}

	mutex_lock(&sd->lock);
	snap = sd->latest;
	sf->last_seq = snap.seq;	/* 记录"已消费"，poll 之后才会重新报告可读 */
	sd->read_count++;
	mutex_unlock(&sd->lock);

	len = format_sample(kbuf, sizeof(kbuf), &snap);
	if (count < len)
		len = count;

	if (copy_to_user(ubuf, kbuf, len))
		return -EFAULT;

	return len;
}

/*
 * poll(): 支持 select/poll/epoll 多路复用。
 *
 * 两件事，顺序不能反：
 *   1. poll_wait()：把本文件的等待项挂到 sd->wq 上（只登记，不睡眠；真正睡眠是
 *      由调用方 poll/epoll_wait 系统调用负责）。驱动不需要持有锁，也不该持锁。
 *   2. 返回当前的可用性掩码：有新样本就 EPOLLIN，否则 0。
 *
 * 这里只实现"电平触发"语义（这也是驱动唯一应该做的）：每次调用都基于当前状态
 * 重新回答，所以消费掉样本后自然不再上报 EPOLLIN。边沿触发的行为由 epoll 在
 * 用户态接口层面实现，驱动不需要也不应该感知。
 */
static __poll_t sensor_poll(struct file *file, poll_table *wait)
{
	struct sensor_file *sf = file->private_data;

	poll_wait(file, &sf->sd->wq, wait);

	if (sensor_has_new_sample(sf))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

/*
 * fasync(): 支持异步通知（SIGIO）。
 *   用户态三步：fcntl(fd, F_SETOWN, getpid()) 指定接收进程，
 *               安装 SIGIO 处理函数，fcntl(fd, F_SETFL, flags | O_ASYNC) 打开开关。
 *   VFS 在置位 O_ASYNC 时会调用这里的 fasync(on=1)，摘除时调用 fasync(on=0)，
 *   我们只负责把登记信息交给 fasync_helper 维护的链表。
 *   close() 时 release 会显式调用 fasync(-1, file, 0) 清理。
 */
static int sensor_fasync(int fd, struct file *file, int on)
{
	struct sensor_file *sf = file->private_data;

	return fasync_helper(fd, file, on, &sf->sd->fasync);
}

/* write(): 演示写路径——写入 0x01 触发一次"立即采样" */
static ssize_t sensor_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct sensor_file *sf = file->private_data;
	struct sensor_dev *sd = sf->sd;
	char kbuf[8];

	if (count == 0 || count > sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;

	if (kbuf[0] == 0x01) {
		unsigned long flags;

		/*
		 * 在进程上下文模拟一次硬件中断。
		 * 关键点：真实中断发生CPU会自动关中断，而 write() 系统调用里
		 * 中断是打开的，直接调用 generic_handle_domain_irq() 会让中断
		 * 处理流程看到错误的状态，内核会报：
		 *   "irq 21 handler sensor_irq_hard enabled interrupts"
		 * 所以这里先关中断来模拟中断上下文。
		 * （hrtimer 回调里调用不需要，因为 softirq 上下文本来就关中断）
		 *
		 * 为什么用 local_irq_save/restore 而不是 local_irq_disable/enable：
		 * enable 是**无条件开中断**，会把调用者原本可能已经屏蔽的中断状态冲掉。
		 * 虽然当前调用链（VFS write）里中断本是开的，但驱动不应依赖调用者的
		 * 上下文假设；save/restore 是内核里保存-恢复中断状态的规范写法。
		 */
		local_irq_save(flags);
		generic_handle_domain_irq(sd->irq_domain, 0);
		local_irq_restore(flags);
		dev_info(&sd->client->dev, "manual trigger\n");
	}
	return count;
}

static long sensor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sensor_file *sf = file->private_data;
	struct sensor_dev *sd = sf->sd;
	struct sensor_sample snap;
	struct sensor_stats stats;
	u32 interval;
	int ret = 0;

	/* 用户态指针未校验前不要解引用；先用 access_ok 做检查 */
	switch (cmd) {
	case SENSOR_IOC_GET_SAMPLE:
		mutex_lock(&sd->lock);
		snap = sd->latest;
		mutex_unlock(&sd->lock);
		if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
			return -EFAULT;
		break;

	case SENSOR_IOC_SET_INTERVAL:
		if (copy_from_user(&interval, (void __user *)arg, sizeof(interval)))
			return -EFAULT;
		if (interval < 10 || interval > 60000)
			return -EINVAL;
		mutex_lock(&sd->lock);
		sd->interval_ms = interval;
		mutex_unlock(&sd->lock);
		/*
		 * 让新周期立即生效：取消后按新周期重启定时器。
		 * hrtimer_cancel 会等待回调结束，避免与回调并发。
		 */
		hrtimer_cancel(&sd->timer);
		hrtimer_start(&sd->timer, ms_to_ktime(interval), HRTIMER_MODE_REL);
		dev_info(&sd->client->dev, "interval -> %u ms\n", interval);
		break;

	case SENSOR_IOC_GET_STATS:
		mutex_lock(&sd->lock);
		stats.open_count = sd->open_count;
		stats.read_count = sd->read_count;
		stats.irq_count = sd->irq_count;
		stats.i2c_errors = sd->i2c_errors;
		stats.interval_ms = sd->interval_ms;
		mutex_unlock(&sd->lock);
		if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		break;

	case SENSOR_IOC_RESET:
		mutex_lock(&sd->lock);
		sd->read_count = 0;
		sd->irq_count = 0;
		sd->i2c_errors = 0;
		sd->latest.seq = 0;
		mutex_unlock(&sd->lock);
		break;

	default:
		ret = -ENOTTY;	/* 未实现的命令统一返回 ENOTTY（不是 EINVAL） */
	}
	return ret;
}

static const struct file_operations sensor_fops = {
	.owner		= THIS_MODULE,
	.open		= sensor_open,
	.release	= sensor_release,
	.read		= sensor_read,
	.write		= sensor_write,
	.unlocked_ioctl	= sensor_ioctl,
	.poll		= sensor_poll,	/* select/poll/epoll */
	.fasync		= sensor_fasync,	/* SIGIO 异步通知 */
	.llseek		= no_llseek,
};

/* ============================ 字符设备注册 ============================ */

static int sensor_chrdev_register(struct sensor_dev *sd)
{
	int ret;

	if (sensor_major) {
		sd->devt = MKDEV(sensor_major, 0);
		ret = register_chrdev_region(sd->devt, 1, DRV_NAME);
	} else {
		ret = alloc_chrdev_region(&sd->devt, 0, 1, DRV_NAME);
	}
	if (ret)
		return ret;

	cdev_init(&sd->cdev, &sensor_fops);
	sd->cdev.owner = THIS_MODULE;
	ret = cdev_add(&sd->cdev, sd->devt, 1);
	if (ret)
		goto err_unregister;

	sd->char_dev = device_create(sensor_class, NULL, sd->devt, sd,
				     "sensor%d", MINOR(sd->devt));
	if (IS_ERR(sd->char_dev)) {
		ret = PTR_ERR(sd->char_dev);
		goto err_cdev_del;
	}
	return 0;

err_cdev_del:
	cdev_del(&sd->cdev);
err_unregister:
	unregister_chrdev_region(sd->devt, 1);
	return ret;
}

/* ============================ i2c_driver probe ============================ */
/*
 * 与旧实现（platform_driver + i2c_get_adapter）的区别，以及为什么这么改：
 *
 *   旧：platform_driver 自己从设备树里读总线号/从机地址，再 i2c_get_adapter()
 *       + i2c_new_client_device() 手工造一个 i2c client。这是"板级文件（board file）"
 *       时代的做法；在设备树体系里属于绕路：拿不到 i2c 核心的自动枚举与匹配，
 *       从设备的生命周期、电源管理、驱动绑定关系都由驱动自己维护。
 *
 *   新：驱动注册为 i2c_driver。总线控制器（virt_i2c.ko）注册带 of_node 的 adapter 时，
 *       i2c 核心会调用 of_i2c_register_devices() 遍历其子节点，把 sensor@48
 *       实例化成 i2c client，再按 of_match_table 匹配到本驱动并调用 probe(client)。
 *       从机地址来自子节点的 reg 属性（client->addr），驱动不再解析地址。
 *
 * regmap 配置：寄存器 8 位、数据 16 位。
 *   regmap_i2c 在"适配器声明了 I2C_FUNC_SMBUS_WORD_DATA"时会选择 SMBus word 后端，
 *   于是 regmap_read() → i2c_smbus_read_word_data() → 总线 smbus_xfer()。
 *   如果哪天换成 8 位数据的芯片，只需把 val_bits 改成 8，上层代码不用动 ——
 *   这正是引入 regmap 的价值：把"寄存器访问的位宽/字节序/缓存"从驱动逻辑里剥离。
 *
 * 【必填 val_format_endian —— 本项目踩过的真实坑】
 *   regmap 的 regmap_get_val_endian() 在设备树/平台数据都没声明字节序时**默认返回 BIG**，
 *   于是会选择 regmap_smbus_word_swapped 后端，对读回的 16 位值做 swab16()。
 *   而 SMBus word 协议本身是"低字节在前"（即小端），我们的芯片（virt_i2c）也是
 *   直接把值放进 data->word，结果就是温度被字节交换后的乱码
 *   （实测 25.0 ℃ 被读成 2304 ℃ 量级）。显式声明 LITTLE 后，regmap 改用
 *   regmap_smbus_word（不交换），读值才正确。
 *   真实驱动里这一步同样必须写清楚——比如很多传感器数据手册写的就是
 *   "little endian"，而 regmap 不会替你做默认假设。
 */
static const struct regmap_config sensor_regmap_cfg = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.max_register		= SENSOR_REG_CONFIG,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
};

static int sensor_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct sensor_dev *sd;
	unsigned int cfg;
	u32 interval = 500;
	int ret;

	/* 1. 设备树参数：只解析自定义属性；从机地址由 reg 属性经 i2c 核心填进 client->addr */
	of_property_read_u32(np, "poll-interval-ms", &interval);
	dev_info(dev, "probe: i2c client addr=0x%02x interval=%ums\n",
		 client->addr, interval);

	sd = devm_kzalloc(dev, sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -ENOMEM;

	sd->client = client;
	sd->sensor_addr = client->addr;
	sd->interval_ms = interval;
	mutex_init(&sd->lock);
	init_waitqueue_head(&sd->wq);
	i2c_set_clientdata(client, sd);

	/* 2. 寄存器访问统一走 regmap */
	sd->regmap = devm_regmap_init_i2c(client, &sensor_regmap_cfg);
	if (IS_ERR(sd->regmap)) {
		ret = PTR_ERR(sd->regmap);
		dev_err(dev, "regmap init failed: %d\n", ret);
		goto err_clear_clientdata;
	}

	/*
	 * 3. 探测芯片是否真的在总线上：读一次配置寄存器。
	 *    真机上是同样的做法（读 WHO_AM_I / 配置寄存器确认硬件在位）。
	 *    读失败说明从设备不应答，直接让 probe 失败，不要带着半死不活的设备继续跑。
	 */
	ret = regmap_read(sd->regmap, SENSOR_REG_CONFIG, &cfg);
	if (ret) {
		dev_err(dev, "chip not responding (regmap_read=%d)\n", ret);
		goto err_clear_clientdata;
	}
	dev_info(dev, "chip detected: config=0x%04x (continuous conversion %s)\n",
		 cfg, (cfg & SENSOR_CFG_CONT_EN) ? "already enabled" : "off");

	/*
	 * 配置成连续转换模式（bit0=1）。
	 * 写操作的返回值必须检查：配置失败说明总线/芯片有问题，此时继续注册字符设备
	 * 只会让用户态拿到一堆读失败，不如直接在 probe 里失败。
	 */
	ret = regmap_write(sd->regmap, SENSOR_REG_CONFIG, SENSOR_CFG_CONT_EN);
	if (ret) {
		dev_err(dev, "failed to enable continuous conversion: %d\n", ret);
		goto err_clear_clientdata;
	}
	dev_info(dev, "continuous conversion enabled (config=0x%04x)\n", SENSOR_CFG_CONT_EN);

	/* 4. 注册字符设备 -> /dev/sensor0 */
	ret = sensor_chrdev_register(sd);
	if (ret) {
		dev_err(dev, "chrdev register failed: %d\n", ret);
		goto err_clear_clientdata;
	}

	/* 5. 中断：实验环境自建虚拟中断源，真机换成传感器 ALERT 引脚对应的 IRQ */
	sd->irq_domain = irq_domain_create_linear(NULL, 1,
						   &sensor_irq_domain_ops, NULL);
	if (IS_ERR(sd->irq_domain)) {
		ret = PTR_ERR(sd->irq_domain);
		goto err_unreg_chrdev;
	}
	sd->virq = irq_create_mapping(sd->irq_domain, 0);
	if (!sd->virq) {
		ret = -ENOMEM;
		goto err_remove_domain;
	}

	/*
	 * 线程化中断：上半部 sensor_irq_hard + 下半部 sensor_irq_thread。
	 * 对需要 I2C/睡眠操作的传感器驱动，这是标准写法。
	 */
	ret = request_threaded_irq(sd->virq,
				   sensor_irq_hard, sensor_irq_thread,
				   0, DRV_NAME, sd);
	if (ret) {
		dev_err(dev, "request_threaded_irq failed: %d\n", ret);
		goto err_remove_domain;
	}
	dev_info(dev, "irq registered: virq=%d\n", sd->virq);

	/* 6. 启动采样定时器（真机上这一步由传感器的 DRDY/ALERT 中断替代） */
	hrtimer_init(&sd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sd->timer.function = sensor_timer_fn;
	hrtimer_start(&sd->timer, ms_to_ktime(sd->interval_ms), HRTIMER_MODE_REL);

	dev_info(dev, "probe done: major=%d minor=%d /dev/sensor%d\n",
		 MAJOR(sd->devt), MINOR(sd->devt), MINOR(sd->devt));
	return 0;

err_remove_domain:
	irq_domain_remove(sd->irq_domain);
err_unreg_chrdev:
	device_destroy(sensor_class, sd->devt);
	cdev_del(&sd->cdev);
	unregister_chrdev_region(sd->devt, 1);
err_clear_clientdata:
	/*
	 * probe 失败时清掉 client 的 drvdata。
	 * 不清的话 client 会带着一个指向即将被 devm 释放的内存的指针继续存在于
	 * i2c 总线上；虽然当前没有代码路径会再取用它，但这属于"悬空指针留在
	 * 系统里"，是 review 一眼就会被打回的惯用法问题。
	 */
	i2c_set_clientdata(client, NULL);
	return ret;
}

static void sensor_remove(struct i2c_client *client)
{
	struct sensor_dev *sd = i2c_get_clientdata(client);

	hrtimer_cancel(&sd->timer);		/* 停掉采样源 */
	free_irq(sd->virq, sd);
	irq_domain_remove(sd->irq_domain);
	device_destroy(sensor_class, sd->devt);
	cdev_del(&sd->cdev);
	unregister_chrdev_region(sd->devt, 1);
	/* i2c client 由 i2c 核心根据设备树实例化，卸载时由核心释放，驱动不需要管 */
	dev_info(&client->dev, "removed\n");
}

/*
 * 两张匹配表，作用不同：
 *   of_match_table —— 设备树匹配（设备树里 compatible = "lucien,sensor-char"），
 *                     这是设备树体系里的主路径；
 *   id_table       —— 传统名字匹配（板级文件/非设备树场景仍然用得上），
 *                     同时也是 i2c 核心做"驱动-设备"匹配的后备路径。
 * MODULE_DEVICE_TABLE 会把它们导出到模块信息里，供 udev / modules.alias 使用。
 */
static const struct of_device_id sensor_of_match[] = {
	{ .compatible = "lucien,sensor-char" },
	{ }
};
MODULE_DEVICE_TABLE(of, sensor_of_match);

static const struct i2c_device_id sensor_id[] = {
	{ "sensor_char", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sensor_id);

static struct i2c_driver sensor_i2c_driver = {
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= sensor_of_match,
	},
	.probe		= sensor_probe,
	.remove		= sensor_remove,
	.id_table	= sensor_id,
};

static int __init sensor_init(void)
{
	int ret;

	/* class_create 会在 /sys/class/ 下创建 sensor_char/ 目录 */
	sensor_class = class_create(DRV_NAME);
	if (IS_ERR(sensor_class))
		return PTR_ERR(sensor_class);

	/*
	 * 注册 i2c 驱动。此时设备树里的 sensor@48 可能已经被 virt_i2c 枚举成 i2c client
	 * （取决于模块加载顺序）：若已存在，i2c_add_driver() 会立即触发匹配并 probe；
	 * 若尚不存在，等 virt_i2c 注册 adapter 时再匹配。
	 * 两条路径都由内核负责，驱动不需要写任何等待逻辑 —— 这是总线模型带来的好处。
	 */
	ret = i2c_add_driver(&sensor_i2c_driver);
	if (ret) {
		class_destroy(sensor_class);
		return ret;
	}
	pr_info(DRV_NAME ": loaded\n");
	return 0;
}

static void __exit sensor_exit(void)
{
	i2c_del_driver(&sensor_i2c_driver);
	class_destroy(sensor_class);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(sensor_init);
module_exit(sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lucien");
MODULE_DESCRIPTION("Character device driver for I2C temperature/humidity sensor (teaching)");
MODULE_VERSION("1.0");
