#!/usr/bin/env bash
# ============================================================================
# 11-vm-prepare-pristine.sh - 【在 Lima Linux 虚拟机内执行】
#                             重建干净内核源码树 + 打兼容补丁 + 生成内核配置
#
# 为什么需要这个脚本？
#   之前的 kernel-src/ 目录被污染了两处，导致编译失败：
#     (1) 树里混入了 macOS 编译产物（Mach-O 可执行文件 scripts/kconfig/conf 等），
#         拷进 Linux 后会被当 shell 脚本执行 → "Syntax error"。
#     (2) scripts/mod/file2alias.c 被人为改过（uuid_t → k_uuid_t），
#         与官方 tarball 不一致 → 宿主工具编译报 "unknown type name 'uuid_t'"。
#   因此不再信任已有目录，直接从官方 tarball 重新解出，保证源码纯净可复现。
#
#   另外 Ubuntu 26.04 (glibc 2.43) 的 <sys/stat.h> 会间接包含
#   /usr/include/linux/stat.h → <linux/types.h>，该调用在内核宿主工具编译时
#   会被解析到 tools/include/linux/types.h，从而与 scripts/mod/list.h 里的
#   struct list_head 定义冲突（重复定义报错）。内核 6.6 发布时 glibc 还没这么新，
#   所以需要打一个 3 行的兼容补丁（见步骤 [2/5]）。
#
# 用法（macOS 侧）：
#   limactl shell dev bash <项目路径>/scripts/11-vm-prepare-pristine.sh
# ============================================================================
set -euo pipefail

PROJ=${PROJ:-/Users/lucien/workspace/self-study/projects/linux-char-driver}
KVER=${KVER:-6.6.156}
TARBALL="$PROJ/kernel-src/linux-$KVER.tar.xz"
KSRC="$HOME/kernel-build/linux-$KVER"

# gcc-13：内核 6.6 LTS 官方支持的编译器上限（Ubuntu 26.04 默认 gcc 15 过新）
if command -v gcc-13 >/dev/null 2>&1; then
    export CC=gcc-13 HOSTCC=gcc-13
fi

echo "==================== [1/5] 从官方 tarball 重新解出干净源码 ===================="
[ -f "$TARBALL" ] || { echo "缺少 tarball: $TARBALL"; exit 1; }
rm -rf "$KSRC"
mkdir -p "$HOME/kernel-build"
echo "  解压 $TARBALL ..."
tar -xf "$TARBALL" -C "$HOME/kernel-build"
echo "  完成：$KSRC"

echo "==================== [2/5] 打 glibc 兼容补丁 ===================="
mkdir -p "$HOME/patches"
PATCHFILE="$HOME/patches/0001-host-tools-newer-glibc.patch"
: > "$PATCHFILE"

python3 - "$KSRC" <<'PY'
import sys, os
ksrc = sys.argv[1]
guard = "__LIST_HEAD_STRUCT_DEFINED__"
old = "struct list_head {\n\tstruct list_head *next, *prev;\n};"
new = ("#ifndef %s\n#define %s\n" % (guard, guard)) + old + "\n#endif"
for rel in ["tools/include/linux/types.h", "scripts/mod/list.h"]:
    p = os.path.join(ksrc, rel)
    t = open(p, encoding="utf-8").read()
    if guard in t:
        print("  已打过补丁，跳过:", rel)
        continue
    assert old in t, "未找到待替换片段: " + rel
    open(p, "w", encoding="utf-8").write(t.replace(old, new, 1))
    print("  已打补丁:", rel)
PY

echo "  生成补丁工件用于文档复现：$PATCHFILE"
for rel in tools/include/linux/types.h scripts/mod/list.h; do
    diff -u --label "a/$rel" --label "b/$rel" \
        <(tar -xOf "$TARBALL" "linux-$KVER/$rel") "$KSRC/$rel" >> "$PATCHFILE" || true
done

echo "==================== [3/5] 生成内核配置 ===================="
cd "$KSRC"
make defconfig >/dev/null
./scripts/config \
    -e DEVTMPFS -e DEVTMPFS_MOUNT \
    -e DEBUG_INFO -e DEBUG_KERNEL -e KALLSYMS -e KALLSYMS_ALL \
    -e FTRACE -e FUNCTION_TRACER -e FUNCTION_GRAPH_TRACER -e DYNAMIC_FTRACE \
    -e MAGIC_SYSRQ -e DEBUG_FS \
    -m I2C_STUB -e I2C_CHARDEV -e I2C \
    -e VIRTIO_BLK -e PCI_HOST_GENERIC \
    -e IIO -e IIO_BUFFER -e IIO_TRIGGER -e IIO_TRIGGERED_BUFFER -e IIO_KFIFO_BUF \
    -e IIO_SW_TRIGGER -e IIO_CONFIGFS -e IIO_HRTIMER_TRIGGER \
    -e CONFIGFS_FS -e REGMAP -e REGMAP_I2C \
    -e KUNIT -e PM_ADVANCED_DEBUG
make olddefconfig >/dev/null
for s in DEVTMPFS I2C_STUB DEBUG_FS FTRACE IIO IIO_TRIGGERED_BUFFER IIO_KFIFO_BUF \
         IIO_SW_TRIGGER IIO_CONFIGFS IIO_HRTIMER_TRIGGER CONFIGFS_FS \
         REGMAP REGMAP_I2C KUNIT PM_ADVANCED_DEBUG; do
    printf '  %-24s %s\n' "$s" "$(grep -E "^CONFIG_$s=|^# CONFIG_$s is not set" .config | head -1 | sed 's/# //')"
done

echo "==================== [4/5] 验证宿主工具链可编译（make prepare）===================="
echo "  编译器: ${CC:-gcc} / ${HOSTCC:-gcc}"
make -j"$(nproc)" prepare
echo "  make prepare 通过 ✔"

echo "==================== [5/5] 完成 ===================="
echo "  源码树: $KSRC"
echo "  补丁  : $PATCHFILE"
echo "  下一步: limactl shell dev bash <项目路径>/scripts/12-vm-build-image.sh （全量编译）"
