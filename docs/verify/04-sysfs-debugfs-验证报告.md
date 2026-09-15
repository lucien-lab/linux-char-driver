# 阶段 04 验证报告：sysfs 设备参数 + debugfs 运行统计（独立复核）

- 验证者：独立验证者（工作树 `04`，构建目录 `~/lab-04`）
- 交付树：`/Users/lucien/workspace/self-study/projects/wt-04`（HEAD = `eacecfa`）
- 本文档是本次验证**唯一**在交付树里写入的文件；`driver/`、`tests/`、`user/` 源码 sha256 与验证前一致（见第七节）。
- 负控实验全部在隔离副本 `/Users/lucien/nc04-sandbox`（= 交付树 `driver/user/tests/dts/scripts` 的 `cp -a` 副本）
  中完成，构建目录 `~/lab-04nc`，与交付工作树隔离。**注**：任务书建议的 `/tmp/nc04` 在 Lima 虚拟机里不可见
  （`/tmp` 是 VM 本地 tmpfs，宿主 `/tmp` 没有挂进 VM），因此改用宿主 `$HOME` 下的独立目录，
  目的与约束（不改交付树源码）完全一致。

---

## 〇、结论摘要

| 项目 | 结论 |
|---|---|
| 阶段 04 测试 | **通过**：42/42 PASS，跑到 `[TEST:END]`，panic=0，WARNING=0（4 次独立运行一致） |
| DoD 第 2 条（本阶段全绿） | **通过** |
| DoD 第 1 条（无 error/warning） | **通过**（`13-vm-fast-cycle.sh` 编译输出无 error/warning） |
| DoD 第 3 条（smoke 回归） | **通过**（15/15）；02=19/19、01=16/16 通过 |
| DoD 第 4 条（panic=0，告警可解释） | **通过** |
| DoD 第 5 条（三份文档落盘且可复现） | **部分通过**：`docs/impl/04-…` 存在且结论与日志一致；`docs/kb/04-…` 形态未在本次范围内核对 |
| 阶段 03 回归 | **不稳定（flaky）**：4 次运行 1 次全绿、3 次出现 `shm_seq_odd_seen=0` FAIL（详见 S1） |
| 交付测试真实覆盖缺口 | **2 处已用负控证实**：`ring_capacity` 断言过宽（NC-A）、remove 清理路径零覆盖（NC-B） |

真实问题按严重度见第四节；负控结论见第三节。

---

## 一、独立复现的五项测试（第 4 项任务）

命令（全部用官方脚本，未自拼 QEMU）：

```bash
WORKTREE=04 bash 20-macos-gen-dtb.sh                     # 新 worktree 首次
limactl shell dev bash -c 'WORKTREE=04 bash …/13-vm-fast-cycle.sh'
LC_ALL=C WORKTREE=04 bash 21-macos-sync-artifacts.sh
LC_ALL=C WORKTREE=04 bash 22-macos-run-test.sh <阶段> <超时>
```

| 阶段 | 项数 | 结果 | 日志文件 | 备注 |
|---|---|---|---|---|
| 04-sysfs-debugfs | 42 | **PASS 42 / FAIL 0** | `logs/20260914-140902-04-04-sysfs-debugfs.log` | panic 0 / 告警 0 |
| 03-ringbuffer-mmap（第 1 次） | 40 | FAIL 1 | `logs/20260914-140913-04-03-ringbuffer-mmap.log` | `shm_seq_odd_seen=0` |
| 03-ringbuffer-mmap（第 2 次） | 40 | FAIL 3 | `logs/20260914-141027-04-03-ringbuffer-mmap.log` | 见 S1 |
| 03-ringbuffer-mmap（第 3 次） | 40 | **PASS 40 / FAIL 0** | `logs/20260914-141039-04-03-ringbuffer-mmap.log` | `odd_seen=164` |
| 03-ringbuffer-mmap（第 4 次） | 40 | FAIL 1 | `logs/20260914-141211-04-03-ringbuffer-mmap.log` | `shm_seq_odd_seen=0` |
| 02-i2c-driver | 19 | **PASS 19 / FAIL 0** | `logs/20260914-140925-04-02-i2c-driver.log` | |
| 01-io-models | 16 | **PASS 16 / FAIL 0** | `logs/20260914-140930-04-01-io-models.log` | |
| smoke | 15 | **PASS 15 / FAIL 0** | `logs/20260914-140941-04-smoke.log` | |

四项回归里 02/01/smoke 稳定通过；03 的 seqlock 放大器检查在本机 4 次里 3 次失败（详见 S1）。

---

## 二、检查项表格（42 项：名称 / 预期 / 实测 / 证据行号）

证据文件统一为 `logs/20260914-140902-04-04-sysfs-debugfs.log`（下文 `L<n>` = 该文件第 n 行）。

### 2.1 sysfs 属性齐全与可读（9 项）

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 1 | sysfs interval_ms | 路径存在 | 存在 | L273 |
| 2 | sysfs seq | 路径存在 | 存在 | L274 |
| 3 | sysfs i2c_errors | 路径存在 | 存在 | L275 |
| 4 | sysfs ring_capacity | 路径存在 | 存在 | L276 |
| 5 | interval_ms 是数字 | 纯十进制 | 500（dtb `poll-interval-ms=500`） | L279；值见 L278 |
| 6 | seq 是数字 | 纯十进制 | 0 | L280 |
| 7 | i2c_errors 是数字 | 纯十进制 | 0 | L281 |
| 8 | ring_capacity 是数字 | 纯十进制 | 85 | L282 |
| 9 | ring_capacity ≥ 64 | > 63 | 85（= kfifo 1536B 向上取整到 2048B / 24B） | L283、L278 |

debugfs `stats`/`ring` 里的 `ring_capacity=85`（L305、L325）与 sysfs 的 85 一致 —— 这是"同一份状态"的旁证。

### 2.2 写入生效与两入口一致性（2 项）

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 10 | sysfs 读回新值=100 | 写 100 后读出 100 | 100 | L289（值见 L288） |
| 11 | ioctl(GET_STATS) 读到同一值 | 与 sysfs 相同 | sysfs=100 / ioctl=100 | L290，L288 |

### 2.3 边界与非法值（7 项）

驱动侧单一判定/单一生效：`sensor_interval_valid()` `driver/sensor_char.c:756`、`sensor_apply_interval()` `:762`，
sysfs store 用 `:959/962`，ioctl 用 `:800/802` —— 与集成负责人裁定 1（下限统一 1ms）一致（代码核对）。

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 12 | 下界 1ms 被接受 | 值=1 | 1（dmesg `interval -> 1 ms`） | L293、L292 |
| 13 | 上界 60000ms 被接受 | 值=60000 | 60000 | L295、L294 |
| 14 | 0ms 被拒绝（值保持不变） | 仍为 60000 | 60000（未被改成 0） | L296 |
| 15 | 0ms 报错信息含 `nvalid` | stderr 有 EINVAL 线索 | `sh: write error: Invalid argument` | L297 |
| 16 | 60001ms 被拒绝 | 仍为 60000 | 60000 | L298 |
| 17 | 非数字被拒绝 | 仍为 60000 | 60000（未被当成 0） | L299 |
| 18 | （隐含）随后可写 20ms | 生效 | `interval -> 20 ms` | L300 |

### 2.4 debugfs 文件齐全与 stats 字段（10 项）

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 19 | debugfs stats | 存在 | 存在 | L302 |
| 20 | debugfs ring | 存在 | 存在 | L303 |
| 21 | debugfs regs | 存在 | 存在 | L304 |
| 22 | stats 含 `open=` | 有该字段 | `open=1 read=0 irq=3 i2c_err=0 interval=20 dropped=0 ring_count=3 ring_capacity=85 seq=3 shm_writes=3` | L306、L305 |
| 23 | stats 含 `irq=` | 有 | irq=3 | L307、L305 |
| 24 | stats 含 `i2c_err=` | 有 | i2c_err=0 | L308、L305 |
| 25 | stats 含 `dropped=` | 有 | dropped=0 | L309、L305 |
| 26 | stats 含 `ring_count=` | 有 | ring_count=3 | L310、L305 |
| 27 | stats 含 `seq=` | 有 | seq=3 | L311、L305 |
| 28 | stats 的 interval 与 sysfs 一致 | interval=20 | interval=20 | L312、L305 |

字段集合与契约（`docs/11-阶段任务书.md` 阶段 04 §2）完全对齐（多余的两个字段 `ring_capacity`/`shm_writes` 属附加信息，不违约）。

### 2.5 debugfs regs（6 项）

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 29 | 打印温度寄存器 00 | 有 `00: ` 行 | `00: 0189` | L317、L314 |
| 30 | 打印配置寄存器 02 | 有 `02: ` 行 | `02: 0001` | L318、L316 |
| 31 | 无 `read error` | 三行都是真值 | 00/01/02 三行均为 `%04x` | L319、L314~L316 |
| 32 | 温度 raw > 0x0100 | > 255 | 0x0189 = 393 → 24.56 ℃（芯片模拟 24.0~26.0 ℃，`driver/virt_i2c.c` 模拟区间） | L321、L320 |
| 33 | 温度 raw ≤ 0xFFFF | 16 位以内 | 393 | L322 |
| 34 | 配置寄存器 = 0x0001 | probe 写入的 CONT_EN | 0001 | L323、L316 |

corner 核对：0x0001 = 连续转换使能位，与 `probe` 的 `regmap_write(..., SENSOR_CFG_CONT_EN)` 一致；
湿度 `01: 0fa6` = 4006 → 40.06 %RH，与阶段 02 的模拟区间一致 —— 说明 regmap 通路与字节序（`REGMAP_ENDIAN_LITTLE`）都是活的。

### 2.6 debugfs ring（4 项）

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 35 | ring 含 `count=` | 有 | `count=6 dropped=0 ring_count=6 ring_capacity=85 shm_writes=6 recent=6` | L329、L325 |
| 36 | ring 含 `dropped=` | 有 | dropped=0 | L330、L325 |
| 37 | ring 含样本明细 `sample[` | 有 | `sample[0] seq=1 temp_milli=24062` | L331、L326 |
| 38 | 样本明细含 seq 与温度 | `temp_milli=` | 有（6 条，L326~L328） | L332 |

### 2.7 故障注入（3 项）

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 39 | 注入 3 次后 i2c_errors 增量 ≥ 3 | ≥3 | **0 → 3（增量 3，恰好等于注入数）** | L342、L341 |
| 40 | 注入结束后采样仍推进 | 序号增长 | 106 → 156 | L344、L343 |
| 41 | （子检查）注入日志链路 | regmap 报错 + 驱动计数 | `virt_i2c: fault injection armed: next 3 transfer(s) will fail` / `sensor_char 0-0048: regmap read failed: -5` | 见同 log 注入段落 |

### 2.8 注入后与告警（2 项）

| # | 检查项 | 预期 | 实测 | 证据 |
|---|---|---|---|---|
| 42 | sensor_test 退出码为 0 | 0 | 0 | L350 |
| 43 | 用户态测试正常结束 | 含"测试结束" | 有 | L351 |
| 44 | dmesg 无 WARNING/Call trace/BUG | 计数 0 | 0（测试体之后重新取样） | L353 |

（表内编号为叙述用序号；脚本内实际为 42 个 `check_*` 调用，与 `[TEST:END] pass=42` 一致，L356。）

---

## 三、负控实验（第 3 项任务）

全部在 `/Users/lucien/nc04-sandbox` 隔离副本 + `WORKTREE=04nc`（构建目录 `~/lab-04nc`）里做，
构建/同步/运行仍走 `13/21/22` 的副本（非自拼命令）。交付树源码 sha256 未变（第七节）。

| 编号 | 篡改内容（仅副本） | 运行 | 结果 | 日志 | 结论 |
|---|---|---|---|---|---|
| 基线 | 无（交付树原样） | `04-sysfs-debugfs` | PASS 42/42 | 交付 `logs/20260914-140902-04-04-sysfs-debugfs.log` | 参照组（工作树隔离，结论与副本构建一致） |
| 基线′ | 副本原样（仅自写探针） | `nc-remove-probe` | PASS 13/13，`STILL-PRESENT`/`already present` 出现 0 次 | `nc04-sandbox/logs/20260914-141232-04nc-nc-remove-probe.log` | 探针本身有效（不是恒 FAIL） |
| **NC-A** | `ring_capacity_show` 返回硬编码 `64u`（`driver/sensor_char.c` 的 `sensor_ring_capacity(sd)` → `64u`） | `04-sysfs-debugfs` | **PASS 42/42（没有 FAIL）** | `nc04-sandbox/logs/20260914-141252-04nc-04-sysfs-debugfs.log` | **真实覆盖缺口**：sysfs 显示 64 而 debugfs 同时显示 85，矛盾在日志里肉眼可见，却没有任何检查项抓住 |
| **NC-C** | 删掉 `interval_ms_store` 里的 `if (!sensor_interval_valid(ms)) return -EINVAL;`（`driver/sensor_char.c:959`） | `04-sysfs-debugfs` | **FAIL 4**：`0ms 被拒绝（值保持不变）` 期望 60000 实际 **0**；`0ms 报错信息含 nvalid` 未命中；`60001ms 被拒绝` 实际 **60001**；`非数字被拒绝` 实际**60001** | `nc04-sandbox/logs/20260914-141309-04nc-04-sysfs-debugfs.log` | 边界检查**有效**，且不是恒真：篡改后值真的被改成非法值并被逐条抓出 |
| **NC-B** | 删掉 `sensor_remove()` 里的 `debugfs_remove_recursive(sd->dbg);` + `sysfs_remove_group(...)`（`driver/sensor_char.c:1370-1371`） | 交付测试 `04-sysfs-debugfs` | **PASS 42/42（没有 FAIL）** | `nc04-sandbox/logs/20260914-141412-04nc-04-sysfs-debugfs.log` | **真实覆盖缺口**：04 测试体从不 rmmod（`tests/runner/init.sh` 只 insmod，末尾 `poweroff`）→ remove 路径完全不执行 |
| NC-B′ | 同上篡改 + 自写探针（3 轮 rmmod/insmod、检查 debugfs 残留/内核报错） | `nc-remove-probe` | 基线探针 **PASS 13/13**；加强探针后 `PASS 13/13`→**FAIL 4 + panic=1 + 告警=2** | 基线 `…-141232-04nc-nc-remove-probe.log`；篡改 `…-141356-04nc-nc-remove-probe.log` | 清理缺失**是可检出的**（残留目录 + `debugfs: Directory 'sensor_char' with parent '/' already present!` + 读残留 debugfs 文件触发内核 panic），只是交付测试没有去检 |

NC-A 的现场证据（同一份日志里自相矛盾）：

```
      · 读到的值：interval_ms=500 seq=0 i2c_errors=0 ring_capacity=64
[CHECK:PASS] ring_capacity 反映 kfifo 实际容量（64 >= 64）
    open=1 read=0 irq=3 i2c_err=0 interval=20 dropped=0 ring_count=3 ring_capacity=85 seq=3
```

NC-B 的现场证据（`…-141328-04nc-nc-remove-probe.log`，加强探针前的版本）：

```
[    0.750818] sensor_char 0-0048: removed
[    0.781126] debugfs: Directory 'sensor_char' with parent '/' already present!
[    0.781280] sensor_char 0-0048: debugfs unavailable: -17
    loop1-AFTER-RMMOD-DEBUGFS-STILL-PRESENT
```

加强探针后同一篡改变成 **FAIL 4 + Kernel panic**（读残留 debugfs 文件走了已释放的 `file->private_data`），
即"删掉清理 = 卸载后残留目录 + 复用失败 + 悬空 fops"，属于可检出的真实缺陷。

**负控结论**：三项负控里 **2 项确认了交付测试的真实覆盖缺口**（NC-A、NC-B），1 项确认检查有效（NC-C）。

---

## 四、真实问题（按严重度排序）

### S1（高，回归不稳定）阶段 03 的 `shm_seq_odd_seen > 0` 检查是 flaky 的

- 现象：`tests/phases/03-ringbuffer-mmap.sh:175` 断言放大器模式下读者见过奇数 seqlock 序号。
  本机 4 次运行：1 次 PASS（`odd_seen=164`），3 次 FAIL（`odd_seen=0`，其中 1 次连带 `mmap_retries=0`、
  用户态程序退出码 1，一次跑出 3 个 FAIL）。
- 证据：`logs/20260914-140913/141027/141211-04-03-ringbuffer-mmap.log`（FAIL）vs
  `logs/20260914-141039-04-03-ringbuffer-mmap.log`（PASS）；跨工作树历史日志里该 FAIL 出现在
  **18 / 83** 份 03 日志中（约 22%），说明是长期存在的间歇性检查，不只是本轮环境噪声。
- 判定：与阶段 04 改动无关（04 只新增 sysfs/debugfs 与 `interval` 抽取；放大器路径 `driver/sensor_char.c:319`
  的 `udelay` 未被改动）。根因是"读方必须恰好落在 1ms 奇数窗口内"的采样式断言，在宿主 CPU 争用
  （本机同时有多个工作树在跑 QEMU）下读者 vCPU 会被剥离该窗口。
- 建议（不属阶段 04 范围）：把断言改为"`retries>0` 或 `odd_seen>0` 二者至少一个"，或把窗口放大到 5ms 再重试。

### S2（中，测试真实覆盖缺口）`ring_capacity` 断言过宽 → 硬编码 64 不被发现

- `tests/phases/04-sysfs-debugfs.sh:50`（`check_gt … 63`）只判 `>63`。NC-A 实测：把 sysfs 返回值硬编码成 64，
  42 项全绿，而 debugfs 同时打印真实的 85。
- 影响：这条检查想守住的契约语义（"看的是 kfifo 向上取整后的真实容量，不是请求值 64"）**没有被守住**，
  正是 `docs/impl/04-…` 第二节 `ring_capacity` 那一段的立论。
- 建议改法：断言"sysfs 的 ring_capacity == debugfs/stats 里的 ring_capacity"（两入口/两接口一致性，
  和 04 测试里已有的 `interval=` 一致性检查同款手法），并保留 `>63`。

### S3（中，测试真实覆盖缺口）remove/清理路径零覆盖

- 04 测试只 insmod 一次，`tests/runner/init.sh` 结尾 `sync; poweroff -f`，从不 rmmod →
  `sensor_remove()`（`driver/sensor_char.c:1358-1371`）在 04 阶段测试里**一次都没执行**。
- NC-B 实测：删掉 `debugfs_remove_recursive` + `sysfs_remove_group` 后 04 仍是 42/42。
- 影响面：清理路径只能在阶段 03 的放大器步骤（`rmmod sensor_char && insmod … shm_publish_delay_us=1000`）
  被顺带执行，而那里的检查项不涉及 debugfs 残留，所以"漏清理"能穿过整套测试。
- 建议改法：在 04 测试末尾加"rmmod → 确认 `/sys/class/sensor_char/sensor0` 与 `/sys/kernel/debug/sensor_char`
  都消失 → 再 insmod → 两个入口都回来 + dmesg 无 `already present`"。

### S4（低，文档/代码注释不准确）`debugfs_create_dir()` 在未挂 debugfs 时的返回码写成了 `-ENODEV`

- `docs/impl/04-…` 第五节与 `driver/sensor_char.c:1282-1284` 的注释、`docs/11` 的裁定 2 都写 `ERR_PTR(-ENODEV)`。
- 实测 6.6.156 源码：`fs/debugfs/inode.c` 的 `start_creating()` 在 `!debugfs_initialized()` 时返回
  `ERR_PTR(-ENOENT)`（`-2`），并非 `-ENODEV`。驱动用 `IS_ERR()` 判断，**代码行为正确**，
  只是注释/文档里的 errno 说错了；裁定 2（非致命 + `debugfs_remove_recursive(NULL)` 安全）已正确落实（`:1286-1294`、`:1337`、`:1370`）。

### S5（低）`stats` 的 interval 一致性检查用子串匹配，可能误判

- `tests/phases/04-sysfs-debugfs.sh:78` 用 `check_contains "interval=20"`；子串匹配下 `interval=200`
  也会命中。当前实测值恰好是 20，结论不失真，但把周期写成 200 的回归无法被抓住。

### S6（低）交付测试未校验属性权限位与只读语义

- 契约要求 `interval_ms` RW、`seq`/`i2c_errors`/`ring_capacity` RO。测试只做存在/可读/可写，不校验 mode。
- 我在隔离副本的探针里补测（`nc-remove-probe` P1/P2）：`-rw-r--r--` / `-r--r--r--` / `-r--r--r--` / `-r--r--r--`，
  向 `seq`、`ring_capacity` 写入被拒绝 —— 基线 13/13 PASS，说明**实现符合契约，只是测试没覆盖**。

---

## 五、代码审查（第 2 项任务）

| 审查点 | 结论 | 证据（file:line） |
|---|---|---|
| 属性读写上下文与锁 | `interval_ms` 读/写都持 `sd->lock`（mutex，进程上下文，允许睡眠）；`seq`/`i2c_errors` 用 `READ_ONCE` 读 u32 计数器，无锁但不会撕裂；`ring_capacity` 调 `kfifo_size()` 只读常量容量，无需锁 | `:935-942`、`:959-963`、`:971-984`、`:991-996` |
| `interval_ms_store` 返回语义 | 成功 `return count`，解析失败返回 `kstrtoul` 的 errno，越界返回 `-EINVAL` | `:951-963` |
| 两入口共享状态 | ioctl 与 sysfs 共用 `sensor_interval_valid()`/`sensor_apply_interval()`；`GET_STATS` 的 `stats.interval_ms` 直接取 `sd->interval_ms`（与 sysfs show 同源） | `:753-766`、`:800-802`、`:959-962`、`:811` |
| 与阶段 02 故障注入配合 | `regmap_read` 失败 → `sd->i2c_errors++` + `dev_warn_ratelimited`，注入 3 次得增量恰好 3（无重复计数、无漏计） | `driver/sensor_char.c:356`、log L341-342 |
| remove 清理 | `sensor_remove()`：先 `debugfs_remove_recursive(sd->dbg)` + `sysfs_remove_group()`，再 `device_destroy()` — 顺序正确（kobject 销毁前摘属性，避免回调里碰到已释放的 sd） | `:1370-1372` |
| probe 错误路径清理 | `err_remove_sysfs` 标签同样先 `debugfs_remove_recursive` + `sysfs_remove_group`，再 `err_unreg_chrdev` 的 `device_destroy`/`cdev_del`/`unregister_chrdev_region`，之后释放 shm/kfifo | `:1335-1347` |
| `debugfs` 对 NULL 的健壮性 | `sd->dbg = debugfs_create_dir(...)`，`IS_ERR` → `dev_warn` + 置 NULL；三个 `debugfs_create_file` 只在成功分支调用；清理统一 `debugfs_remove_recursive(sd->dbg)`（NULL 安全）。**裁定 2 落实** | `:1286-1294`、`:1337`、`:1370` |
| `sd->dbg` 未初始化风险 | `sd` 是 `devm_kzalloc`，`sd->dbg` 初值 NULL；即使 `sysfs_create_group` 失败走 `err_remove_sysfs`，`debugfs_remove_recursive(NULL)` 也安全 | `:1183`、`:1275-1278`、`:1337` |
| 持有锁期间不拷贝到用户态 | `stats` 只在持 `lock` 时 `scnprintf`，锁外 `simple_read_from_buffer`；`ring` 只在持 `shm_lock`（spinlock）时拷数组，锁外格式化 | `:1030-1040`、`:1060-1078` |
| debugfs 一致性 | `ring` 读 `sd->kfifo_dropped`/`shm_writes` 未持 `sd->lock`，多字段非原子快照 —— 调试接口可接受，非缺陷 | `:1069-1072` |
| 已知薄弱点复核 1 | `ring_capacity` 断言只判 `>63`：**独立复核成立**，见 S2（NC-A） | 测试 `:50` |
| 已知薄弱点复核 2 | remove 清理路径无回归：**独立复核成立**，见 S3（NC-B） | `tests/runner/init.sh:79-88` |

---

## 六、契约偏差、恒真检查审查与遗留疑点

### 6.1 契约核对结果（第 1 项任务）

| 契约条目 | 实现 | 判定 |
|---|---|---|
| `interval_ms` RW，1~60000，非法 `-EINVAL` | `DEVICE_ATTR_RW`（0644），`kstrtoul`+`sensor_interval_valid` | 符合 |
| `seq` RO | `DEVICE_ATTR_RO`（0444），`READ_ONCE(sd->latest.seq)` | 符合 |
| `i2c_errors` RO | `DEVICE_ATTR_RO`（0444），`READ_ONCE(sd->i2c_errors)` | 符合 |
| `ring_capacity` RO | `DEVICE_ATTR_RO`（0444），`kfifo_size()/sizeof(sample)` | 符合 |
| 挂载点 `/sys/class/sensor_char/sensor0/` + `sysfs_create_group`/`sysfs_remove_group` | `:1275`、`:1337`、`:1370` | 符合 |
| debugfs `/sys/kernel/debug/sensor_char/` 三文件 `stats`/`ring`/`regs`，RO | `debugfs_create_dir` + 三个 `0444` file，`debugfs_remove_recursive` | 符合 |
| `stats` 字段格式 `open= read= irq= i2c_err= interval= dropped= ring_count= seq=` | 一行内全部存在（附加 `ring_capacity`/`shm_writes`） | 符合（L305） |
| `ring` ≥ count/dropped + 最近 8 条 `seq/temp` | `recent=6` + 6 条 `sample[i] seq=/temp_milli=`（上限 8） | 符合 |
| `regs` 实时读 0x00~0x02（regmap） | 三行真实值，失败时打印 `<read error N>` | 符合 |
| 故障注入后 `i2c_errors` 增长 | 增量恰为 3 | 符合 |
| 裁定 1（周期下限统一 1ms） | 抽出单一判定/生效，两入口共用（`:800-802` vs `:959-962`） | 已落实 |
| 裁定 2（debugfs 非致命） | `dev_warn` + NULL + NULL 安全清理 | 已落实 |

契约偏差：**无功能性偏差**；仅 S4 的文档 errno 描述（`-ENODEV` 应为 `-ENOENT`）与 S6 的测试覆盖（权限位未校验）。

### 6.2 恒真/无效检查审查（第 5 项任务）

逐条读了 42 项的判定表达式，结论：

- **"非法值被拒绝"不是恒真**：`check_eq "0ms 被拒绝（值保持不变）" "60000" "$(cat $SYS/interval_ms)"` 真的在看写后回读值，
  NC-C 证实它会把非法值抓住（实测变成 0 → FAIL）。配套的 `check_contains … "nvalid"` 看的是 stderr 文件内容，
  NC-C 下也一同 FAIL。两条一起判（值 + 报错）已经能区分"拒绝"与"内核接受但改了别的值"。
- **只有 1 项过宽**：`ring_capacity … > 63`（S2），NC-A 证明硬编码 64 能穿过。
- **1 项子串匹配偏松**：`interval=20`（S5）。
- 其余各项都在比较真实观测值（数字正则、check_eq/check_gt 的值、debugfs 内容、dmesg 计数），未发现恒真项。
- 未覆盖但契约要求的行为：属性权限位/只读语义（6.1 S6）、`regs` 的 `01:` 行（允许，契约只说 0x00~0x02，实际有）、
  ioctl→sysfs 方向的写回一致性（只测了 sysfs→ioctl）。

### 6.3 logs/ 交叉检查（第 6 项任务）

| 交叉项 | 实测 | 结论 |
|---|---|---|
| 属性实测值 | `interval_ms=500 seq=0 i2c_errors=0 ring_capacity=85`（L278） | 与 dtb `poll-interval-ms=500` 一致 |
| 两入口一致性 | 写 100 → sysfs=100、ioctl=100（L288）；stats `interval=20` 与 sysfs 同步（L305/L312） | 一致 |
| 故障注入前后 i2c_errors 增量 | 0 → 3，增量=注入数 3（L341）；恢复期序号 106→156（L343） | 错误路径被真实执行且能自恢复 |
| debugfs regs 内容 | `00: 0189`(393→24.56 ℃) / `01: 0fa6`(4006→40.06 %RH) / `02: 0001`(CONT_EN) | 与阶段 02 模拟区间、probe 写入位一致，字节序正确 |
| ring 与 stats 的容量 | 两处都 85（L305、L325） | 同一份状态 |

### 6.4 遗留疑点

1. 阶段 03 的 flaky 检查（S1）会让"回归全绿"这一条随运行次数波动；建议阶段 03 的负责人改成非采样式断言。
2. NC-B′ 里"读残留 debugfs 文件 → Kernel panic"是篡改驱动的后果（已隔离，未污染交付树）；
   它同时说明**交付驱动一旦漏清理，危害是悬空 fops 的 UAF**，而当前测试完全测不到。
3. 未验证 `debugfs` 未挂载时的真实路径（QEMU initramfs 里 `init.sh` 总是挂 debugfs）：
   裁定 2 的实现已按 6.6 API 语义走 `IS_ERR`→NULL 分支，但**没有实测证据**（可用 `debugfs=off` 内核参数做后续补测）。
4. 04 测试没有覆盖 `sysfs_create_group` 失败的错误路径（无法在正常环境触发），只做了静态审查。

---

## 七、DoD 五条逐条判定（`docs/10-开发与验证守则.md` 第五节）

| DoD | 判定 | 依据 |
|---|---|---|
| 1. `make` 无 error、无 warning | **通过** | `13-vm-fast-cycle.sh` 输出只有 `CC [M]`/`LD [M]` 与"生成: …"，无 error/warning 行 |
| 2. 本阶段测试全 PASS 且跑到 `[TEST:END]` | **通过** | 42/42，`[TEST:END] 04-sysfs-debugfs pass=42 fail=0`（L356） |
| 3. smoke 回归全 PASS | **通过** | smoke 15/15；另 02=19/19、01=16/16；但 03 的 `odd_seen` 检查 flaky（S1）——若按"整套回归必须全绿"的严格口径，**本项应记为有条件通过（03 存在既存 flaky 检查）** |
| 4. 日志 panic=0；WARNING/BUG/Call trace 有解释 | **通过** | 04 log `内核 panic: 0`、`内核告警: 0`；44 项告警检查 PASS（L353） |
| 5. 三份文档落盘且结论可被日志复现 | **部分通过** | `docs/impl/04-sysfs-debugfs-实现记录.md` 存在，其中 `ring_capacity=85`、`sysfs=100/ioctl=100`、`i2c_errors 0→3`、`00: 0188` 等关键结论均在本报告日志中找到对应（本次实测 `00: 0189`，属模拟值波动，量级与语义一致）；`docs/kb/04-…` 是否落盘未在本次核对范围（不在任务书要求我检查的文件列表内） |

---

## 八、交付文件 sha256 清单（验证后重新计算，确认未被本次验证改动）

```
bea15b3f193d3d6ee237930208a04b2894371f4f15c926498114d6b8af5c0eac  driver/sensor_char.c
f58775315fcafc8cee76f9a8db609809cf8003128eb95af0f58ba4bd6ba8394e  driver/virt_i2c.c
61321d47ada592c7e55c35a11fd95726664a00384fbfb1e5f51b0f938ef56aba  tests/phases/04-sysfs-debugfs.sh
cb6f8a6caf5bebf82b17208c1b2488db9ab8f7e7e5f2d8ada70d5a10bd8e5da4  tests/runner/lib.sh
1509d417f1a019cfbf00ef601377937876682d74ca33636e296dbde05c5b992e  logs/20260914-140902-04-04-sysfs-debugfs.log
```

- `git status --short` 为空（无修改、无新增跟踪文件）；`git diff --cached` 为空（**无 staged 文件**）。
- 负控副本 `/Users/lucien/nc04-sandbox` 在交付树之外；交付树内未被 `cp -a` 反向写入或修改。

## 九、验证环境

- macOS 宿主 + Lima VM `dev`（aarch64）；内核树 `~/kernel-build/linux-6.6.156`，`CONFIG_KCSAN` 未开启（构建前已确认），未修改内核树。
- 工作树 04：构建 `~/lab-04`，产物 `artifacts/`，日志 `logs/`；负控工作树 04nc：`~/lab-04nc`，产物/日志在 `/Users/lucien/nc04-sandbox/`。
- 全部测试通过 `scripts/13 → scripts/21 → scripts/22` 三段式执行，未自行拼接 QEMU 命令。
