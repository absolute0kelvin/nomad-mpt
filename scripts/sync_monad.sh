#!/bin/bash
# sync_monad.sh - 从 monad 原始代码同步并应用 ARM64 patch
#
# 用法:
#   MONAD_SRC=../monad-0.12.5 ./scripts/sync_monad.sh
#
# 这个脚本会:
#   1. 从 MONAD_SRC 复制必要的文件到 depend/monad
#   2. 应用 ARM64 相关的 patch (Keccak + OpenSSL 汇编)
#   3. 应用修改后的 CMakeLists.txt 文件

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DEST="$PROJECT_ROOT/nomad-mpt-sys/depend/monad"
PATCHES="$PROJECT_ROOT/patches"
BACKUP="$PROJECT_ROOT/backup"

# 默认源目录
MONAD_SRC="${MONAD_SRC:-$PROJECT_ROOT/../monad-0.12.5}"

# 检查源目录
if [[ ! -d "$MONAD_SRC/category" ]]; then
    echo "错误: 找不到 monad 源代码目录: $MONAD_SRC"
    echo "请设置 MONAD_SRC 环境变量指向 monad 源代码目录"
    exit 1
fi

echo "=== MonadDB FFI 同步脚本 ==="
echo "源目录: $MONAD_SRC"
echo "目标目录: $DEST"
echo ""

# 1. 清理目标目录
echo "[1/6] 清理目标目录..."
rm -rf "$DEST/category" "$DEST/cmake" "$DEST/CMakeLists.txt"
rm -rf "$DEST/third_party"

# 2. 复制必要文件
echo "[2/6] 从源目录复制文件..."
mkdir -p "$DEST/category"
mkdir -p "$DEST/third_party"

# 复制核心模块
cp -r "$MONAD_SRC/category/core" "$DEST/category/"
cp -r "$MONAD_SRC/category/mpt" "$DEST/category/"
cp -r "$MONAD_SRC/category/async" "$DEST/category/"

# 复制构建配置
cp -r "$MONAD_SRC/cmake" "$DEST/"
cp "$MONAD_SRC/CMakeLists.txt" "$DEST/"

# 复制第三方依赖（只复制 MPT 需要的库）
# MPT 需要的库：
#   - BLAKE3: 哈希函数
#   - intx: uint128/256/512 大整数类型
#   - unordered_dense: 高性能哈希表
#   - ethash: 提供 keccak 哈希
#   - quill: 日志库
#   - komihash: 哈希函数
#   - evmc: Ethereum VM 接口
#   - ankerl: robin_hood.h 哈希表
#   - openssl: Keccak1600 汇编
#   - cthash: 哈希函数
#   - concurrentqueue: 并发队列
#
# 不复制的库（MPT 不需要）：
#   - asmjit: JIT 汇编生成器（EVM 执行）
#   - blst: BLS 签名（共识）
#   - c-kzg-4844: KZG 承诺（EIP-4844 blob）
#   - c-kzg-4844-builder: KZG 构建工具
#   - execution-specs: 以太坊规范文档
#   - go-ethereum: Go 以太坊参考
#   - immer: 不可变数据结构
#   - nanobench: 基准测试
#   - nlohmann_json: JSON 库
#   - silkpre: 预编译合约
#   - yellowpaper: 以太坊黄皮书

REQUIRED_LIBS="ankerl BLAKE3 concurrentqueue cthash ethash evmc intx komihash openssl quill unordered_dense"

for lib in $REQUIRED_LIBS; do
    if [[ -d "$MONAD_SRC/third_party/$lib" ]]; then
        cp -r "$MONAD_SRC/third_party/$lib" "$DEST/third_party/"
        echo "   ✓ $lib"
    else
        echo "   ⚠ $lib (源目录不存在)"
    fi
done

echo "   复制完成: $(find "$DEST" -type f | wc -l) 个文件"

# 3. 应用 ARM64 核心文件 (从 backup 复制)
echo "[3/6] 应用 ARM64 核心修改..."

if [[ -d "$BACKUP" ]]; then
    cp "$BACKUP/arm_cpu_detect.c" "$DEST/category/core/" 2>/dev/null && echo "   ✓ arm_cpu_detect.c"
    cp "$BACKUP/keccak.c" "$DEST/category/core/" 2>/dev/null && echo "   ✓ keccak.c"
    cp "$BACKUP/keccak_impl.S" "$DEST/category/core/" 2>/dev/null && echo "   ✓ keccak_impl.S"
    cp "$BACKUP/cpu_relax.h" "$DEST/category/core/" 2>/dev/null && echo "   ✓ cpu_relax.h"
else
    echo "   警告: 备份目录不存在，尝试应用 patch..."
    if [[ -f "$PATCHES/arm64-keccak.patch" ]]; then
        cd "$DEST"
        patch -p2 < "$PATCHES/arm64-keccak.patch" || echo "   警告: patch 应用失败"
    fi
fi

# 4. 添加 OpenSSL ARM 汇编文件
echo "[4/6] 添加 OpenSSL ARM64 汇编文件..."

OPENSSL_ASM="$DEST/third_party/openssl/crypto/sha/asm"
mkdir -p "$OPENSSL_ASM"

# 应用 patch（从 $DEST 目录，使用 -p1 来正确处理路径）
if [[ -f "$PATCHES/openssl-arm64.patch" ]]; then
    cd "$DEST"
    # patch 文件路径格式是 third_party/openssl/...，所以从 $DEST 用 -p0
    patch -p0 < "$PATCHES/openssl-arm64.patch" 2>/dev/null && echo "   ✓ OpenSSL ARM64 汇编文件" || {
        echo "   警告: openssl-arm64.patch 应用失败，尝试检查已有文件..."
    }
fi

# 5. 应用修改后的 CMakeLists.txt 文件
echo "[5/6] 应用修改后的 CMakeLists.txt 文件..."

if [[ -d "$BACKUP" ]]; then
    cp "$BACKUP/root_CMakeLists.txt" "$DEST/CMakeLists.txt" 2>/dev/null && echo "   ✓ CMakeLists.txt (根)"
    cp "$BACKUP/core_CMakeLists.txt" "$DEST/category/core/CMakeLists.txt" 2>/dev/null && echo "   ✓ category/core/CMakeLists.txt"
    cp "$BACKUP/mpt_CMakeLists.txt" "$DEST/category/mpt/CMakeLists.txt" 2>/dev/null && echo "   ✓ category/mpt/CMakeLists.txt"
    cp "$BACKUP/async_CMakeLists.txt" "$DEST/category/async/CMakeLists.txt" 2>/dev/null && echo "   ✓ category/async/CMakeLists.txt"
    cp "$BACKUP/async_util.cpp" "$DEST/category/async/util.cpp" 2>/dev/null && echo "   ✓ category/async/util.cpp"
else
    echo "   警告: 备份目录不存在，跳过 CMakeLists.txt 更新"
fi

# 6. 验证关键文件存在
echo ""
echo "[6/6] 验证结果..."
MISSING=0
for f in \
    "category/core/arm_cpu_detect.c" \
    "category/core/keccak_impl.S" \
    "category/core/keccak.c" \
    "category/core/cpu_relax.h" \
    "third_party/openssl/crypto/sha/asm/keccak1600-armv8.S" \
    "third_party/openssl/crypto/sha/asm/arm_arch.h" \
    "CMakeLists.txt" \
    "category/core/CMakeLists.txt" \
    "category/mpt/CMakeLists.txt"
do
    if [[ -f "$DEST/$f" ]]; then
        echo "  ✅ $f"
    else
        echo "  ❌ $f (缺失)"
        MISSING=$((MISSING + 1))
    fi
done

if [[ $MISSING -eq 0 ]]; then
    echo ""
    echo "✅ 同步完成！所有必要文件已就位。"
    echo ""
    echo "下一步: 运行构建"
    echo "  cd nomad-mpt/nomad-mpt-sys/depend/monad/build"
    echo "  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19"
    echo "  make -j\$(nproc)"
else
    echo ""
    echo "⚠️  同步完成，但有 $MISSING 个文件缺失。请检查 patch 和 backup 文件。"
    exit 1
fi
