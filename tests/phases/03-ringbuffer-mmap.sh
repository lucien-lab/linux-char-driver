#!/bin/busybox sh
# ============================================================================
# tests/phases/03-ringbuffer-mmap.sh - 阶段 03：kfifo 环形缓冲 + mmap 零拷贝
#                                      + 多进程并发
#
# 验证思路：每一项都要能"抓住一类具体错误"，而不是走过场。
#
#   溢出策略   调周期到 10ms 且 3 秒不读 → 队列必然填满 → dropped 必须 > 0。
#              只断言"计数增长"，不锁死具体数值（丢多少取决于调度抖动），
#              但"读方跟不上就必须有丢弃"是确定的。
#   队列可取数 同一时刻 ring_count ≥ 1：证明"不读的时候数据是攒着的"，
#              这正是环形缓冲存在的意义（旧实现只留最新一个样本）。
#   seqlock    100 轮一致性读里 mmap_invalid 必须为 0，且快照内样本 seq 严格递增。
#              校验单调性能抓住"没用 seqlock / 少了写屏障"造成的错位数据 ——
#              这类错误不会崩溃，只会静默给出乱序样本，只能靠数据自身性质发现。
#   只读映射   往映射区写必须触发 SIGSEGV；映射超过一页必须 EINVAL。
#              这两条是驱动里的显式校验，不验证就等于不知道它是否生效。
#   零拷贝收益 队列有数据时 read() 必须"立刻"返回（实测 buffered_read_ms < 100），
#              而不是睡到下一个采样周期 —— 这是"解耦采样与读取速率"的直接证据。
#   并发       8 进程 × 200 轮：不能有子进程失败、不能有非法样本、不能有 I2C 错误，
#              并且必须真的读到过样本（否则等于什么都没验）。
#              诚实说明：通过 ≠ 数学证明无竞态，只是把"明显错误"高概率暴露出来
#              （QEMU 单核 TCG 还会削弱并发交错机会），这一结论写进实现文档。
#   告警       必须在测试体执行之后重新 dmesg 取样（阶段 02 的教训：
#              取样窗口为 0 会让测试体期间的 WARNING 落在判定之外，形成假绿）。
# ============================================================================

# 从 [RING]/[CONC] 输出里取 "<键>=<值>"。先按空格拆成 token，
# 这样同一行里的多个键（如 mmap_reads=100 mmap_retries=3）都能取到。
tag_val() {	# tag_val <文件> <键> <默认值>
	v=$(tr ' ' '\n' < "$1" 2>/dev/null | sed -n "s/^$2=//p" | tr -d '\r' | head -1)
	[ -n "$v" ] && echo "$v" || echo "$3"
}

info "== 1) 环形缓冲与 mmap（/bin/ring_mmap_test）=="
/bin/ring_mmap_test > /tmp/ring.txt 2>&1
RC_RING=$?
sed 's/^/    /' /tmp/ring.txt

DROPPED=$(tag_val /tmp/ring.txt dropped 0)
check_gt "kfifo 队满丢弃计数可见（实测 dropped=$DROPPED ≥ 1）" "$DROPPED" 0

RING_CNT=$(tag_val /tmp/ring.txt ring_count 0)
check_gt "缓冲里仍有未读样本（实测 ring_count=$RING_CNT ≥ 1）" "$RING_CNT" 0

# ---- 阶段 03 整改新增：守恒不变量与容量 ----
# 不读书的那 3 秒里，每个中断产生的样本要么进队列、要么因队满被丢弃，
# 不允许"无声蒸发"。差 1 是允许的：硬中断可能已把 irq_count 加 1，
# 而线程化下半部还没来得及入队。
CAP=$(tag_val /tmp/ring.txt ring_capacity 0)
IRQ=$(tag_val /tmp/ring.txt irq_count 0)
DELTA=$(tag_val /tmp/ring.txt conservation_delta 99)
check_gt "环形缓冲实际容量可见（ring_capacity=$CAP ≥ 64）" "$CAP" 63
if [ "$DELTA" -ge 0 ] 2>/dev/null && [ "$DELTA" -le 1 ] 2>/dev/null; then
	pass "样本守恒：ring_count + dropped == irq_count（$RING_CNT + $DROPPED vs irq $IRQ，差 $DELTA ≤ 1）"
else
	fail "样本守恒：ring_count + dropped == irq_count" \
	     "实测 $RING_CNT + $DROPPED vs irq $IRQ，差 $DELTA（>1 说明有样本无声蒸发，
	     例如 copy_to_user 失败后直接丢弃）"
fi
DAC=$(tag_val /tmp/ring.txt dropped_at_capacity 0)
check_eq "发生过丢弃时队列必然是满的（dropped_at_capacity）" "1" "$DAC"

check_contains "mmap 映射成功" /tmp/ring.txt "\[RING\] mmap_ok=1"
check_contains "共享区 magic/version 正确" /tmp/ring.txt "\[RING\] mmap_magic_ok=1 version_ok=1"

READS=$(tag_val /tmp/ring.txt mmap_reads 0)
check_eq "seqlock 一致性读轮数（mmap_reads）" "100" "$READS"

INVALID=$(tag_val /tmp/ring.txt mmap_invalid 99)
check_eq "seqlock 一致性读无撕裂样本（mmap_invalid=0）" "0" "$INVALID"

MONO=$(tag_val /tmp/ring.txt mmap_seq_monotonic 0)
check_eq "快照内样本 seq 严格递增（无错位/回退）" "1" "$MONO"

# ---- 阶段 03 整改新增：共享区 seq 必须真的在推进 ----
# 这是抓"假 seqlock"的关键：初版实现只在内核侧用 seqlock_t，从不写 shm->seq，
# 用户态的重试逻辑是死代码（验证报告 S1，负控 NC-A/NC-B 对它零判别力）。
SEQ_ADV=$(tag_val /tmp/ring.txt shm_seq_advances 0)
SEQ_PF=$(tag_val /tmp/ring.txt shm_seq_probe_first 0)
SEQ_PL=$(tag_val /tmp/ring.txt shm_seq_probe_last 0)
check_eq "共享区 seq 真的在内核侧推进（探针：$SEQ_PF → $SEQ_PL）" "1" "$SEQ_ADV"

DISTINCT=$(tag_val /tmp/ring.txt drained_seq_distinct 0)
check_eq "连读 10 个样本序号严格递增（历史被保留，不是只有最新一个）" "1" "$DISTINCT"

DSAMPLES=$(tag_val /tmp/ring.txt drained_samples 0)
check_eq "连读样本数量达标（drained_samples=10）" "10" "$DSAMPLES"

RO=$(tag_val /tmp/ring.txt mmap_ro_sigsegv 0)
check_eq "共享区只读映射生效（写入触发 SIGSEGV）" "1" "$RO"

OVER=$(tag_val /tmp/ring.txt mmap_oversize_errno 0)
check_eq "超一页的映射被拒绝（errno=EINVAL=22）" "22" "$OVER"

# 该项的判别力来自"测量前把采样周期拉到 2 秒"（见 user/ring_mmap_test.c）：
# 从队列取 → ~1ms；等下一个样本 → ~2000ms。故 100ms 阈值能干净地区分两种实现。
BMS=$(tag_val /tmp/ring.txt buffered_read_ms 99999)
BIV=$(tag_val /tmp/ring.txt buffered_read_interval_ms 0)
if [ "$BMS" -lt 100 ]; then
	pass "队列有数据时 read 立即返回（周期 ${BIV}ms 下实测 ${BMS}ms < 100ms）"
else
	fail "队列有数据时 read 立即返回" \
	     "周期 ${BIV}ms 下实测 ${BMS}ms：疑似 read 还是\"等下一个样本\"而不是从环形缓冲取"
fi
check_eq "测量前已把采样周期拉大到 2s（保证该项有判别力）" "2000" "$BIV"

NEWFD=$(tag_val /tmp/ring.txt new_fd_sees_buffered 0)
check_eq "新开的 fd 立刻读到已缓冲样本（队列语义）" "1" "$NEWFD"

check_true "ring_mmap_test 退出码为 0" "0" "$RC_RING"
check_contains "ring_mmap_test 整体结论为 PASS" /tmp/ring.txt "\[RING\] OVERALL=PASS"

info "== 2) 多进程并发（/bin/concurrency_test）=="
/bin/concurrency_test > /tmp/conc.txt 2>&1
RC_CONC=$?
sed 's/^/    /' /tmp/conc.txt

CFAILED=$(tag_val /tmp/conc.txt failed 99)
check_eq "8 进程 × 200 轮无失败子进程（failed=0）" "0" "$CFAILED"

CINV=$(tag_val /tmp/conc.txt invalid_samples 99)
check_eq "并发下无非法样本（invalid_samples=0）" "0" "$CINV"

CIOERR=$(tag_val /tmp/conc.txt io_errors 99)
check_eq "并发下无非预期 errno（io_errors=0）" "0" "$CIOERR"

CI2C=$(tag_val /tmp/conc.txt i2c_errors 99)
check_eq "并发下无 I2C 传输错误（i2c_errors=0）" "0" "$CI2C"

CSAMPLES=$(tag_val /tmp/conc.txt samples_read 0)
check_gt "并发确实读到了样本（samples_read）" "$CSAMPLES" 0

CSNAP=$(tag_val /tmp/conc.txt snapshots 0)
check_gt "并发下 mmap 快照读取有效（snapshots）" "$CSNAP" 0

# ---- 阶段 03 整改新增：出队全局唯一性（多核下抓真竞态）----
# 每个子进程把自己读到的样本 seq 写入共享数组，父进程排序后查重复。
# 正确实现下一条样本只能被一个读者取走一次 → 重复 seq 就是出队竞态的锛证。
# 原版子进程解析出 seq 后直接丢弃，于是"同一样本被读两次"完全不可见（验证报告 S2）。
CSEQ=$(tag_val /tmp/conc.txt consumed_seqs 0)
CDUP=$(tag_val /tmp/conc.txt dup_seqs 99)
check_gt "并发确实消费到了样本序号（consumed_seqs）" "$CSEQ" 0
check_eq "出队全局无重复（dup_seqs=0，即无"同一样本被读两次"）" "0" "$CDUP"
CSEQMIN=$(tag_val /tmp/conc.txt seq_min 0)
CSEQMAX=$(tag_val /tmp/conc.txt seq_max 0)
info "并发消费序号范围：$CSEQMIN ~ $CSEQMAX（缺口数=$(tag_val /tmp/conc.txt seq_gaps 0)，缺口来自队满丢弃/EAGAIN，属正常）"

check_true "concurrency_test 退出码为 0" "0" "$RC_CONC"
check_contains "concurrency_test 整体结论为 PASS" /tmp/conc.txt "\[CONC\] OVERALL=PASS"

info "== 3) seqlock 放大器验证（受控放大写入窗口，让重试路径真的被执行）=="
# 为什么必须做这一步：正常写入窗口只有 ~1µs，而采样周期是 10ms，
# 读者撞上"写入中"的概率约 10⁻⁴ —— "seqlock 生效"的正向证据在自然条件下拿不到。
# 打开驱动的 shm_publish_delay_us 参数把窗口从 ~1µs 放大到 5ms 后：
#   重试次数 > 0  ：证明协议路径真的被执行（而不是重试逻辑是死代码）
#   撕裂样本 = 0 ：证明协议真的有效（一致性成立）
# 这两项配合起来，才能把"假 seqlock"与"真 seqlock"区分开。
KVER=$(uname -r)
AMP_OK=1
if rmmod sensor_char 2>/tmp/rmmod.txt && \
   insmod "/lib/modules/$KVER/sensor_char.ko" shm_publish_delay_us=5000 2>/tmp/insmod.txt; then
	sleep 1
	/bin/ring_mmap_test quick > /tmp/ring_amp.txt 2>&1
	RC_AMP=$?
	sed 's/^/    /' /tmp/ring_amp.txt | head -20
	AMP_RETRIES=$(tag_val /tmp/ring_amp.txt mmap_retries 0)
	AMP_INVALID=$(tag_val /tmp/ring_amp.txt mmap_invalid 99)
	AMP_ODD=$(tag_val /tmp/ring_amp.txt shm_seq_odd_seen 0)
	AMP_CHG=$(tag_val /tmp/ring_amp.txt shm_seq_changes 0)
	AMP_MS=$(tag_val /tmp/ring_amp.txt loop_elapsed_ms 0)
	AMP_READS=$(tag_val /tmp/ring_amp.txt mmap_reads 0)
	info "放大器模式：iters=$AMP_READS elapsed=${AMP_MS}ms retries=$AMP_RETRIES odd_seen=$AMP_ODD seq_changes=$AMP_CHG invalid=$AMP_INVALID"
	check_gt "读者确实在跟踪写方（shm_seq_changes=$AMP_CHG ≥ 3）" "$AMP_CHG" 2
	check_gt "读者确实撞上过写入窗口（shm_seq_odd_seen=$AMP_ODD > 0）" "$AMP_ODD" 0
	check_gt "seqlock 重试路径真的被执行（mmap_retries=$AMP_RETRIES > 0）" "$AMP_RETRIES" 0
	check_eq "重试后数据始终一致（放大窗口下 mmap_invalid=0）" "0" "$AMP_INVALID"
	check_true "放大器模式程序退出码为 0" "0" "$RC_AMP"
else
	AMP_OK=0
	fail "放大器模式重新加载驱动（insmod shm_publish_delay_us=5000）" \
	     "rmmod/insmod 失败：$(cat /tmp/rmmod.txt /tmp/insmod.txt 2>/dev/null | head -2 | tr '\n' ' ')"
fi

# 无论放大器实验成败，都要把驱动恢复成默认参数，否则后续检查会被放大窗口拖慢
rmmod sensor_char 2>/dev/null
insmod "/lib/modules/$KVER/sensor_char.ko" 2>/dev/null
sleep 1
check_exists "放大器实验后设备节点恢复（/dev/sensor0）" /dev/sensor0

info "== 4) 回归：原有接口（read/ioctl/write）仍可用 =="
/bin/sensor_test > /tmp/utest3.txt 2>&1
RC_UT=$?
check_true "sensor_test 退出码为 0" "0" "$RC_UT"
check_contains "sensor_test 正常结束" /tmp/utest3.txt "测试结束"
check_contains "stats 新增环形缓冲字段可见" /tmp/utest3.txt "ring:"
/bin/sensor_stat > /tmp/stat3.txt 2>&1
info "驱动统计：$(cat /tmp/stat3.txt 2>/dev/null)"

info "== 5) 内核告警检查（测试体之后重新取样）=="
dmesg > /tmp/dmesg-03.txt 2>/dev/null
W=$(grep -cE 'WARNING:|Call trace:' /tmp/dmesg-03.txt)
check_eq "dmesg 无 WARNING/Call trace（取样覆盖整段测试）" "0" "$W"
if [ "$W" != "0" ]; then
	info "告警上下文（最多 5 行）："
	grep -nE 'WARNING:|Call trace:' /tmp/dmesg-03.txt | head -5 | sed 's/^/        /'
fi
