#!/bin/busybox sh
# ============================================================================
# tests/phases/05-iio.sh - 阶段 05：IIO 子系统驱动验证
#
# 验证范围（对应 docs/11「阶段 05」契约）：
#   1. IIO 驱动是否真的把设备注册进了 IIO 子系统（而不是自己造 /dev）
#   2. sysfs 单点读取路径：raw / processed / scale 三个属性是否语义正确
#   3. 设备是否通过设备树 + i2c 子系统绑定（sensor@49 → sensor_iio）
#   4. hrtimer software trigger 能否用 configfs 实例化并绑定到设备
#   5. 缓冲区流式读取路径：/dev/iio:deviceN 能否读到合法样本
#   6. 无内核告警
#
# 与 01/02/03 阶段的区别：本阶段不加载 sensor_char.ko（见 05-iio.modules），
# 所以这里不断言 /dev/sensor0 相关行为。
# ============================================================================

info "== 1) IIO 设备注册（按 name 发现，不写死设备编号）=="
IIO_DIR=""
for d in /sys/bus/iio/devices/iio:device*; do
    [ -e "$d/name" ] || continue
    if [ "$(cat "$d/name" 2>/dev/null)" = "sensor_iio" ]; then
        IIO_DIR="$d"
        break
    fi
done
check_nonempty "IIO 设备已注册（name=sensor_iio）" "$IIO_DIR"
if [ -z "$IIO_DIR" ]; then
    fail "后续检查依赖 IIO 设备" "未找到 iio:deviceN（name=sensor_iio），中止"
    return 0
fi
IIO_PATH="/dev/$(basename "$IIO_DIR")"
info "  IIO 设备目录: $IIO_DIR"
info "  字符设备    : $IIO_PATH"
info "  /sys/bus/iio/devices 内容: $(ls /sys/bus/iio/devices/)"

info "== 2) 设备树 + i2c 绑定（sensor@49 → sensor_iio）=="
I2C_DEV=""
for d in /sys/bus/i2c/devices/*-0049; do
    [ -e "$d/name" ] || continue
    I2C_DEV="$d"
    break
done
check_nonempty "i2c 从设备 0x49 由设备树枚举出来" "$I2C_DEV"
if [ -n "$I2C_DEV" ]; then
    DRVLINK="$(readlink -f "$I2C_DEV/driver" 2>/dev/null)"
    case "$DRVLINK" in
        *sensor_iio*) pass "0x49 已绑定到 sensor_iio 驱动（${DRVLINK}）" ;;
        *) fail "0x49 绑定到 sensor_iio 驱动" "实际 driver=$DRVLINK" ;;
    esac
    check_contains "从设备名来自设备树 compatible" "$I2C_DEV/name" "sensor-iio"
fi

info "== 3) sysfs 单点读取路径：raw / processed / scale =="
RAW="$(cat "$IIO_DIR/in_temp_raw" 2>/dev/null)"
info "  in_temp_raw=$RAW"
case "$RAW" in
    ''|*[!0-9-]*) fail "in_temp_raw 可读且为整数" "读到 '$RAW'" ;;
    *)
        if [ "$RAW" -ge 240 ] && [ "$RAW" -le 416 ]; then
            pass "in_temp_raw 落在芯片模拟区间（24~26℃ → raw 384±32，实测 ${RAW}）"
        else
            fail "in_temp_raw 落在芯片模拟区间" "实测 raw=${RAW}（期望 240~416；若为字节交换值如 0x9001→36865 会在此暴露）"
        fi
        ;;
esac

PROC="$(cat "$IIO_DIR/in_temp_input" 2>/dev/null)"
info "  in_temp_input(processed)=$PROC"
case "$PROC" in
    ''|*[!0-9-]*) fail "in_temp_input 可读且为整数（毫摄氏度）" "读到 '$PROC'" ;;
    *)
        if [ "$PROC" -ge 15000 ] && [ "$PROC" -le 45000 ]; then
            pass "in_temp_input 在合理区间（15000~45000 毫℃ 实测 ${PROC}）"
        else
            fail "in_temp_input 在合理区间" "实测 $PROC 毫℃"
        fi
        ;;
esac

SCALE="$(cat "$IIO_DIR/in_temp_scale" 2>/dev/null)"
check_eq "in_temp_scale 为 62.500000（1/16 ℃ = 62.5 毫℃/LSB）" "62.500000" "$SCALE"

HRAW="$(cat "$IIO_DIR/in_humidityrelative_raw" 2>/dev/null)"
info "  in_humidityrelative_raw=$HRAW"
check_nonempty "湿度通道 raw 可读" "$HRAW"

info "== 4) hrtimer software trigger（configfs 实例化）=="
CFG=/sys/kernel/config/iio/triggers/hrtimer
mkdir -p "$CFG/inst0" 2>/dev/null
if [ -d "$CFG/inst0" ]; then
    pass "configfs 实例化 hrtimer trigger 成功（$CFG/inst0）"
else
    fail "configfs 实例化 hrtimer trigger" "$CFG/inst0 不存在（configfs 是否挂载？IIO_CONFIGFS 是否编入？）"
fi

# 找到 trigger 设备目录（名字 = configfs 实例名，见 iio_trigger_alloc(NULL, "%s", name)）
TRIG_DIR=""
for t in /sys/bus/iio/devices/trigger*; do
    [ -e "$t/name" ] || continue
    if [ "$(cat "$t/name" 2>/dev/null)" = "inst0" ]; then
        TRIG_DIR="$t"
        break
    fi
done
check_nonempty "trigger 已注册到 IIO（name=inst0）" "$TRIG_DIR"
info "  trigger 设备目录: $TRIG_DIR"

# 坑：kernel 6.6 的 sampling_frequency 挂在 **trigger 设备** 上，不在 configfs 实例目录里。
# 内核源码：drivers/iio/trigger/iio-trig-hrtimer.c:80
#   static DEVICE_ATTR(sampling_frequency, S_IRUGO | S_IWUSR, ...)
# 而 configfs 实例目录的 config_item_type 只填了 .ct_owner（同文件 iio_hrtimer_type），
# 所以往 /sys/kernel/config/.../inst0/sampling_frequency 写会得到 EACCES（用 O_CREAT 打不开）。
# 正确路径是 /sys/bus/iio/devices/triggerN/sampling_frequency。
if [ -n "$TRIG_DIR" ]; then
    echo 50 > "$TRIG_DIR/sampling_frequency" 2>/dev/null
    FREQ="$(cat "$TRIG_DIR/sampling_frequency" 2>/dev/null)"
    info "  sampling_frequency 写入 50 后回读: $FREQ"
    case "$FREQ" in
        50*) pass "可通过 trigger 设备设置采样频率（实测 ${FREQ}）" ;;
        *)   fail "可通过 trigger 设备设置采样频率" "写入 50 后回读 '$FREQ'" ;;
    esac
fi

info "== 5) 缓冲区流式读取路径 =="
if [ -n "$TRIG_DIR" ]; then
    echo inst0 > "$IIO_DIR/trigger/current_trigger" 2>/dev/null
    check_eq "current_trigger 已绑定为 inst0" "inst0" "$(cat "$IIO_DIR/trigger/current_trigger" 2>/dev/null)"
else
    fail "current_trigger 绑定" "没有可用的 trigger"
fi

for en in in_temp_en in_humidityrelative_en in_timestamp_en; do
    echo 1 > "$IIO_DIR/scan_elements/$en" 2>/dev/null
    check_eq "scan_elements/$en 已启用" "1" "$(cat "$IIO_DIR/scan_elements/$en" 2>/dev/null)"
done

# 缓冲长度：默认很小（本项目实测 2），流式读会来不及取就丢样本。
# 必须在 buffer/enable=0 时设置（内核会拒绝运行中改长度）。
echo 64 > "$IIO_DIR/buffer/length" 2>/dev/null
check_eq "buffer/length 可设置为 64" "64" "$(cat "$IIO_DIR/buffer/length" 2>/dev/null)"

echo 1 > "$IIO_DIR/buffer/enable" 2>/dev/null
check_eq "buffer 已开启（buffer/enable=1）" "1" "$(cat "$IIO_DIR/buffer/enable" 2>/dev/null)"

info "== 6) 用户态通过 /dev/$(basename "$IIO_DIR") 读缓冲样本 =="
/bin/iio_read_test "$IIO_PATH" "$IIO_DIR" > /tmp/iio.txt 2>&1
check_true "iio_read_test 退出码为 0" "0" "$?"
BOFF="$(grep -m1 'layout stride=' /tmp/iio.txt)"
info "  $BOFF"
# 布局自检：temp(16bit)+humidity(16bit)+timestamp(64bit, 8 字节对齐) → stride 应为 16
# 这条能挡住"用户态按 =_index 当偏移"这类解析错误（那种错会算出 stride=10 或读到错位数据）
STRIDE="$(sed -n 's/.*layout stride=\([0-9]*\).*/\1/p' /tmp/iio.txt | head -1)"
check_eq "扫描元素跨度与内核布局算法一致（stride=16 字节）" "16" "${STRIDE:-}"
check_contains "读到的样本数达到要求" /tmp/iio.txt "OVERALL=PASS"
DEV_READS="$(sed -n 's/.*dev_reads=\([0-9]*\).*/\1/p' /tmp/iio.txt | head -1)"
check_gt "成功读到 ≥3 个缓冲样本（实测 ${DEV_READS:-0}）" "${DEV_READS:-0}" 2

info "== 7) 内核告警检查（在测试体执行之后取样）=="
dmesg > /tmp/dmesg-iio.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:' /tmp/dmesg-iio.txt)
check_eq "dmesg 无 WARNING/Call trace" "0" "$W"
if [ "$W" != "0" ]; then
    grep -nE 'WARNING:|Call trace:' /tmp/dmesg-iio.txt | head -5
fi

# 收尾：关掉缓冲，便于后续排查（不影响检查结论）
echo 0 > "$IIO_DIR/buffer/enable" 2>/dev/null
