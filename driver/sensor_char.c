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
 *   [数据通路 —— 阶段 03 起是"一个生产者、两个出口"]
 *     传感器寄存器 --I2C--> 线程化中断处理（唯一生产者）
 *        ├── kfifo 环形缓冲（64 个样本）--waitqueue--> read()/poll()/epoll/SIGIO
 *        └── mmap 共享页（seqlock 保护）----------> 用户态直接读（零拷贝）
 *     另外保留 latest（最新一个样本）供 ioctl(GET_SAMPLE) 立即取值。
 *
 *     为什么要有环形缓冲：采样由硬件节奏驱动（每 interval_ms 一个样本），
 *     而读进程的节奏由应用决定（可能几秒读一次，也可能一直不读）。
 *     没有缓冲时只能"用最新值覆盖旧值"，中间采样点会静默丢失；
 *     有了 kfifo，读进程可以事后补读，直到缓冲被填满为止。
 *
 *     【溢出策略：丢新不丢旧】kfifo 满时丢弃刚产生的新样本并累加 kfifo_dropped，
 *     而不是覆盖最旧的样本。理由：
 *       1) 已排队的数据是"用户态还欠着没读的历史"，静默改写历史比丢新更难排查；
 *       2) 计数（kfifo_dropped）让"丢了多少"变成可观测量，而不是无声无息；
 *       3) 传感器数据通常是时序敏感的，保留更早的时间序列比保留最新点更有价值。
 *     代价是最新样本可能进不了队列——但它仍然能在 latest/共享区里看到，
 *     所以"最新值"这条路径不受影响。
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
#include <linux/debugfs.h>		/* 运行统计/寄存器现场观测（阶段 04） */
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>		/* poll_wait / fasync_helper / kill_fasync */
#include <linux/jiffies.h>
#include <linux/kfifo.h>		/* 采样环形缓冲 */
#include <linux/mm.h>			/* remap_pfn_range / __get_free_pages / VM_* 标志 */
#include <linux/seqlock.h>		/* mmap 共享区的读写一致性（内核侧参考实现） */
#include <linux/delay.h>		/* fsleep：共享区写入窗口的测试放大器（睡眠延时） */
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
#include <linux/pm_runtime.h>		/* Runtime PM：空闲自动挂起（阶段 06） */

#include "sensor_ioctl.h"
#include "sensor_calc.h"		/* 纯逻辑计算：驱动与 KUnit 测试共用同一份实现 */

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

/* 环形缓冲容量（样本个数）：64 个样本足以覆盖"应用偶发几秒不读"的场景，
 * 同时在 QEMU 里跑溢出测试时只需短时间不读就能填满（10ms 周期 → 0.7 秒）。 */
#define SENSOR_RING_SAMPLES	64

/* 共享区布局（struct sensor_shm、SENSOR_SHM_MAGIC/VERSION/SAMPLES）
 * 定义在同一份 header 里，用户态测试程序按同样布局解析，避免两边漂移。 */

/* 共享区必须能放进一页：.mmap 只映射一页，放不下就意味着用户态会映射到不完整的数据 */
static_assert(sizeof(struct sensor_shm) <= PAGE_SIZE,
	      "sensor_shm 必须能放进一页（mmap 只映射一页）");

/*
 * 传感器寄存器原始值 -> 毫摄氏度：换算函数定义在 sensor_calc.h，
 * 由驱动与 KUnit 测试（sensor_kunit.ko）**共用同一份实现** ——
 * 否则测试测的是一份代码、驱动跑的是另一份，单元测试就失去意义。
 * 关键点：12 位补码必须先做符号扩展再乘，直接拿 u16 做算术会把
 * 负温度算成大正数（-0.0625 ℃ 变成 +255.9 ℃）。
 */

struct sensor_dev {
	/* 字符设备 */
	struct cdev	cdev;
	dev_t		devt;
	struct device	*char_dev;
	struct dentry	*dbg;		/* /sys/kernel/debug/sensor_char/ */

	/* I2C：从设备由 i2c 核心根据设备树枚举得到，驱动只持有 client */
	struct i2c_client	*client;
	struct regmap		*regmap;	/* 寄存器访问抽象层（后端 regmap_i2c） */
	u32			sensor_addr;

	/* 中断 */
	struct irq_domain	*irq_domain;	/* 自建虚拟中断域（真机不需要） */
	int			virq;
	struct hrtimer		timer;		/* 周期性触发虚拟中断 */

	/* 数据与同步 */
	struct mutex		lock;		/* 保护 latest/stats/共享区元数据与 I2C 访问 */
	wait_queue_head_t	wq;		/* read()/poll() 阻塞队列 */
	struct fasync_struct	*fasync;	/* 注册了 O_ASYNC 的进程链表（SIGIO 通知用） */
	struct sensor_sample	latest;		/* 最新一个样本（ioctl 立即取值用） */
	unsigned long		interval_ms;

	/* 采样环形缓冲：read() 的数据源（阶段 03） */
	struct kfifo		ring;
	spinlock_t		ring_lock;	/* 保护 kfifo 的读写，见 read() 里的说明 */
	u32			kfifo_dropped;	/* 队满而丢弃的新样本数 */

	/* mmap 共享区：一页，只读映射给用户态（阶段 03） */
	struct sensor_shm	*shm;
	unsigned long		shm_addr;	/* __get_free_pages 返回的地址（释放时要用） */
	/*
	 * 共享区写侧的串行化锁。
	 *
	 * 注意：这里用的是普通 spinlock_t，而不是 seqlock_t。
	 * 原因：seqlock_t 的序号计在它自己的 seqcount 里（结构体私有内存），
	 * 用户态 mmap 到的那一页根本看不到它，于是"内核用了 seqlock"
	 * 对用户态读者毫无意义——这是本阶段真实踩过的坑（初版就是这么写的，
	 * 用户态的重试逻辑成了死代码）。正确做法是在共享区里亲手维护
	 * 用户可见的 seq 字段，见 sensor_shm_publish()。
	 */
	/*
	 * 保护共享区写入（序列化多个写者）的锁。
	 * 用 mutex 而不是 spinlock：三个调用点都在可睡眠上下文——
	 *   (1) sensor_irq_thread()：线程化中断下半部（内核线程，内部本来就有 mutex_lock + 会睡眠的 I2C 读）
	 *   (2) sensor_ioctl(RESET)：进程上下文
	 *   (3) sensor_dbg_ring_read()：debugfs 读取（进程上下文）
	 * 换用 mutex 之后，写入窗口内的放大延时才可以安全地**睡眠**（见 sensor_shm_publish 注释），
	 * 这是把"放大器检查间歇失败"变成确定性可复现的关键。
	 *
	 * 注意：锁只序列化写者；用户态读者是无锁的，
	 * 所以 seq/数据之间的可见性仍必须靠下面显式的 smp_wmb() 保证，不能依赖锁。
	 */
	struct mutex		shm_lock;
	u32			shm_writes;	/* 共享区发布次数（debugfs/证据用） */
	unsigned int		shm_publish_delay_us;	/* 测试用：放大写入窗口，默认 0 */

	u32			open_count;
	u32			read_count;
	u32			irq_count;
	u32			i2c_errors;

	/*
	 * Runtime PM（阶段 06）。
	 * 用 volatile + READ_ONCE/WRITE_ONCE 读写：它会被定时器回调（softirq）、
	 * 线程化中断下半部、进程上下文（ioctl/sysfs/open）与 PM 工作队列共同访问。
	 */
	bool			suspended;	/* true = 已挂起：定时器与中断已停 */
};

/*
 * per-open 上下文说明（阶段 01 引入，阶段 03 退役）：
 *
 *   阶段 01 为了让 poll() 具备正确的电平语义，引入了 struct sensor_file，
 *   用"本 fd 已消费到哪个序号（last_seq）"判断可读性。
 *
 *   阶段 03 引入 kfifo 之后它不再需要了："还有没有新样本"这个问题现在由
 *   共享队列是否非空唯一回答 —— 消费就是出队，队列空就是没有数据。
 *   这个判定天然与 read() 一致（read 出队失败即 EAGAIN/阻塞），
 *   因此 poll 不会再出现"报告可读但 read 却阻塞"的不一致：
 *   如果还要保留 per-fd 的 last_seq，就会在多进程并发时出现这种矛盾
 *   （A 进程读走了样本，B 进程的 last_seq 没跟上 → B 的 poll 报可读、read 却拿不到）。
 *
 *   代价是语义变化：一个刚打开的 fd 只有在"缓冲里确实还有未读样本"时才立刻可读，
 *   而不是"设备曾经产生过样本就永远立即可读"。这是队列语义的正常结果，
 *   阶段 01 测试里对应的前置条件也据此调整（见 user/io_models_test.c）。
 */

static struct class *sensor_class;
static int sensor_major;			/* 0 = 动态分配主设备号 */
module_param(sensor_major, int, 0444);
MODULE_PARM_DESC(sensor_major, "固定主设备号，0 表示动态分配");

/*
 * 共享区写入窗口的测试放大器（微秒），默认 0 = 关闭。
 *
 * 为什么需要它：正常写入窗口只有 ~1µs，而采样周期是 10ms 量级，
 * 读者撞上"写入中"的概率约 10⁻⁴ —— 意味着“seqlock 是否真的生效”
 * 在自然条件下几乎观测不到（正向证据拿不到，检查项也就无法被验证）。
 * 打开后写入窗口被拉长到数百微秒：
 *   - 正确实现：读者重试次数 > 0 且数据始终一致（协议生效）；
 *   - 破坏实现（不写 seq / 少写屏障）：读者会拿到撕裂数据（检查项报错）。
 * 只在测试脚本里通过 insmod 参数打开，正式运行保持 0。
 */
static unsigned int shm_publish_delay_us;
module_param(shm_publish_delay_us, uint, 0444);
MODULE_PARM_DESC(shm_publish_delay_us,
		 "调试用：在共享区写入窗口内插入睡眠延时(µs)，放大可观测窗口，默认 0");

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
 * 把样本投递到 mmap 共享区 —— 用户态可见的 seqlock 协议。
 *
 * ============================ 协议 ============================
 *   seq 为奇数 = 正在写入；seq 为偶数 = 共享区内容一致，可放心读取。
 *
 *   写方（本函数）：           读方（用户态 shm_snapshot）：
 *     seq++（→ 奇数）            s1 = seq
 *     smp_wmb()                  （s1 为奇数 → 重试）
 *     写 header 与 samples[]     读 header 与 samples[]
 *     [可选 fsleep 放大器]        s2 = seq
 *     smp_wmb()                  若 s1 != s2 → 重试
 *     seq++（→ 偶数）            否则本次数据一致
 *
 * 两侧的写屏障顺序与内核 raw_write_seqcount_begin/end()（include/linux/seqlock.h）
 * 完全一致：begin 是"先置奇、再屏障"，end 是"先屏障、再置偶"。
 * 少任何一个屏障，读者都可能拿到"seq 检查通过、但数据其实是旧的/写了一半"的结果——
 * 这种错误不崩溃、不报错，只会静默给出错位数据，只能靠屏障从根上避免。
 *
 * ============================ 为什么不能只用 seqlock_t ============================
 *   seqlock_t 的序号在 sd->shm_lock 的私有内存里，用户态 mmap 看不到；
 *   只在内核侧用 write_seqlock() 只能保证"内核写者之间互斥"，
 *   对用户态读者没有任何可观测的一致性信号。所以必须在共享区里
 *   亲手维护 seq，并且只有这个 seq 才是与用户态的契约。
 *
 * ============================ 锁与上下文 ============================
 *   mutex：三个写入/读取点都在可睡眠上下文（线程化中断下半部、ioctl 进程上下文、
 *   debugfs 读取），而且写入窗口内可能插入睡眠延时（测试放大器），所以用 mutex 而非 spinlock。
 *   它只序列化写者，防止两个写者的奇数窗口互相错乱。
 *
 *   常见误解："中断里的代码不能睡眠"只适用于硬中断上半部；
 *   request_threaded_irq 的下半部是普通内核线程，可以睡眠
 *   （本文件在它里面本来就要 mutex_lock + 做会睡眠的 I2C 读）。
 *
 * ============================ 测试放大器 ============================
 *   shm_publish_delay_us（模块参数，默认 0）在奇数窗口内插入 **睡眠** 延时（fsleep），
 *   把"写入中"的窗口从 ~1µs 放大到毫秒量级。
 *   为什么必须睡眠而不能自旋：自旋会让写者占满一个 CPU，读者若被调度到同一 CPU
 *   就在整个窗口内得不到执行机会，永远观测不到奇数值（实测同一代码连跑 3 次有 2 次失败，
 *   并触发 sched: RT throttling）；睡眠延时让写者主动让出 CPU，观测变得确定。
 *   用途：正常写窗口只占采样周期的 10⁻⁴ 量级，读者几乎不可能自然撞上，
 *   于是"seqlock 到底管不管用"无法被观测。放大后：
 *     - 好实现：读者重试次数 > 0，且 invalid 始终为 0（证明协议真的生效）；
 *     - 坏实现：读者会读到撕裂数据，invalid > 0（证明检查项有判别力）。
 *   这是"受控放大器"验证法，只在测试时由测试脚本 insmod 参数打开。
 */
static void sensor_shm_publish(struct sensor_dev *sd, const struct sensor_sample *s)
{
	struct sensor_shm *shm = sd->shm;
	u32 seq;

	if (!shm)
		return;

	mutex_lock(&sd->shm_lock);

	seq = READ_ONCE(shm->seq) + 1;
	WRITE_ONCE(shm->seq, seq);	/* → 奇数：写入中 */
	smp_wmb();			/* 与 raw_write_seqcount_begin() 一致 */

	/*
	 * 注意：先置奇再写数据。若反过来（先写数据再置奇），
	 * 读方可能恰好落在"数据已变、seq 仍是偶"的窗口里，直接采信错误数据。
	 *
	 * 放大窗口必须用**睡眠**延时（fsleep），不能用 udelay 自旋：
	 *   本函数运行在线程化中断下半部（内核线程），上下文允许睡眠；
	 *   而自旋会让写者在整个窗口内占满一个 CPU——若读者恰好被调度到同一个 CPU，
	 *   它在这段时间根本得不到执行机会，于是"永远观测不到奇数值"。
	 *   实测症状：同一代码连跑 3 次，2 次 odd_seen=0 / retries=0（检查项间歇失败），
	 *   同时写者还因为长时间自旋触发了 sched: RT throttling。
	 *   改成睡眠延时后，窗口内写者主动让出 CPU，检查项变得确定可复现。
	 */
	if (sd->shm_publish_delay_us)
		fsleep(sd->shm_publish_delay_us);

	shm->samples[shm->write_idx] = *s;
	/* 回绕推进共用 sensor_calc.h 的实现（回绕边界有 KUnit 覆盖） */
	shm->write_idx = sensor_fifo_next(shm->write_idx, SENSOR_SHM_SAMPLES);
	if (shm->count < SENSOR_SHM_SAMPLES)
		shm->count++;

	smp_wmb();			/* 与 raw_write_seqcount_end() 一致 */
	WRITE_ONCE(shm->seq, seq + 1);	/* → 偶数：一致 */

	sd->shm_writes++;

	mutex_unlock(&sd->shm_lock);
}

/*
 * 下半部（内核线程上下文）：这里才允许睡眠，做 I2C 读。
 * 真机上同样结构：threaded IRQ 里读传感器寄存器。
 */
static irqreturn_t sensor_irq_thread(int irq, void *data)
{
	struct sensor_dev *sd = data;
	struct sensor_sample sample;
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
	next = sensor_raw_to_milli(raw);

	/*
	 * 注意：sd->lock 在本函数入口已经持有，从 regmap_read 到样本入队、
	 * 共享区发布为止都在同一个临界区里，这里不要再加锁
	 * （mutex 不可重入，重复加锁会直接死锁 —— 本阶段真实踩过这个坑）。
	 */
	sample.seq = sd->latest.seq + 1;
	sample.temp_milli = next;
	sample.irq_count = sd->irq_count;
	sample.ts_ns = ktime_get_ns();
	sd->latest = sample;

	/*
	 * 出口 1：环形缓冲（read/poll/epoll/SIGIO 的数据源）。
	 *
	 * 用 kfifo_in_spinlocked 而不是 kfifo_put：kfifo 的免锁性质只对
	 * "单生产者 + 单消费者"成立；本设备允许多个进程各自 open 后同时 read()，
	 * 多读者必须串行化，否则 in/out 索引会互相覆盖。
	 *   返回值 = 实际写入字节数；不满一条样本（即队列已满）= 0，
	 *   此时丢弃新样本并计数（丢弃策略见文件头说明）。
	 */
	if (kfifo_in_spinlocked(&sd->ring, &sample, sizeof(sample),
				&sd->ring_lock) != sizeof(sample))
		sd->kfifo_dropped++;

	/* 出口 2：mmap 共享区（零拷贝通道） */
	sensor_shm_publish(sd, &sample);

	mutex_unlock(&sd->lock);

	/*
	 * 唤醒阻塞在 read() 里的进程。
	 * 必须放在"样本已入队"之后：wait_event 的条件是"队列非空"，
	 * 先改状态再唤醒才能避免丢唤醒（wake_up 之后才入队会出现
	 * "被唤醒但发现队列为空 → 又睡回去"的假唤醒循环）。
	 * 唤醒本身放在锁外，避免"唤醒后马上又抢不到锁"的惊群效应。
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
 * 数据可用性判定：read()、poll() 共用同一个判定，避免两条路径的语义漂移。
 *
 * 判定依据是"环形缓冲里还有没有未消费的样本"——这是唯一与 read() 完全一致的
 * 事实：read() 取不到数据就说明没有新样本。因此 poll() 不会出现
 * "报告可读但 read 却阻塞"的矛盾。
 * （阶段 01 曾用 per-fd 的 last_seq 判断，但那在"多进程共享同一队列"时会打架：
 *   A 读走样本后 B 的 last_seq 不会跟着变，B 的 poll 就会谎报可读。）
 *
 * 这里不加锁：kfifo 的 in/out 索引是整字读取，kfifo 内部用 smp_wmb() 保证
 * "数据先于索引对读者可见"，所以免锁读不会读到未写入的数据。这也是
 * wait_event 的条件表达式必须"无副作用、可重复求值"的直接体现。
 */
static bool sensor_has_new_sample(struct sensor_dev *sd)
{
	/*
	 * 无锁读 kfifo 索引是**有意为之的良性数据竞争**：
	 *   - in/out 是整字量，读到的值可能瞬时不一致（写方正在改）；
	 *   - 但这里的用途只是"先生成等待条件，再去睡"，醒来后 wait_event 会重新求值，
	 *     瞬时不一致最多导致多睡/少睡一次，不会造成数据错误。
	 * 所以不需要加锁；但注释必须说清楚这是良性的，而不是无意漏锁
	 * （KCSAN 会对这类访问报 data race，属于预期噪声，见实现文档"并发证据等级"一节）。
	 */
	return !kfifo_is_empty(&sd->ring);
}

/* 环形缓冲的实际容量（样本数）。
 * 注意：kfifo_alloc 会把字节容量向上取整到 2 的幂，所以实际能放的样本数
 * 往往比请求的多（例如请求 64×24=1536 字节 → 实际 2048 字节 → 85 个样本）。
 * 这个换算必须暴露出来，否则用户态拿到的是"字节数"而不是"样本数"。 */
static unsigned int sensor_ring_capacity(struct sensor_dev *sd)
{
	return kfifo_size(&sd->ring) / sizeof(struct sensor_sample);
}

/* 当前已缓冲但未被读走的样本数 */
static unsigned int sensor_ring_count(struct sensor_dev *sd)
{
	/* 同 sensor_has_new_sample()：无锁读索引是良性的，仅供统计展示 */
	return kfifo_len(&sd->ring) / sizeof(struct sensor_sample);
}

/* 前向声明：release() 里要摘除异步通知，而 sensor_fasync 定义在后面 */
static int sensor_fasync(int fd, struct file *file, int on);

/*
 * open()：只记录私有数据、累计打开次数，不再分配任何 per-open 状态。
 * （阶段 01 的 struct sensor_file 在阶段 03 退役：可读性由共享队列决定。）
 *
 * 阶段 06 新增：占用一个 runtime PM 引用。
 *   只要还有进程打开着设备，就不允许自动挂起 —— 否则正在 read() 的进程会因为
 *   采样被停掉而卡在超时上（行为上看起来像"驱动坏了"）。
 *   用 pm_runtime_resume_and_get() 而不是 pm_runtime_get_sync()：前者在出错时
 *   会把引用退还（get_sync 会留下一个引用，导致设备再也挂不下去，是经典泄漏）。
 */
static int sensor_open(struct inode *inode, struct file *file)
{
	struct sensor_dev *sd = container_of(inode->i_cdev, struct sensor_dev, cdev);
	struct device *dev = &sd->client->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		dev_err(dev, "runtime resume failed: %d\n", ret);
		return ret;
	}

	file->private_data = sd;

	mutex_lock(&sd->lock);
	sd->open_count++;
	mutex_unlock(&sd->lock);
	dev_info(dev, "opened (count=%u)\n", sd->open_count);
	return 0;
}

static int sensor_release(struct inode *inode, struct file *file)
{
	struct sensor_dev *sd = file->private_data;
	struct device *dev = &sd->client->dev;

	/*
	 * 摘除异步通知：fasync_helper(..., 0, ...) 会遍历 fasync 链表把本 file 摘掉。
	 * 显式调用（而不是依赖 close 的隐式清理）保证"注册-摘除"严格配对。
	 */
	sensor_fasync(-1, file, 0);

	file->private_data = NULL;

	/*
	 * 归还 runtime PM 引用（与 open 里的 get 严格配对）：
	 *   mark_last_busy()  记录"最后一次使用时刻"，autosuspend 的计时从它开始；
	 *                     （少了这一步，核心会认为设备从上次忙到现在已经超时，可能立即挂起）
	 *   put_autosuspend() 引用降到 0 后按 autosuspend 延迟（1 秒）安排挂起。
	 * 这一秒的缓冲是刻意的：用户态典型的"读完就关、过一会再开"不会引发反复的
	 * suspend/resume（每次 resume 都要重新拉 I2C 总线，代价不小）。
	 */
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "closed\n");
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
/*
 * read(): 从环形缓冲取一个样本，格式化后返回一行文本。
 *   cat /dev/sensor0 可以持续看到数据流；行为与传感器驱动常见的字符接口一致。
 *
 * 四种 IO 模型里，这里承担"阻塞"与"非阻塞"两种：
 *   阻塞   ：队列空时睡在 sd->wq 上，被生产者（线程化中断下半部）唤醒；
 *            超时（SENSOR_READ_TIMEOUT_MS）返回 -ETIMEDOUT
 *   非阻塞 ：O_NONBLOCK 下队列空立刻返回 -EAGAIN
 *
 * 为什么用 for(;;) 重试而不是"判断一次就取"：
 *   多个进程共享同一个队列，被唤醒后可能已被别的进程抢先取走（惊群）。
 *   取不到就回到等待（或返回 EAGAIN），语义始终自洽。
 *
 * 为什么 kfifo 访问要加锁：
 *   kfifo 的免锁性质只覆盖"单读者 + 单写者"；本设备允许多进程并发 open/read，
 *   多读者必须串行化，否则 out 索引会互相覆盖、导致数据错乱。
 *   （生产者侧同样用 ring_lock，所以"多读者 + 单写者"整体是安全的。）
 */
static ssize_t sensor_read(struct file *file, char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct sensor_dev *sd = file->private_data;
	struct sensor_sample snap;
	char kbuf[96];
	int len, ret;

	for (;;) {
		/* 出队：返回值 = 实际读出的字节数，0 表示队列为空 */
		if (kfifo_out_spinlocked(&sd->ring, &snap, sizeof(snap),
					 &sd->ring_lock) == sizeof(snap))
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/*
		 * 等待新样本。wait_event_* 是"先登记等待项、再求值条件"的循环，
		 * 生产者是"先入队、再 wake_up"，两者配合不会丢唤醒。
		 * 超时返回 0（不是错误码），<0 表示被信号打断。
		 */
		ret = wait_event_interruptible_timeout(
			sd->wq, sensor_has_new_sample(sd),
			msecs_to_jiffies(SENSOR_READ_TIMEOUT_MS));
		if (ret == 0)
			return -ETIMEDOUT;
		if (ret < 0)
			return -ERESTARTSYS;	/* 收到信号 */
	}

	mutex_lock(&sd->lock);
	sd->read_count++;
	mutex_unlock(&sd->lock);

	len = format_sample(kbuf, sizeof(kbuf), &snap);
	if (count < len)
		len = count;

	if (copy_to_user(ubuf, kbuf, len)) {
		/*
		 * 拷贝失败时把样本放回队列尾部，而不是默默丢掉：
		 * kfifo_out 已经把样本取走了，若直接返回 -EFAULT，这个样本就
		 * 凭空消失且不计入 kfifo_dropped —— 统计与实际不符（"样本无声蒸发"），
		 * 而 ring_count + kfifo_dropped == irq_count 这个守恒不变量正好能抓住它。
		 * 放回尾部会让顺序略有变化（该样本变成最新），但比丢掉更容易用。
		 */
		kfifo_in_spinlocked(&sd->ring, &snap, sizeof(snap), &sd->ring_lock);
		return -EFAULT;
	}

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
/*
 * poll(): 支持 select/poll/epoll 多路复用。
 *
 * 两件事，顺序不能反：
 *   1. poll_wait()：把本文件的等待项挂到 sd->wq 上（只登记，不睡眠；真正睡眠
 *      由调用方 poll/epoll_wait 系统调用负责）。驱动不需要持有锁，也不该持锁。
 *   2. 返回当前的可用性掩码：队列非空就 EPOLLIN，否则 0。
 *
 * 这里只实现"电平触发"语义（驱动唯一应该做的）：每次调用都基于当前状态重新
 * 回答，样本被读走（队列变空）后自然不再上报 EPOLLIN。边沿触发由 epoll 在
 * 用户态接口层面实现，驱动不需要也不应该感知。
 */
static __poll_t sensor_poll(struct file *file, poll_table *wait)
{
	struct sensor_dev *sd = file->private_data;

	poll_wait(file, &sd->wq, wait);

	if (sensor_has_new_sample(sd))
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
	struct sensor_dev *sd = file->private_data;

	return fasync_helper(fd, file, on, &sd->fasync);
}

/*
 * mmap(): 把内核里的共享页映射到用户态，实现"零拷贝"读。
 *
 * 与 read() 的本质区别：
 *   read()  ：内核缓冲 --copy_to_user--> 用户缓冲（每次读一次拷贝 + 一次系统调用）
 *   mmap()  ：用户页表直接指向内核分配的物理页（不拷贝，之后取值完全在用户态）
 * 适合"高频取最新数据"的场景；代价是接口变成"自己解析共享内存 + 自己处理并发",
 * 因此共享区用 seqlock 约定（见 struct sensor_shm 的说明）。
 *
 * 两个必须做的校验：
 *   - 映射长度不能超过一页：我们只分配了一页，多映射会读到无关的内存；
 *   - 必须是只读映射：共享区由内核单方写入，用户态写会破坏 seqlock 的一致性假设。
 */
static int sensor_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sensor_dev *sd = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > PAGE_SIZE)
		return -EINVAL;

	/*
	 * 偏移必须为 0：共享区只有一页，remap_pfn_range 直接用 virt_to_pfn(sd->shm)，
	 * 完全不看 vma->vm_pgoff。若不检查，mmap(..., 4096) 会"成功"但映射的仍是同一页，
	 * 调用者会以为自己拿到了第二页数据 —— 宁可显式拒绝这种无意义的偏移。
	 */
	if (vma->vm_pgoff)
		return -EINVAL;

	if (vma->vm_flags & VM_WRITE)
		return -EPERM;

	/*
	 * 页保护显式设为只读。
	 * 为什么不直接沿用 vma->vm_page_prot：它来自用户请求的 PROT_*，
	 * 让内核侧的只读语义依赖调用方参数是脆弱的；显式覆盖后语义由驱动保证。
	 */
	vma->vm_page_prot = PAGE_READONLY;
	/*
	 * 映射范围不可扩展、coredump 时不导出内容。
	 * 注意 Linux 6.3 起 vma->vm_flags 是只读成员（union 里的 const vm_flags_t），
	 * 必须用 vm_flags_set() 这类 helper 修改；直接 `|=` 会编译报错。
	 */
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	/*
	 * remap_pfn_range 把"物理页帧号"直接填进用户页表。
	 * 共享区来自 __get_free_pages（线性映射区），virt_to_pfn 可直接换算；
	 * 若改用 vmalloc 分配，就必须用 vmalloc_to_pfn 逐页换算，
	 * 或者改用 vm_ops->fault + vm_insert_page（见实现文档的取舍分析）。
	 */
	return remap_pfn_range(vma, vma->vm_start, virt_to_pfn(sd->shm),
			       size, vma->vm_page_prot);
}

/* write(): 演示写路径——写入 0x01 触发一次"立即采样" */
static ssize_t sensor_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct sensor_dev *sd = file->private_data;
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

/*
 * ============================ 采样周期：范围判定与生效动作 ============================
 *
 * 为什么要把这两件事抽成共用函数：
 *   "周期"现在有两个入口——ioctl(SENSOR_IOC_SET_INTERVAL) 与 sysfs 的 interval_ms。
 *   如果两处各写一份判定，它们迟早会漂移（本项目阶段 04 之前的 ioctl 下限是 10ms，
 *   而 sysfs 契约要求 1ms）。用户态就会遇到"同一个设置，走 A 接口成功、走 B 接口失败"
 *   这种最难查的不一致。因此统一为：范围判定一份（在 sensor_calc.h 里，
 *   可被 KUnit 直接覆盖边界）、生效动作一份，两个入口都调它们。
 */

/* 让新周期立即生效：取消后按新周期重启定时器（hrtimer_cancel 会等回调结束） */
static void sensor_apply_interval(struct sensor_dev *sd, unsigned long ms)
{
	struct device *dev = &sd->client->dev;

	/*
	 * 取一个"不唤醒设备"的引用，防止本函数执行期间设备被自动挂起：
	 * 否则在 READ_ONCE(suspended) 与 hrtimer_start() 之间存在窗口，
	 * autosuspend 若在此刻完成，就会留下"状态 suspended + 定时器仍在触发"
	 * 的假省电（阶段 06 验证者发现的竞态，31 项检查抓不到）。
	 * 用 get_noresume 而不是 get_sync：本函数在"已挂起"时应当只记账，不能把设备唤醒。
	 */
	pm_runtime_get_noresume(dev);

	mutex_lock(&sd->lock);
	sd->interval_ms = ms;
	mutex_unlock(&sd->lock);

	/*
	 * 设备已被 runtime PM 挂起时不要去启动定时器：那会让"已挂起"的设备偷偷采样，
	 * runtime_status 与实际行为不一致（阶段 06 的检查项就是靠这个一致性来判断
	 * "挂起是否真的省电"，不一致会让结论变成假的）。
	 * 此时只记下新周期，等 resume 时按新值启动。
	 */
	if (READ_ONCE(sd->suspended)) {
		dev_info(dev, "interval -> %lu ms (deferred: device runtime-suspended)\n", ms);
		pm_runtime_put_autosuspend(dev);
		return;
	}

	hrtimer_cancel(&sd->timer);
	hrtimer_start(&sd->timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
	dev_info(dev, "interval -> %lu ms\n", ms);
	pm_runtime_put_autosuspend(dev);
}


static long sensor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sensor_dev *sd = file->private_data;
	struct sensor_sample snap;
	struct sensor_stats stats;
	u32 interval;
	unsigned long flags;
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
		/*
		 * 范围判定与 sysfs 的 interval_ms 共用同一个函数，
		 * 两个入口不会再有“一边能写、一边报 EINVAL”的不一致。
		 */
		if (!sensor_interval_valid(interval))
			return -EINVAL;
		sensor_apply_interval(sd, interval);
		break;

	case SENSOR_IOC_GET_STATS:
		mutex_lock(&sd->lock);
		stats.open_count = sd->open_count;
		stats.read_count = sd->read_count;
		stats.irq_count = sd->irq_count;
		stats.i2c_errors = sd->i2c_errors;
		stats.interval_ms = sd->interval_ms;
		stats.kfifo_dropped = sd->kfifo_dropped;
		stats.ring_count = sensor_ring_count(sd);
		stats.ring_capacity = sensor_ring_capacity(sd);
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
		sd->kfifo_dropped = 0;
		/*
		 * 一并清空环形缓冲：RESET 的语义是"回到刚 probe 完的状态"，
		 * 留着几十个旧样本会让"重置"名不副实。
		 * kfifo_reset 要求独占访问（它会同时改写 in/out），所以用 ring_lock 保护。
		 */
		spin_lock_irqsave(&sd->ring_lock, flags);
		kfifo_reset(&sd->ring);
		spin_unlock_irqrestore(&sd->ring_lock, flags);
		/*
		 * 共享区也必须一起清空，否则会出现"seq 回退"：
		 * RESET 把 latest.seq 归零后，新样本的 seq 从 1 重新开始，
		 * 但共享区里还留着旧样本（seq 是几百），于是读者看到
		 * 同一个窗口内 seq 先大后小 —— 这是真实踩到的 bug，
		 * 被 user/ring_mmap_test.c / concurrency_test.c 的单调性校验抓住。
		 */
		if (sd->shm) {
			/*
			 * 清空共享区同样要走 seqlock 协议：这是一次"写入"，
			 * 否则读者可能在 count/write_idx 刚清零、samples 还没被覆盖时
			 * 采信一份自相矛盾的快照。seq 保持偶数是协议的终态要求。
			 */
			u32 seq;

			mutex_lock(&sd->shm_lock);
			seq = READ_ONCE(sd->shm->seq) + 1;
			WRITE_ONCE(sd->shm->seq, seq);
			smp_wmb();
			sd->shm->count = 0;
			sd->shm->write_idx = 0;
			smp_wmb();
			WRITE_ONCE(sd->shm->seq, seq + 1);
			mutex_unlock(&sd->shm_lock);
		}
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
	.mmap		= sensor_mmap,	/* 零拷贝共享页 */
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

/* ============================ sysfs：设备参数 ============================ */
/*
 * 为什么放 sysfs、为什么挂在这里：
 *   sysfs 的约定是「一个文件一个值（one value per file）」，而且它是**稳定 ABI**：
 *   一旦发布，语义就不能随版本改动㈠以这里只放「设备参数/状态」这类稳定的单值属性，
 *   统计快照那种「一大串字段」的东西放 debugfs（见下一节）。
 *
 *   属性挂在字符设备类设备上（sd->char_dev，即 /dev/sensor0 对应的
 *   /sys/class/sensor_char/sensor0/），而不是 i2c client 设备上：
 *   用户面对的是「这个传感器字符设备」，路径与 /dev/sensor0 一一对应，最直观。
 *
 *   dev_get_drvdata(dev) 能拿到 sd：device_create() 的第 4 个参数就是 drvdata。
 */

static ssize_t interval_ms_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct sensor_dev *sd = dev_get_drvdata(dev);
	unsigned long ms;

	mutex_lock(&sd->lock);
	ms = sd->interval_ms;
	mutex_unlock(&sd->lock);

	return sysfs_emit(buf, "%lu\n", ms);
}

static ssize_t interval_ms_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sensor_dev *sd = dev_get_drvdata(dev);
	unsigned long ms;
	int ret;

	/*
	 * 用 kstrtoul 而不是 simple_strtoul：前者会报错、能拒绍非法输入
	 * （负数、字母、尾随垃圾），后者默默返回 0，会把「解析失败」当成「用户写了0」。
	 */
	ret = kstrtoul(buf, 0, &ms);
	if (ret)
		return ret;
	if (!sensor_interval_valid(ms))
		return -EINVAL;

	sensor_apply_interval(sd, ms);
	return count;
}
static DEVICE_ATTR_RW(interval_ms);

/* 当前最新样本序号（只读），与 ioctl(GET_STATS) 看到的是同一份数据 */
static ssize_t seq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sensor_dev *sd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(sd->latest.seq));
}
static DEVICE_ATTR_RO(seq);

/* 累计 I2C 传输失败次数：故障注入后看它是否增长，是验证错误处理路径的入口 */
static ssize_t i2c_errors_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct sensor_dev *sd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(sd->i2c_errors));
}
static DEVICE_ATTR_RO(i2c_errors);

/*
 * 环形缓冲容量（样本数）。为什么要专门暴露：kfifo_alloc 会把字节容量
 * 向上取整到 2 的幂，所以「请求 64 个」实际得到的是 85 个（1536B → 2048B）。
 * 用户态要计算满没满、该读多少，必须知道真实容量而不是请求值。
 */
static ssize_t ring_capacity_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct sensor_dev *sd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", sensor_ring_capacity(sd));
}
static DEVICE_ATTR_RO(ring_capacity);

static struct attribute *sensor_attrs[] = {
	&dev_attr_interval_ms.attr,
	&dev_attr_seq.attr,
	&dev_attr_i2c_errors.attr,
	&dev_attr_ring_capacity.attr,
	NULL,
};

static const struct attribute_group sensor_attr_group = {
	.attrs = sensor_attrs,
};

/* ============================ debugfs：运行统计与现场观测 ============================ */
/*
 * debugfs 与 sysfs 的分工（面试常问）：
 *   sysfs   = 稳定 ABI，只放设备参数/状态单值属性；
 *   debugfs = 内核明确声明「不是稳定 ABI、不保证向后兼容」，适合放统计、
 *             寄存器快照、内部状态转储。把这些塞进 sysfs 会把 sysfs 变成 dump 接口
 *             （违反 one-value-per-file），而且一旦发布就得永久兼容。
 *
 * 三个文件都是「每次 read 现场生成文本」（不缓存），所以读到的永远是当前值。
 * 内容都很短（远小于一页），所以直接用 simple_read_from_buffer 拷贝即可，
 * 不需要上 seq_file（那种场景是内容超过 4KB、需要分页输出）。
 */

static ssize_t sensor_dbg_stats_read(struct file *file, char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	struct sensor_dev *sd = file->private_data;
	char kbuf[320];
	int len;

	mutex_lock(&sd->lock);
	len = scnprintf(kbuf, sizeof(kbuf),
			"open=%u read=%u irq=%u i2c_err=%u interval=%lu dropped=%u ring_count=%u ring_capacity=%u seq=%u shm_writes=%u\n",
			sd->open_count, sd->read_count, sd->irq_count, sd->i2c_errors,
			sd->interval_ms, sd->kfifo_dropped, sensor_ring_count(sd),
			sensor_ring_capacity(sd), READ_ONCE(sd->latest.seq),
			sd->shm_writes);
	mutex_unlock(&sd->lock);

	return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
}

/*
 * ring：环形缓冲状态 + 最近 8 条样本。
 * 样本从 mmap 共享区里取（而不是去动 kfifo 的读写索引）——读共享区不会
 * 干扰 read() 的消费进度。取的时候短暂持有 shm_lock、拷到本地数组，
 * 之后在锁外格式化，避免在持锁期间做 scnprintf/拷贝到用户空间。
 */
static ssize_t sensor_dbg_ring_read(struct file *file, char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct sensor_dev *sd = file->private_data;
	struct sensor_sample snap[8];
	unsigned int n = 0, total = 0, i, start, widx;
	char kbuf[640];
	int len = 0;

	mutex_lock(&sd->shm_lock);
	total = sd->shm->count;
	widx = sd->shm->write_idx;
	n = total > ARRAY_SIZE(snap) ? ARRAY_SIZE(snap) : total;
	start = (widx + SENSOR_SHM_SAMPLES - n) % SENSOR_SHM_SAMPLES;
	for (i = 0; i < n; i++)
		snap[i] = sd->shm->samples[(start + i) % SENSOR_SHM_SAMPLES];
	mutex_unlock(&sd->shm_lock);

	len += scnprintf(kbuf + len, sizeof(kbuf) - len,
			 "count=%u dropped=%u ring_count=%u ring_capacity=%u shm_writes=%u recent=%u\n",
			 total, sd->kfifo_dropped, sensor_ring_count(sd),
			 sensor_ring_capacity(sd), sd->shm_writes, n);
	for (i = 0; i < n; i++)
		len += scnprintf(kbuf + len, sizeof(kbuf) - len,
				 "sample[%u] seq=%u temp_milli=%d\n",
				 i, snap[i].seq, snap[i].temp_milli);

	return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
}

/*
 * regs：用 regmap 现场读一次寄存器 0x00~0x02。
 * 意义不只是「看一眼寄存器」：它跑的是与中断线程采集样本**完全相同**的
 * 寄存器访问路径（regmap → i2c core → 总线 smbus_xfer），
 * 所以它是「regmap 通路是活的」最直接的证据。
 * 读失败要如实展示错误码，不能显示一个假值（否则看的人会以为总线正常）。
 */
static ssize_t sensor_dbg_regs_read(struct file *file, char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct sensor_dev *sd = file->private_data;
	struct device *dev = &sd->client->dev;
	char kbuf[256];
	unsigned int reg, val;
	int len = 0, ret;

	/*
	 * 真机上对设备寄存器发起 I2C 事务前必须先让设备进入活跃状态：
	 * 挂起时很多芯片的总线接口是关的（读不到、或读到无意义值），
	 * 所以这里取一次 runtime PM 引用，读完再归还（与 open/read 的用法一致）。
	 * 这也解释了为什么调试接口也要遵守电源管理——否则"看现场"这个动作
	 * 本身就能破坏现场。
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		len = scnprintf(kbuf, sizeof(kbuf), "<runtime resume failed %d>\n", ret);
		return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
	}

	for (reg = SENSOR_REG_TEMP; reg <= SENSOR_REG_CONFIG; reg++) {
		ret = regmap_read(sd->regmap, reg, &val);
		if (ret)
			len += scnprintf(kbuf + len, sizeof(kbuf) - len,
					 "%02x: <read error %d>\n", reg, ret);
		else
			len += scnprintf(kbuf + len, sizeof(kbuf) - len,
					 "%02x: %04x\n", reg, val);
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
}

static const struct file_operations sensor_dbg_stats_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= sensor_dbg_stats_read,
	.llseek	= default_llseek,
};

static const struct file_operations sensor_dbg_ring_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= sensor_dbg_ring_read,
	.llseek	= default_llseek,
};

static const struct file_operations sensor_dbg_regs_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= sensor_dbg_regs_read,
	.llseek	= default_llseek,
};

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

/* ============================ Runtime PM（阶段 06）============================
 *
 * 要解决的问题：这个驱动在 probe 里启动了一个周期性 hrtimer，它会不停地
 * 触发中断 -> 线程化下半部 -> 发起 I2C 事务。哪怕没有任何进程打开设备，
 * 这套动作也一直在跑 —— 在电池设备上就是纯浪费（真实传感器上还意味着
 * ADC 一直在转换）。
 *
 * Runtime PM 的模型是"引用计数 + 回调"：
 *   * 使用者（本驱动里是 open()）取引用；不使用时归还；
 *   * 引用降到 0 后，核心在 autosuspend 延迟（1 秒）之后回调 runtime_suspend；
 *   * 再次有人取引用时，核心回调 runtime_resume。
 *
 * 与系统级 suspend（suspend-to-RAM）的区别：runtime PM 对单个设备生效，
 * 与系统是否睡眠无关，可在运行时反复进出；两者的回调函数集也不同
 * （SET_RUNTIME_PM_OPS vs SET_SYSTEM_SLEEP_PM_OPS）。
 * ===========================================================================*/

/* 1 秒的 autosuspend 延迟：在"及时省电"与"避免频繁开关"之间取折中 */
#define SENSOR_AUTOSUSPEND_DELAY_MS	1000

/*
 * 注意从这里开始都可以睡眠（PM 回调运行在进程上下文），
 * 因此可以用 hrtimer_cancel / disable_irq（后者会等正在执行的 handler 结束）。
 */
static int sensor_runtime_suspend(struct device *dev)
{
	struct sensor_dev *sd = i2c_get_clientdata(to_i2c_client(dev));

	if (!sd)
		return 0;

	/*
	 * 真正的"省电"必须停掉数据源：
	 *   hrtimer_cancel() 会同步等待正在执行的定时器回调结束，再用
	 *   disable_irq() 关掉中断线（真机上传感器可能自己报 DRDY，光停定时器不够）。
	 * 只改一个标志而不停硬件，会出现"runtime_status 说已挂起、实际还在采样"
	 * 这种最典型的假省电（阶段 06 的测试项专门检查这一点）。
	 *
	 * 顺序：先停定时器（它可能正在触发中断）再关中断线，
	 * 反过来做会残留一个已经排队的中断。
	 */
	hrtimer_cancel(&sd->timer);
	disable_irq(sd->virq);
	WRITE_ONCE(sd->suspended, true);

	/*
	 * 把 hrtimer_active() 也打出来：seq/irq 冻结并不能证明定时器真停了
	 * （只 disable_irq 而忘了 hrtimer_cancel 时，计数照样冻结——阶段 06 验证者
	 * 用破坏性负控实验实证过这个盲点：只 disable_irq 而忘了 hrtimer_cancel 时，计数照样冻结）。timer_active=0 才是直接证据。
	 */
	dev_info(dev, "runtime suspend: sampling stopped (timer_active=%d irq_count=%u seq=%u)\n",
		 hrtimer_active(&sd->timer), sd->irq_count, sd->latest.seq);
	return 0;
}

static int sensor_runtime_resume(struct device *dev)
{
	struct sensor_dev *sd = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long interval;

	if (!sd)
		return 0;

	/*
	 * 恢复采样。用 interval_ms 的当前值重启定时器：挂起期间用户可能通过
	 * sysfs/ioctl 改过周期（sensor_apply_interval 那时只能"记账"），
	 * 恢复时按最新值生效，不会丢掉设置。
	 */
	interval = READ_ONCE(sd->interval_ms);
	enable_irq(sd->virq);
	WRITE_ONCE(sd->suspended, false);
	hrtimer_start(&sd->timer, ms_to_ktime(interval), HRTIMER_MODE_REL);

	dev_info(dev, "runtime resume: sampling restarted (timer_active=%d interval=%lums)\n",
		 hrtimer_active(&sd->timer), interval);
	return 0;
}

static const struct dev_pm_ops sensor_pm_ops = {
	SET_RUNTIME_PM_OPS(sensor_runtime_suspend, sensor_runtime_resume, NULL)
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

	/*
	 * 4. 采样环形缓冲（read 的数据源）。
	 *    kfifo_alloc 会把请求的字节数向上取整到 2 的幂，所以"64 个样本"最终可能
	 *    得到更多槽位（见 sensor_ring_capacity 的说明）；我们只依赖"满了会丢并计数"，
	 *    不依赖精确容量。
	 */
	ret = kfifo_alloc(&sd->ring, SENSOR_RING_SAMPLES * sizeof(struct sensor_sample),
			  GFP_KERNEL);
	if (ret) {
		dev_err(dev, "kfifo_alloc failed: %d\n", ret);
		goto err_clear_clientdata;
	}
	spin_lock_init(&sd->ring_lock);
	dev_info(dev, "ring ready: capacity=%u samples (%zu bytes each)\n",
		 sensor_ring_capacity(sd), sizeof(struct sensor_sample));

	/*
	 * 5. mmap 共享区：一页，只读映射给用户态。
	 *    __GFP_ZERO 让未写入区域为 0：用户态在第一个样本到达前读到的是"空状态"
	 *    （magic/version 已就位、count=0），而不是随机数据。
	 */
	sd->shm_addr = __get_free_pages(GFP_KERNEL | __GFP_ZERO, 0);
	if (!sd->shm_addr) {
		ret = -ENOMEM;
		dev_err(dev, "shared page alloc failed\n");
		goto err_free_ring;
	}
	sd->shm = (struct sensor_shm *)sd->shm_addr;
	sd->shm->magic = SENSOR_SHM_MAGIC;
	sd->shm->version = SENSOR_SHM_VERSION;
	mutex_init(&sd->shm_lock);
	sd->shm_publish_delay_us = shm_publish_delay_us;
	/*
	 * 放在赋值之后打印。教训：这条日志最初写在 "ring ready" 那行（赋值之前），
	 * 于是永远打印 0，把"参数没生效"的假象带进了排查过程 ——
	 * 证据日志的采样点必须在被观测状态确定之后。
	 */
	dev_info(dev, "shm publish delay = %u us\n", sd->shm_publish_delay_us);

	/* 4. 注册字符设备 -> /dev/sensor0 */
	ret = sensor_chrdev_register(sd);
	if (ret) {
		dev_err(dev, "chrdev register failed: %d\n", ret);
		goto err_free_shm;
	}

	/*
	 * 5. sysfs（设备参数）+ debugfs（运行统计）
	 *    sysfs 挂在字符设备类设备上 -> /sys/class/sensor_char/sensor0/
	 *    两个系统分别对应两种可见性：稳定 ABI（sysfs）与调试通道（debugfs）。
	 */
	ret = sysfs_create_group(&sd->char_dev->kobj, &sensor_attr_group);
	if (ret) {
		dev_err(dev, "sysfs_create_group failed: %d\n", ret);
		goto err_unreg_chrdev;
	}

	/*
	 * debugfs 创建失败不当作致命错误：它只在 CONFIG_DEBUG_FS 且实际挂载了
	 * debugfs 时才可用（生产内核常常不挂），属于可选调试通道。
	 * 失败时置 NULL：后续 debugfs_remove_recursive(NULL) 是安全的。
	 */
	sd->dbg = debugfs_create_dir(DRV_NAME, NULL);
	if (IS_ERR_OR_NULL(sd->dbg)) {
		dev_warn(dev, "debugfs unavailable: %ld\n", PTR_ERR(sd->dbg));
		sd->dbg = NULL;
	} else {
		debugfs_create_file("stats", 0444, sd->dbg, sd, &sensor_dbg_stats_fops);
		debugfs_create_file("ring", 0444, sd->dbg, sd, &sensor_dbg_ring_fops);
		debugfs_create_file("regs", 0444, sd->dbg, sd, &sensor_dbg_regs_fops);
	}
	dev_info(dev, "user interfaces ready: sysfs=/sys/class/%s/sensor%d, debugfs=%s\n",
		 DRV_NAME, MINOR(sd->devt), sd->dbg ? "/sys/kernel/debug/" DRV_NAME : "unavailable");

	/* 6. 中断：实验环境自建虚拟中断源，真机换成传感器 ALERT 引脚对应的 IRQ */
	sd->irq_domain = irq_domain_create_linear(NULL, 1,
						   &sensor_irq_domain_ops, NULL);
	if (IS_ERR(sd->irq_domain)) {
		ret = PTR_ERR(sd->irq_domain);
		goto err_remove_sysfs;
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

	/* 7. 启动采样定时器（真机上这一步由传感器的 DRDY/ALERT 中断替代） */
	hrtimer_init(&sd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sd->timer.function = sensor_timer_fn;
	hrtimer_start(&sd->timer, ms_to_ktime(sd->interval_ms), HRTIMER_MODE_REL);

	/*
	 * 8. Runtime PM：让设备在无人使用时自动挂起（停采样、关中断）。
	 *
	 * 引用计数要配平——用 get_noresume() + put_autosuspend() 成对：
	 *   get_noresume()  : 声明"probe 期间设备是活的"，但不去触发一次 resume；
	 *   set_active()    : 把 PM 状态标成 active（此时定时器确实已在跑）；
	 *   put_autosuspend(): 交还上面那一个引用 -> 引用降到 0，
	 *                      若此后没有任何 open，1 秒后内核回调 runtime_suspend。
	 *
	 * 为什么必须在 probe 里配平：若只 get 不 put（或什么都不做），
	 * 引用计数永远不为 0，设备永远不会挂起，autosuspend 形同虚设——
	 * 而表面上一切正常（不会有任何报错），属于最难发现的一类错误。
	 * 调用的顺序也要注意：先 set_active 再 enable，否则设备会被当成
	 * "已挂起"而多跑一次无意义的 resume。
	 */
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, SENSOR_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "runtime PM enabled (control=%s autosuspend=%dms)\n",
		 pm_runtime_enabled(dev) ? "auto" : "on",
		 SENSOR_AUTOSUSPEND_DELAY_MS);
	dev_info(dev, "probe done: major=%d minor=%d /dev/sensor%d\n",
		 MAJOR(sd->devt), MINOR(sd->devt), MINOR(sd->devt));
	return 0;

err_remove_domain:
	irq_domain_remove(sd->irq_domain);
err_remove_sysfs:
	/* 先摘掉用户可见接口，再往下走销毁 kobject/字符设备 */
	debugfs_remove_recursive(sd->dbg);
	sysfs_remove_group(&sd->char_dev->kobj, &sensor_attr_group);
err_unreg_chrdev:
	device_destroy(sensor_class, sd->devt);
	cdev_del(&sd->cdev);
	unregister_chrdev_region(sd->devt, 1);
err_free_shm:
	free_pages(sd->shm_addr, 0);
err_free_ring:
	kfifo_free(&sd->ring);
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

	/*
	 * 先关掉 runtime PM，再拆硬件。
	 *
	 * 为什么这个顺序最安全：pm_runtime_disable() 会同步等待正在执行的
	 * suspend/resume 回调结束，并让此后所有 PM 请求失败退出。
	 * 如果反过来（先 hrtimer_cancel/free_irq，最后才 disable），
	 * 在这个窗口里内核仍可能回调 sensor_runtime_resume()——
	 * 而 resume 里会 hrtimer_start()，把采样在原地重新启动，
	 * 与"正在拆卸设备"直接矛盾。
	 * 本函数开头正好是客户端已经与驱动解除绑定的时刻，也没有新的 open 能进来。
	 */
	pm_runtime_disable(&client->dev);

	hrtimer_cancel(&sd->timer);		/* 停掉采样源 */
	free_irq(sd->virq, sd);
	irq_domain_remove(sd->irq_domain);
	/*
	 * 摘掉用户可见接口：必须在 device_destroy() 之前。
	 * sysfs 属性挂在那颗 kobject 上，先销毁 kobject 会留下“属性回调里的
	 * sd 指针已被释放”的窗口；debugfs 传 NULL 是安全的（不可用时 sd->dbg 就是 NULL）。
	 */
	debugfs_remove_recursive(sd->dbg);
	sysfs_remove_group(&sd->char_dev->kobj, &sensor_attr_group);
	device_destroy(sensor_class, sd->devt);
	cdev_del(&sd->cdev);
	unregister_chrdev_region(sd->devt, 1);
	/*
	 * 释放驱动自己分配的两块内存：共享页与 kfifo 内部缓冲。
	 * 顺序上必须在"停掉生产者"（hrtimer_cancel + free_irq）之后 ——
	 * 否则中断处理还可能往已经释放的缓冲里写数据。
	 */
	free_pages(sd->shm_addr, 0);
	kfifo_free(&sd->ring);
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
		/* Runtime PM 回调挂在这里；挂到 driver 上而不是 device 上，
		 * 是因为同一套逻辑适用于该驱动的所有设备实例。 */
		.pm		= &sensor_pm_ops,
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
