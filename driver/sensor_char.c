// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_char.c - 字符设备驱动：I2C 温湿度传感器采集（教学/实验版）
 *
 * ============================ 架构总览 ============================
 *
 *   [硬件层]
 *     I2C 总线（实验环境：i2c-stub 虚拟总线 i2c-0；真机：i2c-1）
 *       └── 传感器从设备（实验环境 0x48，tmp105 寄存器语义；
 *                        真机替换为 SHT30 0x44 等，只改 dts + 解析函数）
 *
 *   [内核层]
 *     platform_driver  --设备树 compatible 匹配--> probe()
 *        ├── i2c_get_adapter() + i2c_new_client_device()  创建 i2c client
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
 * 设备树解耦要点：本文件不出现任何硬件地址/总线号字面量，全部来自 dts：
 *   compatible = "lucien,sensor-char";
 *   i2c-bus = <0>;            真机上可改用 i2c 控制器 phandle 引用
 *   sensor-addr = <0x48>;
 *   poll-interval-ms = <500>;
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
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
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
#define SENSOR_REG_TEMP	0x00	/* tmp105 温度寄存器（SHT30 为 0x0000 命令字） */

/* 阻塞 read() 等待新样本的最长时间：超时返回 -ETIMEDOUT，避免用户态永久卡死 */
#define SENSOR_READ_TIMEOUT_MS	2000

/* 传感器寄存器原始值 -> 毫摄氏度：LSB = 1/16 °C = 62.5 m°C */
#define RAW_TO_MILLI(raw)	((s32)(raw) * 1000 / 16)

struct sensor_dev {
	/* 字符设备 */
	struct cdev	cdev;
	dev_t		devt;
	struct device	*char_dev;

	/* I2C */
	struct i2c_adapter	*adapter;
	struct i2c_client	*client;
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
	s32 raw;
	s32 next;

	mutex_lock(&sd->lock);

	raw = i2c_smbus_read_word_data(sd->client, SENSOR_REG_TEMP);
	if (raw < 0) {
		sd->i2c_errors++;
		dev_warn_ratelimited(&sd->client->dev,
				     "I2C read failed: %d\n", raw);
		mutex_unlock(&sd->lock);
		return IRQ_HANDLED;
	}

	/*
	 * 模拟真实传感器数据变化：
	 * 实验环境的 i2c-stub 寄存器是可读写的，这里读出后小幅改动再写回，
	 * 让数据在 22.000 ~ 28.000 °C 之间缓慢变化。
	 * 真机请删除这段写回逻辑，直接使用 raw。
	 */
	next = RAW_TO_MILLI(raw);
	if (sd->latest.seq == 0)
		next = 25000;
	next += ((sd->latest.seq % 16) < 8) ? 100 : -100;	/* ±0.1 °C 锯齿 */
	if (next > 28000)
		next = 22000;
	i2c_smbus_write_word_data(sd->client, SENSOR_REG_TEMP,
				  (s16)(next * 16 / 1000));

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
		/*
		 * 在进程上下文模拟一次硬件中断。
		 * 关键点：真实中断发生CPU会自动关中断，而 write() 系统调用里
		 * 中断是打开的，直接调用 generic_handle_domain_irq() 会让中断
		 * 处理流程看到错误的状态，内核会报：
		 *   "irq 21 handler sensor_irq_hard enabled interrupts"
		 * 所以这里用 local_irq_disable/enable 模拟中断上下文。
		 * （hrtimer 回调里调用不需要，因为 softirq 上下文本来就关中断）
		 */
		local_irq_disable();
		generic_handle_domain_irq(sd->irq_domain, 0);
		local_irq_enable();
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

/* ============================ 设备树匹配 + probe ============================ */

static int sensor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct sensor_dev *sd;
	struct i2c_board_info info = {};
	u32 bus = 0, addr = 0x48, interval = 500;
	int ret;

	/* 1. 从设备树取参数——驱动里不写死任何硬件信息 */
	of_property_read_u32(np, "i2c-bus", &bus);
	of_property_read_u32(np, "sensor-addr", &addr);
	of_property_read_u32(np, "poll-interval-ms", &interval);
	dev_info(dev, "DT: i2c-bus=%u sensor-addr=0x%02x interval=%ums\n",
		 bus, addr, interval);

	sd = devm_kzalloc(dev, sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -ENOMEM;

	sd->sensor_addr = addr;
	sd->interval_ms = interval;
	mutex_init(&sd->lock);
	init_waitqueue_head(&sd->wq);
	platform_set_drvdata(pdev, sd);

	/* 2. 接上 I2C 子系统：拿 adapter，再在它上面创建从设备 client */
	sd->adapter = i2c_get_adapter(bus);
	if (!sd->adapter) {
		/*
		 * 总线还没就绪时返回 -EPROBE_DEFER，内核会在稍后自动重试 probe。
		 * 这是设备树驱动里处理依赖顺序的标准手段（尤其真机上 i2c 控制器较晚注册）。
		 */
		dev_warn(dev, "i2c bus %u not ready, defer probe\n", bus);
		return -EPROBE_DEFER;
	}

	strscpy(info.type, "sensor_demo", I2C_NAME_SIZE);
	info.addr = addr;
	sd->client = i2c_new_client_device(sd->adapter, &info);
	if (IS_ERR(sd->client)) {
		ret = PTR_ERR(sd->client);
		dev_err(dev, "failed to create i2c client: %d\n", ret);
		goto err_put_adapter;
	}
	dev_info(dev, "i2c client on bus %d addr 0x%02x\n",
		 sd->adapter->nr, addr);

	/* 3. 注册字符设备 -> /dev/sensor0 */
	ret = sensor_chrdev_register(sd);
	if (ret) {
		dev_err(dev, "chrdev register failed: %d\n", ret);
		goto err_unreg_client;
	}

	/* 4. 中断：实验环境自建虚拟中断源，真机换成传感器 ALERT 引脚对应的 IRQ */
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

	/* 5. 初始化传感器寄存器（写初值 25.0 °C），并启动采样定时器 */
	i2c_smbus_write_word_data(sd->client, SENSOR_REG_TEMP, 25 * 16);
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
err_unreg_client:
	i2c_unregister_device(sd->client);
err_put_adapter:
	i2c_put_adapter(sd->adapter);
	return ret;
}

/*
 * 注意：6.6 内核里 platform_driver.remove 的返回类型是 int（这是历史遗留，
 * 返回值会被驱动核心忽略）；更新的内核提供了 void 返回的 .remove_new()。
 */
static int sensor_remove(struct platform_device *pdev)
{
	struct sensor_dev *sd = platform_get_drvdata(pdev);

	hrtimer_cancel(&sd->timer);		/* 停掉采样源 */
	free_irq(sd->virq, sd);
	irq_domain_remove(sd->irq_domain);
	device_destroy(sensor_class, sd->devt);
	cdev_del(&sd->cdev);
	unregister_chrdev_region(sd->devt, 1);
	i2c_unregister_device(sd->client);
	i2c_put_adapter(sd->adapter);
	dev_info(&pdev->dev, "removed\n");
	return 0;
}

/* compatible 列表：内核用它与 dts 节点的 compatible 匹配（设备树解耦的关键） */
static const struct of_device_id sensor_of_match[] = {
	{ .compatible = "lucien,sensor-char" },
	{ }
};
MODULE_DEVICE_TABLE(of, sensor_of_match);

static struct platform_driver sensor_platform_driver = {
	.probe		= sensor_probe,
	.remove		= sensor_remove,
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= sensor_of_match,
	},
};

static int __init sensor_init(void)
{
	int ret;

	/* class_create 会在 /sys/class/ 下创建 sensor_char/ 目录 */
	sensor_class = class_create(DRV_NAME);
	if (IS_ERR(sensor_class))
		return PTR_ERR(sensor_class);

	ret = platform_driver_register(&sensor_platform_driver);
	if (ret) {
		class_destroy(sensor_class);
		return ret;
	}
	pr_info(DRV_NAME ": loaded\n");
	return 0;
}

static void __exit sensor_exit(void)
{
	platform_driver_unregister(&sensor_platform_driver);
	class_destroy(sensor_class);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(sensor_init);
module_exit(sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lucien");
MODULE_DESCRIPTION("Character device driver for I2C temperature/humidity sensor (teaching)");
MODULE_VERSION("1.0");
