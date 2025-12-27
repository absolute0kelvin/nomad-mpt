#!/bin/bash
# MonadDB Go 绑定构建脚本
#
# 这个脚本会：
# 1. 构建 Rust FFI crate（如果需要）
# 2. 编译 C API 包装层
# 3. 复制静态库到 Go 可访问的目录
# 4. 构建 Go 包
#
# 使用方法：
#   ./scripts/build.sh        # 完整构建
#   ./scripts/build.sh quick  # 跳过 Rust 构建，只复制库文件

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GO_BINDINGS_DIR="$(dirname "$SCRIPT_DIR")"
NOMAD_MPT_DIR="$(dirname "$(dirname "$GO_BINDINGS_DIR")")"
RUST_DIR="$NOMAD_MPT_DIR/bindings/rust"
LIB_DIR="$GO_BINDINGS_DIR/monaddb/lib"
CORE_DIR="$NOMAD_MPT_DIR/core"

echo "=== MonadDB Go 绑定构建 ==="
echo "Go 绑定目录: $GO_BINDINGS_DIR"
echo "Rust 目录:   $RUST_DIR"
echo "Core 目录:   $CORE_DIR"
echo "库输出目录:  $LIB_DIR"
echo ""

# 创建 lib 目录
mkdir -p "$LIB_DIR"

# 步骤 1: 构建 Rust crate（除非指定 quick 模式）
if [ "$1" != "quick" ]; then
    echo "[1/4] 构建 Rust FFI crate..."
    cd "$RUST_DIR"
    cargo build --release
    echo "     ✓ Rust 构建完成"
else
    echo "[1/4] 跳过 Rust 构建（quick 模式）"
fi

# 找到 Rust 构建输出目录
RUST_OUT_DIR=$(find "$RUST_DIR/target/release/build" -name "out" -path "*nomad-mpt-sys*" -type d | head -1)

if [ -z "$RUST_OUT_DIR" ]; then
    echo "     ✗ 错误: 找不到 Rust 构建输出目录"
    echo "     请先运行: cd $RUST_DIR && cargo build --release"
    exit 1
fi

# 步骤 2: 编译 C API 包装层
echo ""
echo "[2/4] 编译 C API 包装层..."

MONAD_DIR="$NOMAD_MPT_DIR/depend/monad"
THIRD_PARTY="$MONAD_DIR/third_party"

# 检查编译器
if command -v clang++-19 &> /dev/null; then
    CXX="clang++-19"
elif command -v clang++ &> /dev/null; then
    CXX="clang++"
else
    CXX="g++"
fi
echo "     使用编译器: $CXX"

# 编译 nomad_mpt.cpp
$CXX -c -std=c++23 -O2 \
    -I"$CORE_DIR/include" \
    -I"$MONAD_DIR" \
    -I"$MONAD_DIR/category" \
    -I"$THIRD_PARTY" \
    -I"$THIRD_PARTY/intx/include" \
    -I"$THIRD_PARTY/evmc/include" \
    -I"$THIRD_PARTY/ethash/include" \
    -I"$THIRD_PARTY/nlohmann_json/single_include" \
    -I"$THIRD_PARTY/immer" \
    -I"$THIRD_PARTY/quill/quill/include" \
    -I"$THIRD_PARTY/cthash/include" \
    -I"$THIRD_PARTY/ankerl" \
    -I"$THIRD_PARTY/concurrentqueue" \
    -I"$THIRD_PARTY/unordered_dense/include" \
    "$CORE_DIR/src/nomad_mpt.cpp" \
    -o "$LIB_DIR/nomad_mpt.o"

# 创建静态库
ar rcs "$LIB_DIR/libnomad_mpt.a" "$LIB_DIR/nomad_mpt.o"
rm "$LIB_DIR/nomad_mpt.o"
echo "     ✓ libnomad_mpt.a 编译完成"

# 步骤 3: 复制静态库
echo ""
echo "[3/4] 复制静态库..."
echo "     Rust 输出目录: $RUST_OUT_DIR"

# 库文件列表（按依赖顺序）
declare -A LIBS=(
    # 核心库
    ["monad_ffi"]="$RUST_OUT_DIR/build/libmonad_ffi.a"
    
    # 第三方库
    ["quill"]="$RUST_OUT_DIR/build/third_party/quill/quill/libquill.a"
    ["blake3"]="$RUST_OUT_DIR/build/third_party/BLAKE3/c/libblake3.a"
    ["keccak"]="$RUST_OUT_DIR/build/third_party/ethash/lib/keccak/libkeccak.a"
)

for name in "${!LIBS[@]}"; do
    src="${LIBS[$name]}"
    dst="$LIB_DIR/lib${name}.a"
    
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        echo "     ✓ $name"
    else
        echo "     ⚠ $name (源文件不存在: $src)"
    fi
done

# 复制头文件
echo ""
echo "     复制头文件..."
cp "$NOMAD_MPT_DIR/core/include/nomad_mpt.h" "$GO_BINDINGS_DIR/monaddb/" 2>/dev/null || true
echo "     ✓ nomad_mpt.h"

# 步骤 4: 构建 Go 包
echo ""
echo "[4/4] 构建 Go 包..."
cd "$GO_BINDINGS_DIR"

# 设置 CGO 环境
export CGO_ENABLED=1

# 尝试构建
if go build ./...; then
    echo "     ✓ Go 包构建成功"
else
    echo "     ✗ Go 包构建失败"
    echo ""
    echo "可能的原因："
    echo "  1. 缺少系统库，请安装："
    echo "     sudo apt install liburing-dev libgmp-dev libzstd-dev libarchive-dev"
    echo "     sudo apt install libboost-fiber-dev libboost-context-dev"
    echo "  2. Rust 构建输出目录结构变化，检查 RUST_OUT_DIR"
    exit 1
fi

echo ""
echo "=== 构建完成 ==="
echo ""
echo "使用方法："
echo "  cd $GO_BINDINGS_DIR"
echo "  go run examples/basic/main.go"

