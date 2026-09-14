// SPDX-License-Identifier: GPL-2.0
/*
 * virt_i2c.c - 虚拟 I2C 控制器 + 芯片寄存器级模拟（教学/实验用）
 *
 * ======================== 为什么需要这个驱动 ========================
 *
 * 本项目要验证的能力是："硬件由设备树描述 → 内核自动实例化 I2C 从设备 →
 * 驱动以标准 i2c_driver 形态注册 → 寄存器访问走 regmap"。
 *
 * 但 QEMU 的 virt 机器里没有 I2C 控制器。内核自带的 i2c-stub 虽然能造出一条
 * 虚拟总线，它的 i2c_adapter 却**没有设备树节点**，因此 i2c 核心不会调用
 * of_i2c_register_devices() 去枚举子节点，"设备树描述从设备"这条链路就断了。
 *
 * 所以这里实现一个最小的 I2C 控制器驱动：
 *   1. platform_driver，由 compatible = "lucien,virt-i2c" 匹配（设备树里声明）
 *   2. 注册 i2c_adapter，并设置 adap->dev.of_node = 本设备节点
 *      → i2c_register_adapter() 内部会调用 of_i2c_register_devices()
 *      → 子节点 sensor@48 被自动实例化成 i2c client
 *      → sensor_char.ko 的 probe(struct i2c_client *) 被调用
 *   3. 实现 i2c_algorithm.smbus_xfer，模拟一颗温度传感器的寄存器空间
 *
 * 换句话说：这个驱动扮演的是"板子上那颗 I2C 控制器 + 挂在总线上的传感器芯片"，
 * 让整个驱动栈（i2c core / regmap / 传感器驱动）跑在真实的内核代码路径上，
 * 而不是被桩函数替代。
 *
 * ======================== 芯片寄存器图（本项目自定义） ========================
 *
 *   0x00  TEMP      只读，16 位；数值 = 毫摄氏度 * 16 / 1000（LSB = 1/16 ℃）
 *   0x01  HUMIDITY  只读，16 位；数值 = %RH * 100（LSB = 0.01 %RH）
 *   0x02  CONFIG    读写，16 位；**bit0 = 连续转换使能：1=使能，0=关闭**
 *                   （模拟值，行为上只做记录；语义必须三处一致：本注释、
 *                    probe 里的上电默认值、上层驱动 sensor_char.c 的日志/写入值）
 *
 * 注：真实 TMP105 把 12 位温度放在寄存器高 12 位（低 4 位是状态位）。
 * 这里为了与项目既有代码/文档保持一致，采用"低 12 位有效"的简化约定，
 * 两种约定的差异只体现在换算函数里，不影响驱动栈本身的教学价值。
 *
 * ======================== 温度值必须是确定性的 ========================
 * 不能用随机数：测试脚本要断言"温度落在合理区间""样本序号递增"，
 * 随机数会让失败不可复现。这里用内部计数做锯齿漂移：24.0 ~ 26.0 ℃。
 *
 * ======================== 故障注入 ========================
 * /sys/kernel/debug/virt_i2c/inject_error  写 N → 接下来 N 次传输返回 -EIO
 * /sys/kernel/debug/virt_i2c/stats         读统计（transfers/errors/injected_left/...）
 * 用途：让上层驱动（regmap 错误处理、i2c_errors 计数、错误恢复路径）真的被执行到。
 *      "能制造故障"是验证错误处理代码的唯一办法。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/mod_devicetable.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kstrtox.h>
#include <linux/string.h>
#include <linux/err.h>

#define DRV_NAME	"virt_i2c"

/* 寄存器地址 */
#define VREG_TEMP	0x00
#define VREG_HUMIDITY	0x01
#define VREG_CONFIG	0x02
#define VREG_COUNT	0x03	/* 寄存器空间大小（超出范围的访问视为总线错误） */

/* 一条总线上模拟的从机地址范围：0x48 ~ 0x4b（每颗芯片一套独立寄存器） */
#define VBANK_BASE_ADDR	0x48
#define VBANK_MAX	4

/* 温度漂移参数：24.0 ~ 26.0 ℃，步进 0.1 ℃，21 次采样一个完整来回 */
#define TEMP_BASE_MILLI	25000
#define TEMP_STEP_MILLI	100
#define TEMP_PERIOD	21

/* 毫摄氏度 → 寄存器原始值（LSB = 1/16 ℃） */
#define MILLI_TO_RAW(m)	((u16)(s16)((m) * 16 / 1000))

/*
 * 一颗"芯片"的寄存器组。
 * 按从机地址分 bank，是因为一条总线上可以挂多颗同型号芯片：
 *   0x48 给字符设备驱动（sensor_char.ko）
 *   0x49 给 IIO 驱动（sensor_iio.ko，另一个阶段使用，两者互不干扰）
 */
struct virt_i2c_bank {
	u16	regs[VREG_COUNT];
	u32	samples;	/* 该芯片累计"转换"次数（每次读温度寄存器自增） */
	bool	present;	/* 该地址上是否真的有一颗芯片（不存在的地址返回 ENXIO） */
};

struct virt_i2c {
	struct device		*dev;
	struct i2c_adapter	adap;
	struct mutex		lock;		/* 保护 banks 与统计：smbus_xfer 可能被并发调用 */
	struct virt_i2c_bank	banks[VBANK_MAX];
	u32			transfers;	/* 累计传输次数 */
	u32			errors;		/* 累计失败次数（含注入） */
	u32			injected_left;	/* 故障注入剩余次数 */
	u32			ro_writes;	/* 对只读寄存器的非法写次数 */
	struct dentry		*dbg;
};

/*
 * 模拟一次芯片内部 ADC 转换：
 * 每次读温度寄存器时调用，产生确定性的锯齿波数据。
 */
static void virt_i2c_convert(struct virt_i2c_bank *bank)
{
	s32 milli;

	bank->samples++;
	milli = TEMP_BASE_MILLI +
		((s32)(bank->samples % TEMP_PERIOD) - (TEMP_PERIOD / 2)) * TEMP_STEP_MILLI;
	bank->regs[VREG_TEMP] = MILLI_TO_RAW(milli);

	/* 湿度：40.00 ~ 41.99 %RH，与温度用不同的周期，避免两者完全同步 */
	bank->regs[VREG_HUMIDITY] = (u16)(4000 + (bank->samples % 200));
}

/*
 * SMBus 传输实现。
 *
 * 为什么实现 smbus_xfer 而不是 master_xfer？
 *   regmap_i2c 在 reg_bits=8/val_bits=16 且适配器声明了 I2C_FUNC_SMBUS_WORD_DATA
 *   时会选择 regmap_smbus_word 后端，它内部调用 i2c_smbus_read_word_data()；
 *   i2c 核心看到适配器实现了 smbus_xfer 就直接调用它（不再走 master_xfer 模拟）。
 *   注意：**不要**声明 I2C_FUNC_I2C，否则 regmap 会改走 raw i2c_transfer 路径
 *   （需要 master_xfer 编码寄存器地址），与本实现不匹配。
 *
 * 返回值约定：0 成功；负数 errno（-ENXIO 地址无芯片、-EIO 传输失败、-EOPNOTSUPP 不支持的操作）
 */
static s32 virt_i2c_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			       unsigned short flags, char read_write,
			       u8 command, int size, union i2c_smbus_data *data)
{
	struct virt_i2c *chip = i2c_get_adapdata(adap);
	struct virt_i2c_bank *bank;
	u8 reg = command;
	s32 ret = 0;

	/* 地址上没有芯片：真实 I2C 从机不会产生 ACK，这里返回 ENXIO */
	if (addr < VBANK_BASE_ADDR || addr >= VBANK_BASE_ADDR + VBANK_MAX)
		return -ENXIO;

	bank = &chip->banks[addr - VBANK_BASE_ADDR];
	if (!bank->present)
		return -ENXIO;

	/* 寄存器越界：视为从机 NACK */
	if (reg >= VREG_COUNT)
		return -EIO;

	mutex_lock(&chip->lock);
	chip->transfers++;

	/*
	 * 故障注入：模拟"总线干扰/芯片无应答"。
	 * 放在锁内递减，保证并发注入时次数精确。
	 */
	if (chip->injected_left) {
		chip->injected_left--;
		chip->errors++;
		mutex_unlock(&chip->lock);
		dev_info(chip->dev, "injected error: addr=0x%02x reg=0x%02x (left=%u)\n",
			 addr, reg, chip->injected_left);
		return -EIO;
	}

	switch (size) {
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ) {
			if (reg == VREG_TEMP)
				virt_i2c_convert(bank);
			data->byte = bank->regs[reg] & 0xff;
		} else if (reg == VREG_TEMP || reg == VREG_HUMIDITY) {
			/* 只读寄存器被写：真实芯片不会回应，返回 -EIO */
			chip->ro_writes++;
			ret = -EIO;
		} else {
			bank->regs[reg] = (bank->regs[reg] & 0xff00) | data->byte;
		}
		break;

	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_READ) {
			if (reg == VREG_TEMP)
				virt_i2c_convert(bank);
			data->word = bank->regs[reg];
		} else if (reg == VREG_TEMP || reg == VREG_HUMIDITY) {
			chip->ro_writes++;
			ret = -EIO;
		} else {
			bank->regs[reg] = data->word;
		}
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	if (ret)
		chip->errors++;
	mutex_unlock(&chip->lock);
	return ret;
}

static u32 virt_i2c_functionality(struct i2c_adapter *adap)
{
	/*
	 * 只声明确实实现的能力（声明的每一位都会让 i2c 核心认为可以直接调用，
	 * 声明了却不实现属于 over-claim，上层会拿到一个莫名其妙的 -EOPNOTSUPP）。
	 *
	 * 为什么不含 I2C_FUNC_SMBUS_BYTE：本芯片是"寄存器型"器件，所有访问都带寄存器
	 * 地址（即命令字节），而无命令字节的 SMBus Byte 协议（size=I2C_SMBUS_BYTE）
	 * 在 smbus_xfer 里没有对应分支（走 default 返回 -EOPNOTSUPP）。
	 * 与其声明一个不实现的能力位，不如不声明。
	 *
	 * 声明 I2C_FUNC_I2C 会让 regmap 改走 raw i2c_transfer 路径（需要 master_xfer
	 * 自己编寄存器地址），与本实现不匹配，所以也不能声明。
	 */
	return I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA;
}

static const struct i2c_algorithm virt_i2c_algo = {
	.smbus_xfer	= virt_i2c_smbus_xfer,
	.functionality	= virt_i2c_functionality,
};

/* ============================ debugfs 接口 ============================ */

/*
 * 统计缓冲区大小。
 *
 * 为什么用"剩余空间"（rem）而不是固定 768 写死：如果 off 超过缓冲区，
 * size_t 的 (768 - off) 会下溢成一个巨大的值，scnprintf 就会越界写内存。
 * 用 rem 递减 + scnprintf 的截断语义（返回值 <= rem-1）可以保证永不下溢。
 */
#define VIRT_I2C_STATS_BUF	1024

static ssize_t virt_i2c_stats_read(struct file *filp, char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct virt_i2c *chip = filp->private_data;
	char *buf;
	int n, i;
	size_t off = 0, rem = VIRT_I2C_STATS_BUF;
	ssize_t ret;

	buf = kzalloc(VIRT_I2C_STATS_BUF, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&chip->lock);
	n = scnprintf(buf + off, rem,
		      "transfers=%u errors=%u injected_left=%u ro_writes=%u\n",
		      chip->transfers, chip->errors, chip->injected_left,
		      chip->ro_writes);
	off += n;
	rem -= (size_t)n;

	for (i = 0; i < VBANK_MAX && rem; i++) {
		struct virt_i2c_bank *b = &chip->banks[i];

		if (!b->present)
			continue;
		n = scnprintf(buf + off, rem,
			      "bank=0x%02x samples=%u temp_raw=0x%04x humidity_raw=%u config=0x%04x\n",
			      VBANK_BASE_ADDR + i, b->samples, b->regs[VREG_TEMP],
			      b->regs[VREG_HUMIDITY], b->regs[VREG_CONFIG]);
		off += n;
		rem -= (size_t)n;
	}
	mutex_unlock(&chip->lock);

	ret = simple_read_from_buffer(ubuf, len, ppos, buf, off);
	kfree(buf);
	return ret;
}

/*
 * 故障注入开关：写入十进制 N，接下来 N 次传输返回 -EIO。
 * 写 0 表示取消注入。
 */
static ssize_t virt_i2c_inject_write(struct file *filp, const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	struct virt_i2c *chip = filp->private_data;
	char kbuf[16];
	u32 n;

	if (len == 0 || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	if (kstrtou32(kbuf, 0, &n))
		return -EINVAL;

	mutex_lock(&chip->lock);
	chip->injected_left = n;
	mutex_unlock(&chip->lock);

	dev_info(chip->dev, "fault injection armed: next %u transfer(s) will fail\n", n);
	return len;
}

static const struct file_operations virt_i2c_stats_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= virt_i2c_stats_read,
	.llseek	= default_llseek,
};

static const struct file_operations virt_i2c_inject_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= virt_i2c_inject_write,
	.llseek	= default_llseek,
};

/* ============================ platform driver ============================ */

static int virt_i2c_probe(struct platform_device *pdev)
{
	struct virt_i2c *chip;
	int ret, i;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	mutex_init(&chip->lock);
	for (i = 0; i < VBANK_MAX; i++) {
		chip->banks[i].present = true;
		/* 上电默认：连续转换使能（bit0=1，见文件头寄存器图中的 CONFIG 定义） */
		chip->banks[i].regs[VREG_CONFIG] = 0x0001;
	}
	platform_set_drvdata(pdev, chip);

	chip->adap.owner = THIS_MODULE;
	chip->adap.algo = &virt_i2c_algo;
	chip->adap.dev.parent = &pdev->dev;
	strscpy(chip->adap.name, DRV_NAME, sizeof(chip->adap.name));
	i2c_set_adapdata(&chip->adap, chip);

	/*
	 * 关键一步：把设备树节点挂到 adapter 的设备上。
	 * i2c_register_adapter() 会执行 of_i2c_register_devices(adap)，
	 * 它遍历 adap->dev.of_node 的子节点，为每个带 reg 属性的子节点
	 * 创建 i2c client —— 这正是"设备树描述硬件"的核心机制。
	 * 如果这里不设置，子节点 sensor@48 永远不会被实例化。
	 */
	chip->adap.dev.of_node = pdev->dev.of_node;

	ret = i2c_add_adapter(&chip->adap);
	if (ret) {
		dev_err(&pdev->dev, "i2c_add_adapter failed: %d\n", ret);
		return ret;
	}

	chip->dbg = debugfs_create_dir(DRV_NAME, NULL);
	if (!IS_ERR_OR_NULL(chip->dbg)) {
		debugfs_create_file("stats", 0444, chip->dbg, chip, &virt_i2c_stats_fops);
		debugfs_create_file("inject_error", 0200, chip->dbg, chip, &virt_i2c_inject_fops);
	}

	/*
	 * 打印时用 chip->adap.dev.of_node（而不是 pdev->dev.of_node）：
	 * 前者才是 i2c 核心实际用来枚举子节点的那个字段，也是测试脚本真正要
	 * 断言的对象。用后者会打印出非空节点名，导致"删掉上面那行赋值"这种
	 * 真实故障在日志上完全看不出来（测试会假绿）。
	 * %pOF 对 NULL 节点打印 "<no-node>"，因此这行日志能直接区分设没设成功。
	 */
	dev_info(&pdev->dev, "registered i2c adapter i2c-%d, adapter.of_node=%pOF (sub-devices enumerated from DT)\n",
		 chip->adap.nr, chip->adap.dev.of_node);
	return 0;
}

/*
 * 注意：6.6 内核里 platform_driver.remove 的返回类型是 int（历史遗留），
 * 返回 void 的 .remove_new() 到更新的内核才统一为 .remove。
 */
static int virt_i2c_remove(struct platform_device *pdev)
{
	struct virt_i2c *chip = platform_get_drvdata(pdev);

	debugfs_remove_recursive(chip->dbg);
	i2c_del_adapter(&chip->adap);
	dev_info(&pdev->dev, "removed\n");
	return 0;
}

static const struct of_device_id virt_i2c_of_match[] = {
	{ .compatible = "lucien,virt-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, virt_i2c_of_match);

static struct platform_driver virt_i2c_driver = {
	.probe		= virt_i2c_probe,
	.remove		= virt_i2c_remove,
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= virt_i2c_of_match,
	},
};
module_platform_driver(virt_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lucien");
MODULE_DESCRIPTION("Virtual I2C controller with register-level chip emulation (teaching)");
MODULE_VERSION("1.0");
