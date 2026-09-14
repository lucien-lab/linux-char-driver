// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_iio.c - 同一颗传感器，改用内核 IIO 子系统实现（与字符设备版并存对照）
 *
 * ======================== 为什么传感器驱动应该走 IIO ========================
 *
 * sensor_char.c 用"自创 /dev/sensor0 + 自定义 ioctl"的方式暴露一颗温度/湿度传感器。
 * 这在教学和专用设备上没问题，但对"通用传感器"来说是错的层次：内核里已经有
 * 一个专门为这类器件设计的子系统 —— IIO（Industrial I/O）。
 *
 * 走 IIO 之后，下面这些事全部由子系统接管，不再由驱动各写一套：
 *   - 统一的用户态 ABI：/sys/bus/iio/devices/iio:deviceN/{name,in_temp_input,...}
 *     （用户态不用再读每个厂商私有的 ioctl 结构体，工具链 libiio 直接可用）
 *   - 通道枚举：in_temp_*、in_humidityrelative_* 这些文件名由 channel 规格推导
 *   - 单点读（sysfs）与流式读（/dev/iio:deviceN 字符设备 + 内核 kfifo 缓冲）两条路径
 *   - 触发与缓冲框架：谁触发采样（trigger）、怎么进缓冲、多设备共享一个触发源
 *   - 单位与换算约定（raw / scale / offset / processed 与 milli 单位体系）
 *
 * 本文件要回答的问题是"子系统框架替你做了什么"，因此刻意写得像真实上游驱动。
 *
 * ======================== 两条取数路径（本项目两条都实现了） ========================
 *
 *   1) sysfs 单点读取：
 *        用户态 cat /sys/bus/iio/devices/iio:deviceN/in_temp_raw
 *          → 内核 VFS → iio_read_channel_info() → driver 的 read_raw()
 *        read_raw 通过 regmap 读传感器寄存器，返回 raw 值或 processed 值。
 *        上下文：进程上下文（sysfs 读写），**允许睡眠**（I2C 访问会睡眠）。
 *
 *   2) 缓冲区流式读取（triggered buffer）：
 *        用户态 read(/dev/iio:deviceN)
 *          → IIO 内核缓冲（kfifo）→ 由 trigger 周期性地把样本推进来
 *        触发源用内核标准的 hrtimer software trigger（configfs 实例化），
 *        每次触发会调用本驱动的 trigger_handler()；handler 里读寄存器后
 *        调用 iio_push_to_buffers_with_timestamp()。
 *        上下文：IIO 把 poll func 注册成**线程化**处理函数（见 iio_alloc_pollfunc
 *        的 thread 参数），所以 handler 里可以睡眠 —— 这正是 regmap/I2C 能直接
 *        用在缓冲路径上的原因。
 *
 * ======================== 寄存器图（与 virt_i2c.c 的芯片模拟一致） ========================
 *
 *   0x00  TEMP      只读，16 位；数值 = 毫摄氏度 * 16 / 1000（LSB = 1/16 ℃ = 62.5 m℃）
 *   0x01  HUMIDITY  只读，16 位；数值 = %RH * 100（LSB = 0.01 %RH = 10 m%RH）
 *   0x02  CONFIG    读写，16 位；bit0 = 连续转换使能（1=使能）
 *
 * 本驱动挂在 0x49（设备树 sensor@49），字符设备驱动挂在 0x48：
 * 两颗芯片是 virt_i2c 里互相独立的 bank，因此两个驱动可以同时装载、互不干扰。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define DRV_NAME		"sensor_iio"

#define SENSOR_REG_TEMP		0x00
#define SENSOR_REG_HUMIDITY	0x01
#define SENSOR_REG_CONFIG	0x02
#define SENSOR_CFG_CONT_EN	0x0001

/*
 * 换算约定（IIO 的 raw / scale / processed 三件套）：
 *
 *   温度：LSB = 1/16 ℃ = 62.5 毫摄氏度
 *         → scale = 62 + 500000/1000000（IIO_VAL_INT_PLUS_MICRO，单位"毫摄氏度/LSB"）
 *         → processed(毫摄氏度) = raw * 1000 / 16，与 sensor_char.c 的换算同源
 *   湿度：LSB = 0.01 %RH = 10 毫%RH
 *         → scale = 10（IIO_VAL_INT，单位"毫%RH/LSB"）
 *         → processed(毫%RH) = raw * 10
 *
 * 注意 processed 一律用"毫"为单位，这是 IIO 的约定：用户态拿到的数值与
 * scale 相乘即可还原物理量，不用关心驱动内部用什么位宽/寄存器。
 */
#define SENSOR_TEMP_SCALE_INT	62
#define SENSOR_TEMP_SCALE_MICRO	500000
#define SENSOR_HUM_SCALE_MILLI	10

struct sensor_iio {
	struct regmap	*regmap;
	struct mutex	lock;		/* 保护 regmap：sysfs 读与 trigger_handler 可能并发 */
	u32		read_errors;	/* 寄存器读失败次数（内部统计；受 st->lock 保护） */
};

/*
 * regmap 配置必须显式声明寄存器值字节序。
 * regmap_get_val_endian() 在没人声明时默认返回 BIG，于是 regmap_i2c 会挑
 * regmap_smbus_word_swapped 后端对 16 位值做 swab16()，读出来的温度就是乱的
 * （字符设备版踩过这个坑：25 ℃ 被读成 2000+ ℃）。SMBus word 本身是低字节在前，
 * 所以这里声明 LITTLE。
 */
static const struct regmap_config sensor_iio_regmap_cfg = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.max_register		= SENSOR_REG_CONFIG,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
};

/* 寄存器原始值 -> 毫摄氏度（LSB = 62.5 m℃）。先转 s16 再乘，负温才是对的。 */
static s32 sensor_iio_raw_to_milli(int raw)
{
	return ((s32)(s16)raw) * 1000 / 16;
}

/* 读一个寄存器，失败计入 read_errors。只在这里加锁，调用者不需要再持锁。 */
static int sensor_iio_read_reg(struct sensor_iio *st, u8 reg, int *raw)
{
	unsigned int val;
	int ret;

	mutex_lock(&st->lock);
	ret = regmap_read(st->regmap, reg, &val);
	if (ret)
		st->read_errors++;	/* 自增放在锁内：与读路径并发时不留数据竞争（KCSAN 可证） */
	mutex_unlock(&st->lock);
	if (ret)
		return ret;
	*raw = (s16)val;	/* 芯片以补码表示负温，按有符号 16 位解释 */
	return 0;
}

/* ============================ sysfs 单点读取路径 ============================ */

static int sensor_iio_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask)
{
	struct sensor_iio *st = iio_priv(indio_dev);
	int raw, ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = sensor_iio_read_reg(st, chan->address, &raw);
		if (ret)
			return ret;
		*val = raw;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		ret = sensor_iio_read_reg(st, chan->address, &raw);
		if (ret)
			return ret;
		if (chan->type == IIO_TEMP)
			*val = sensor_iio_raw_to_milli(raw);	/* 毫摄氏度 */
		else
			*val = raw * SENSOR_HUM_SCALE_MILLI;	/* 毫 %RH */
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_TEMP) {
			*val = SENSOR_TEMP_SCALE_INT;
			*val2 = SENSOR_TEMP_SCALE_MICRO;
			return IIO_VAL_INT_PLUS_MICRO;
		}
		*val = SENSOR_HUM_SCALE_MILLI;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_info sensor_iio_info = {
	.read_raw = sensor_iio_read_raw,
};

static const struct iio_chan_spec sensor_iio_channels[] = {
	{
		.type = IIO_TEMP,
		.address = SENSOR_REG_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = 0,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_CPU,
		},
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.address = SENSOR_REG_HUMIDITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = 1,
		.scan_type = {
			.sign = 'u',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_CPU,
		},
	},
	/*
	 * 软件时间戳通道：scan_index 必须紧跟数据通道。
	 * 它让"缓冲区里的样本布局"对用户态是可描述的（用户态按
	 * scan_elements 目录下各通道的 index 属性给出的偏移解析，见 user/iio_read_test.c）。
	 */
	IIO_CHAN_SOFT_TIMESTAMP(2),
};

/* ============================ 缓冲区流式读取路径 ============================ */

/*
 * trigger_handler：每收到一次 trigger（本项目是 hrtimer 周期性触发）就被调用一次。
 *
 * 参数 p 是 iio_poll_func（由 IIO 触发框架传入），从中取出 indio_dev。
 * 运行上下文是线程化的（可睡眠），所以这里可以直接发 I2C 传输 —— 这是
 * "buffer 路径 + 真实硬件访问" 的标准写法。
 */
static irqreturn_t sensor_iio_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct sensor_iio *st = iio_priv(indio_dev);
	/*
	 * 扫描数据缓冲区：字段顺序必须与 channel 的 scan_index 顺序一致
	 * （core 按启用通道从这块内存里顺序取 storagebits 大小的字段）。
	 * ts 用 __aligned(8) 保证时间戳落在 8 字节对齐处。
	 */
	struct {
		s16 temp;
		s16 humidity;
		s64 ts __aligned(8);
	} scan;
	int raw, ret;

	/* 只读被启用的通道：active_scan_mask 的第 N 位对应 scan_index=N */
	if (test_bit(0, indio_dev->active_scan_mask)) {
		ret = sensor_iio_read_reg(st, SENSOR_REG_TEMP, &raw);
		scan.temp = ret ? 0 : (s16)raw;
	}
	if (test_bit(1, indio_dev->active_scan_mask)) {
		ret = sensor_iio_read_reg(st, SENSOR_REG_HUMIDITY, &raw);
		scan.humidity = ret ? 0 : (s16)raw;
	}

	iio_push_to_buffers_with_timestamp(indio_dev, &scan,
					   iio_get_time_ns(indio_dev));

	/* 通知触发框架本次处理结束（否则 trigger 不会再次投递） */
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* ============================ i2c_driver probe/remove ============================ */

static int sensor_iio_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sensor_iio *st;
	struct iio_dev *indio_dev;
	unsigned int cfg;
	int ret;

	/* 第三参数是私有数据大小，iio_priv() 取出来的就是这块（devm 自动释放） */
	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	mutex_init(&st->lock);

	st->regmap = devm_regmap_init_i2c(client, &sensor_iio_regmap_cfg);
	if (IS_ERR(st->regmap)) {
		dev_err(dev, "regmap init failed: %ld\n", PTR_ERR(st->regmap));
		return PTR_ERR(st->regmap);
	}

	/*
	 * 芯片在位探测：读配置寄存器。
	 * 真机驱动同样会在 probe 里读 WHO_AM_I / 配置寄存器确认器件存在，
	 * 而不是"注册完就算成功"。
	 */
	ret = regmap_read(st->regmap, SENSOR_REG_CONFIG, &cfg);
	if (ret) {
		dev_err(dev, "chip not responding (regmap_read=%d)\n", ret);
		return ret;
	}
	dev_info(dev, "chip detected: config=0x%04x (continuous conversion %s)\n",
		 cfg, (cfg & SENSOR_CFG_CONT_EN) ? "already enabled" : "off");

	ret = regmap_write(st->regmap, SENSOR_REG_CONFIG, SENSOR_CFG_CONT_EN);
	if (ret) {
		dev_err(dev, "failed to enable continuous conversion: %d\n", ret);
		return ret;
	}

	indio_dev->name = DRV_NAME;
	indio_dev->info = &sensor_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;	/* 缓冲区模式由下面 setup 追加 */
	indio_dev->channels = sensor_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(sensor_iio_channels);

	/*
	 * 装配 triggered buffer：
	 *   第一个回调（top half）传 NULL：hrtimer 触发本身就是软中断上下文，
	 *   没有"硬中断上半部"要做的事；
	 *   第二个回调是线程化处理函数，里面可以睡眠（I2C 访问）。
	 * 该函数会把 INDIO_BUFFER_TRIGGERED 加进 indio_dev->modes，
	 * 于是 /sys/.../buffer/ 与 /dev/iio:deviceN 才会出现。
	 */
	ret = devm_iio_triggered_buffer_setup(dev, indio_dev, NULL,
					      sensor_iio_trigger_handler, NULL);
	if (ret) {
		dev_err(dev, "triggered buffer setup failed: %d\n", ret);
		return ret;
	}

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret) {
		dev_err(dev, "iio_device_register failed: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(client, indio_dev);
	dev_info(dev, "IIO device registered: name=%s channels=%d (temp + humidity + timestamp)\n",
		 indio_dev->name, indio_dev->num_channels);
	return 0;
}

/*
 * remove 里不需要手动拆缓冲/注销设备：probe 全程用 devm_* 接口，
 * 内核会按申请顺序的逆序自动释放（devm_iio_device_register → devres 注销设备，
 * devm_iio_triggered_buffer_setup → 释放缓冲，devm_regmap_init_i2c → 释放 regmap）。
 * 这里只留一条日志，方便在 dmesg 里确认卸载路径真的走到了。
 */
static void sensor_iio_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "removed\n");
}

static const struct i2c_device_id sensor_iio_id[] = {
	{ "sensor_iio", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sensor_iio_id);

static const struct of_device_id sensor_iio_of_match[] = {
	{ .compatible = "lucien,sensor-iio" },
	{ }
};
MODULE_DEVICE_TABLE(of, sensor_iio_of_match);

static struct i2c_driver sensor_iio_driver = {
	.driver = {
		.name		= DRV_NAME,
		.of_match_table	= sensor_iio_of_match,
	},
	.probe		= sensor_iio_probe,
	.remove		= sensor_iio_remove,
	.id_table	= sensor_iio_id,
};
module_i2c_driver(sensor_iio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lucien");
MODULE_DESCRIPTION("IIO driver for the virtual temperature/humidity sensor (teaching)");
MODULE_VERSION("1.0");
