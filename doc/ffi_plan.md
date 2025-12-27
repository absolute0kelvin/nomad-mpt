# MonadDB Rust FFI 开发规划 (FFI Plan)

## 目标

建立一套完整的 Rust 绑定 (`nomad-mpt`)，使得 Rust 开发者能够以近乎零开销的方式直接调用 Monad 的 MPT 引擎，并保持与以太坊状态规范的 100% 兼容。

---

## 当前状态 ✅

**最后更新**: 2025-12-26

| 阶段 | 状态 | 说明 |
|------|------|------|
| 阶段零 (ARM64 支持) | ✅ 完成 | `clang-19` 构建成功，NEON + SHA3 支持 |
| 阶段一 (基础构建) | ✅ 完成 | 同步脚本 + `cargo build` 通过 |
| 阶段二 (C++ 内部适配) | ✅ 完成 | 预设 StateMachine，无需 VTable |
| 阶段三 (同步 API) | ✅ 完成 | cxx 桥接实现 (内存模式 + 磁盘模式) |
| 阶段四 (异步 ck_fifo) | ✅ 完成 | 高并发 find 与 traverse |
| 阶段五 (Go 绑定) | ✅ 完成 | CGO 绑定，同步 API 完整支持 |

### 已知限制

| 功能 | 状态 | 说明 |
|------|------|------|
| `db_open_disk_ro` | ❌ 未实现 | 只读磁盘模式需要 RODb 支持 |
| `db_clear` | ❌ 已移除 | 请使用 CLI 工具 |
| `traverse` | ✅ 已实现 | 异步遍历子树，结果通过 Traverse FIFO 返回 |
| Go FIFO API | 🔬 实验性 | C API 层为 stub，Rust 绑定有完整实现 |
| Key 长度 | 推荐 32 字节 | 以太坊标准，EthereumStateMachine 支持批量更新 |

### 目录结构

```
nomad-mpt/
├── bindings/                    # 语言绑定
│   ├── rust/                    # Rust 绑定 (cxx)
│   │   ├── nomad-mpt-sys/       # FFI crate
│   │   │   ├── src/
│   │   │   │   ├── lib.rs       # cxx bridge 定义
│   │   │   │   ├── bridge.cpp   # 同步 API C++ 实现
│   │   │   │   ├── bridge_fifo.cpp  # 异步 API C++ 实现
│   │   │   │   └── ...
│   │   │   ├── examples/
│   │   │   └── tests/
│   │   └── Cargo.toml           # workspace
│   │
│   └── go/                      # Go 绑定 (已完成)
│       ├── monaddb/             # CGO 包
│       │   ├── db.go            # 数据库操作
│       │   ├── node.go          # 节点操作
│       │   ├── fifo.go          # 异步 FIFO (实验性)
│       │   └── lib/             # 静态库
│       ├── examples/            # 示例代码
│       └── scripts/build.sh     # 构建脚本
│
├── core/                        # 纯 C API (用于 Go/Java/C#)
│   ├── include/
│   │   └── nomad_mpt.h          # 稳定 C 头文件
│   └── src/
│       └── nomad_mpt.cpp        # C API 实现
│
├── depend/                      # 依赖库
│   ├── monad/                   # MonadDB C++ 源码
│   └── ck/                      # Concurrency Kit
│
├── backup/                      # 备份文件
├── patches/                     # 补丁
└── scripts/                     # 构建脚本
```

**设计理念**:
- `core/` - 提供稳定的纯 C API，供非 Rust 语言使用（CGO/JNI/P-Invoke）
- `bindings/rust/` - Rust 专用的 cxx 绑定，类型安全、零开销
- `bindings/go/` - Go 绑定（使用 core/ 的 C API 通过 CGO）
- 未来可添加 `bindings/java/`、`bindings/csharp/` 等

### 构建验证

```bash
# ARM64 Linux C++ 库构建
cd nomad-mpt/depend/monad/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
make -j$(nproc)

# Rust FFI crate 构建
cd nomad-mpt/bindings/rust
cargo build --release

# 运行示例
cargo run --release --example basic

# 运行测试
cargo test --release

# 生成:
# - libmonad_ffi.a (3.7 MB, 5804 symbols)
# - libmonad_core.a (包含 Keccak256 ARM64 NEON + SHA3 实现)
```

---

## 重要说明

### 平台与架构支持

| 平台 | 架构 | 状态 | 说明 |
|------|------|------|------|
| Linux | x86_64 | ✅ 完整支持 | AVX2/AVX512 + io_uring |
| Linux | ARM64 | ✅ 完整支持 | NEON + ARMv8.2 SHA3 + io_uring |

**核心依赖**：
- `io_uring`（Linux 5.1+）— 异步 I/O
- `libhugetlbfs`（可选）— 大页内存优化
- Keccak256 汇编优化 — 平台相关

### MonadDB 的定位

MonadDB 是一个 **Authenticated Key-Value Store**，既提供：
- **KV 存储功能**：Value 直接存储在 MPT 叶子节点中
- **Merkle 认证功能**：每个节点维护哈希，可生成 State Root 和 Merkle Proof

```
find(key) → node.value()  // 获取存储的数据
find(key) → node.data()   // 获取 Merkle hash（用于 proof）
```

---

## 阶段零：ARM64 架构支持 (ARM64 Porting) 🆕

**目标**：让 MonadDB 能在 Linux ARM64 平台上编译和运行。

> **优先级说明**：ARM 支持是 FFI 开发的前置条件。建议按以下顺序：
> 1. 先完成 ARM NEON 基础支持（让代码能编译运行）
> 2. 再加入 ARMv8.2 SHA3 硬件加速（性能优化）
> 3. 最后做 FFI 绑定（功能完整后再暴露接口）

### 0.1 现状分析

当前 Keccak256 实现仅支持 x86_64：

```asm
// category/core/keccak_impl.S（当前）
#if defined(__x86_64__)
    #if defined(__AVX512F__)
        #include <crypto/sha/asm/keccak1600-avx512.S>
    #elif defined(__AVX2__)
        #include <crypto/sha/asm/keccak1600-avx2.S>
    #else
        #error avx2 or avx512 required
    #endif
#else
    #error unsupported arch  // ← ARM 会在这里失败
#endif
```

### 0.2 获取 OpenSSL ARM Keccak 实现

参考 [OpenSSL ARMv8.2 SHA3 支持](https://github.com/openssl/openssl/pull/21398)，在 Apple M1 上可获得约 **36% 性能提升**。

```bash
# 从 OpenSSL 仓库获取 ARM64 实现
cd third_party/openssl/crypto/sha/asm/

# 下载 ARM64 汇编生成器
curl -O https://raw.githubusercontent.com/openssl/openssl/master/crypto/sha/asm/keccak1600-armv8.pl

# 生成 Linux ARM64 汇编
perl keccak1600-armv8.pl linux64 keccak1600-armv8.S

```

### 0.3 修改 keccak_impl.S 支持多架构

```asm
// category/core/keccak_impl.S（修改后）

#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

// ============ x86_64: AVX2 or AVX512 ============
#if defined(__x86_64__)
    #if defined(__AVX512F__)
        #include <crypto/sha/asm/keccak1600-avx512.S>
    #elif defined(__AVX2__)
        #include <crypto/sha/asm/keccak1600-avx2.S>
    #else
        #error x86_64 requires AVX2 or AVX512
    #endif

// ============ ARM64: NEON + optional SHA3 extension ============
#elif defined(__aarch64__) || defined(__arm64__)
    // Linux ARM64 - ELF 格式
    #include <crypto/sha/asm/keccak1600-armv8.S>

#else
    #error unsupported architecture (supported: x86_64, aarch64)
#endif
```

### 0.4 ARMv8.2 SHA3 运行时检测

OpenSSL 的汇编代码内部已经支持两种模式：
- **NEON baseline**：所有 ARM64 CPU
- **ARMv8.2 SHA3 扩展**：Apple M1/M2/M3/M4, AWS Graviton 3+

**推荐方案**：直接检测 SHA3 特性，而非依赖 CPU 型号白名单。这样可以自动支持未来的新芯片。

> **已验证**：在 Apple M4 上通过 Linux `/proc/cpuinfo` 确认 `sha3` 特性存在。

```c
// category/core/arm_cpu_detect.c

#include <stdint.h>

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)

// 全局标志，启动时初始化，被 OpenSSL 汇编代码引用
unsigned int OPENSSL_armcap_P = 0;

// ⚠️ 重要：位定义必须与 OpenSSL arm_arch.h 完全一致！
#define ARMV7_NEON      (1 << 0)
#define ARMV8_AES       (1 << 2)
#define ARMV8_SHA1      (1 << 3)
#define ARMV8_SHA256    (1 << 4)
#define ARMV8_PMULL     (1 << 5)
#define ARMV8_SHA512    (1 << 6)
#define ARMV8_SHA3      (1 << 11)  // ARMv8.2 SHA3 扩展 - 注意是 bit 11！

// ============ Linux: 直接检测 SHA3 特性 ============
#if defined(__linux__)
#include <sys/auxv.h>

#ifndef HWCAP_SHA3
#define HWCAP_SHA3 (1 << 17)
#endif

__attribute__((constructor))
static void monad_detect_arm_features(void) {
    OPENSSL_armcap_P |= ARMV7_NEON;  // 所有 ARM64 都有 NEON
    
    unsigned long hwcap = getauxval(AT_HWCAP);
    if (hwcap & HWCAP_SHA3) {
        OPENSSL_armcap_P |= ARMV8_SHA3;
    }
    // ... 其他特性检测
}

#endif // __linux__

#endif // __aarch64__
```

### 0.4.1 运行时函数选择

检测到 SHA3 支持后，需要在 `keccak.c` 中实现运行时选择：

```c
// category/core/keccak.c (关键片段)

// 基础 NEON 实现
extern size_t SHA3_absorb(uint64_t A[5][5], unsigned char const *inp, size_t len, size_t r);
extern void SHA3_squeeze(uint64_t A[5][5], unsigned char *out, size_t len, size_t r, int next);

// ARMv8.2 SHA3 硬件扩展实现 (更快)
extern size_t SHA3_absorb_cext(uint64_t A[5][5], unsigned char const *inp, size_t len, size_t r);
extern void SHA3_squeeze_cext(uint64_t A[5][5], unsigned char *out, size_t len, size_t r, int next);

extern unsigned int OPENSSL_armcap_P;
#define ARMV8_SHA3 (1 << 11)

// 运行时选择：有 SHA3 硬件就用 _cext 版本
static inline size_t sha3_absorb(uint64_t A[5][5], unsigned char const *inp, size_t len, size_t r) {
    if (OPENSSL_armcap_P & ARMV8_SHA3) {
        return SHA3_absorb_cext(A, inp, len, r);
    }
    return SHA3_absorb(A, inp, len, r);
}
```


### 0.5 CMakeLists.txt 修改

```cmake
# category/core/CMakeLists.txt 中添加

# 架构检测
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(MONAD_ARCH "x86_64")
    # 检测 AVX2/AVX512
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx512f" HAVE_AVX512)
    check_cxx_compiler_flag("-mavx2" HAVE_AVX2)
    
    if(HAVE_AVX512)
        add_compile_definitions(MONAD_KECCAK_AVX512)
    elseif(HAVE_AVX2)
        add_compile_definitions(MONAD_KECCAK_AVX2)
    else()
        message(FATAL_ERROR "x86_64 requires AVX2 or AVX512 support")
    endif()
    
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(MONAD_ARCH "aarch64")
    add_compile_definitions(MONAD_KECCAK_ARM64)
    
    # ARM CPU 特性检测
    target_sources(monad_core PRIVATE arm_cpu_detect.c)
    
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

message(STATUS "Building for architecture: ${MONAD_ARCH}")
```

### 0.6 预期性能

根据 [OpenSSL 测试数据](https://github.com/openssl/openssl/pull/21398)：

| 平台 | CPU | 实现 | Keccak256 吞吐量 |
|------|-----|------|-----------------|
| x86_64 | Intel Xeon | AVX512 | ~2.5 GB/s |
| x86_64 | Intel Core | AVX2 | ~1.8 GB/s |
| ARM64 | Apple M1 | NEON | ~1.2 GB/s |
| ARM64 | Apple M1 | **SHA3 扩展** | **~1.6 GB/s (+36%)** |
| ARM64 | Apple M4 | **SHA3 扩展** | **~2.0 GB/s (预估)** |
| ARM64 | Graviton 3 | SHA3 扩展 | ~1.4 GB/s |

### 0.7 测试验证

```bash
# 在 ARM64 机器上编译
mkdir build-arm64 && cd build-arm64
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 运行 Keccak 单元测试
./category/core/test/keccak_test

# 运行 MPT 测试确保 state root 正确
./category/mpt/test/merkle_trie_test
```

---

## 阶段一：基础构建与环境对齐 (Foundation)

**目标**：解决交叉编译和链接问题，确保 `cargo build` 能正确产出。

### 1.1 代码组织

**问题**：当前 `depend/monad` 下复制了整个代码库，维护负担大。

**方案**：从 `monad-0.12.5` 原始代码复制必要文件，然后应用 ARM64 patch。

**复制范围**（仅需要 MPT 和核心依赖）：
```bash
# 从 monad-0.12.5 复制必要文件到 depend/monad
cp -r monad-0.12.5/category/{core,mpt,async} depend/monad/category/
cp -r monad-0.12.5/third_party depend/monad/
cp monad-0.12.5/CMakeLists.txt depend/monad/
cp -r monad-0.12.5/cmake depend/monad/
```

**ARM64 Patch 清单**：
| 类型 | 文件 | 说明 |
|------|------|------|
| 新增 | `category/core/arm_cpu_detect.c` | CPU 特性检测 |
| 修改 | `category/core/keccak_impl.S` | 多架构汇编包装 |
| 修改 | `category/core/keccak.c` | 运行时 SHA3 选择 |
| 修改 | `category/core/CMakeLists.txt` | 编译 ARM 文件 |
| 新增 | `third_party/openssl/.../arm_arch.h` | ARM 能力位定义 |
| 新增 | `third_party/openssl/.../keccak1600-armv8.S` | ARM64 汇编 |
| 新增 | `third_party/openssl/.../keccak1600-armv8.pl` | 汇编生成器 |

**自动化脚本**：
```bash
#!/bin/bash
# scripts/sync_monad.sh - 从原始代码同步并应用 patch

MONAD_SRC="${MONAD_SRC:-../monad-0.12.5}"
DEST="depend/monad"

# 1. 清理旧文件
rm -rf "$DEST"/{category,third_party,cmake,CMakeLists.txt}

# 2. 复制必要目录
cp -r "$MONAD_SRC"/category/{core,mpt,async} "$DEST/category/"
cp -r "$MONAD_SRC"/third_party "$DEST/"
cp -r "$MONAD_SRC"/cmake "$DEST/"
cp "$MONAD_SRC"/CMakeLists.txt "$DEST/"

# 3. 应用 ARM64 patch
patch -d "$DEST" -p1 < patches/arm64-keccak.patch

echo "Synced from $MONAD_SRC and applied ARM64 patches"
```

### 1.2 依赖库探测

在 `build.rs` 中集成依赖检测：

```rust
fn main() {
    // 平台检测
    #[cfg(not(target_os = "linux"))]
    compile_error!("nomad-mpt-sys only supports Linux");
    
    // 使用 pkg-config 探测系统库
    pkg_config::probe_library("liburing").expect("liburing not found");
    pkg_config::probe_library("tbb").expect("TBB not found");
    
    // 可选：hugetlbfs（CI 环境可能没有）
    let has_hugetlbfs = pkg_config::probe_library("hugetlbfs").is_ok();
    if has_hugetlbfs {
        println!("cargo:rustc-cfg=feature=\"hugetlbfs\"");
    }
    
    // CMake 构建
    let dst = cmake::Config::new(&cpp_source_dir)
        .define("MONAD_COMPILER_LLVM", "OFF")
        .define("MONAD_COMPILER_TESTING", "OFF")
        .build();
    
    // 链接库
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!("cargo:rustc-link-lib=static=monad_trie");
    println!("cargo:rustc-link-lib=static=monad_core");
    println!("cargo:rustc-link-lib=static=monad_async");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-lib=dylib=uring");
}
```

### 1.3 架构适配 ✅

根据宿主机架构自动切换汇编优化实现：

```rust
// build.rs 中的实际实现
let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
match target_arch.as_str() {
    "x86_64" => {
        // x86_64: AVX2/AVX512 汇编优化
        println!("cargo:rustc-cfg=keccak_asm_x86");
    }
    "aarch64" => {
        // ARM64: NEON + ARMv8.2 SHA3 汇编优化
        println!("cargo:rustc-cfg=keccak_asm_arm64");
    }
    _ => {
        // 其他架构: 使用 portable C 实现
        println!("cargo:rustc-cfg=keccak_portable");
    }
}
```

**两种架构都使用汇编优化**：

| 架构 | 实现 | 特性 |
|------|------|------|
| x86_64 | `keccak1600-avx2.S` / `keccak1600-avx512.S` | AVX2/AVX512 SIMD |
| ARM64 | `keccak1600-armv8.S` | NEON + ARMv8.2 SHA3 硬件加速 |

---

## 阶段二：C++ 内部适配（设计说明）

> **注意**：本阶段描述的是 C++ Bridge 内部实现细节，**不暴露给 FFI 接口**。
> Rust 用户无需了解这些内容。

### 设计决策

经过分析，以下复杂 C++ 特性在 FFI 层被隐藏：

| 内部类型 | FFI 处理方式 | Rust 用户看到的 |
|---------|-------------|----------------|
| `boost::intrusive::slist<Update>` | C++ 内部转换 | `&[RawUpdate]` 扁平数组 |
| `StateMachine` 虚基类 | 提供预设实现 | 无需关心，使用默认以太坊配置 |
| `std::shared_ptr<Node>` | opaque 包装 | `NodeHandle` 不透明类型 |
| `Compute` 哈希策略 | 预设 Keccak256 | 无需关心 |

### StateMachine 预设实现

为标准以太坊用例提供预设的 `EthereumStateMachine`：

```cpp
// bridge.cpp 内部实现

class EthereumStateMachine final : public mpt::StateMachine {
    static constexpr auto prefix_len = 2;    // 以太坊账户 trie
    static constexpr auto cache_depth = 8;   // 缓存前 8 层
    size_t depth{0};
    
public:
    void down(unsigned char) override { ++depth; }
    void up(size_t n) override { depth -= n; }
    
    Compute &get_compute() const override {
        static MerkleCompute m{};  // Keccak256 Merkle 哈希
        return m;
    }
    
    bool cache() const override { return depth < cache_depth; }
    bool compact() const override { return true; }
    bool is_variable_length() const override { return false; }
    
    std::unique_ptr<StateMachine> clone() const override {
        return std::make_unique<EthereumStateMachine>(*this);
    }
};
```

**扩展性**：如果未来需要自定义 StateMachine（如 Poseidon 哈希），可以添加 VTable 回调支持。

---

## 阶段三：同步 API 绑定 (Sync API)

**目标**：使用 `cxx` 实现类型安全的同步 FFI 接口。

### 3.1 cxx Bridge 定义

```rust
// src/lib.rs

#[cxx::bridge(namespace = "monad::ffi")]
pub mod ffi {
    // 共享类型
    #[derive(Debug)]
    struct RawUpdate {
        key: *const u8,
        key_len: usize,
        value: *const u8,
        value_len: usize,
        version: i64,
        nested_updates: *const RawUpdate,
        nested_count: usize,
    }
    
    unsafe extern "C++" {
        include!("nomad-mpt-sys/src/bridge.hpp");
        
        // Opaque 类型
        type DbHandle;
        type NodeHandle;
        type UpdateBuilder;
        
        // Db 生命周期
        fn db_open_rw(config_json: &str) -> Result<UniquePtr<DbHandle>>;
        fn db_open_ro(config_json: &str) -> Result<UniquePtr<DbHandle>>;
        fn db_close(db: UniquePtr<DbHandle>);
        
        // 同步读写
        fn db_find(db: &DbHandle, key: &[u8], version: u64) -> Result<UniquePtr<NodeHandle>>;
        fn db_upsert(
            db: Pin<&mut DbHandle>,
            root: &NodeHandle,
            updates: &[RawUpdate],
            version: u64,
        ) -> Result<UniquePtr<NodeHandle>>;
        
        // 元数据
        fn db_get_latest_version(db: &DbHandle) -> u64;
        fn db_get_earliest_version(db: &DbHandle) -> u64;
        fn db_load_root_for_version(db: &DbHandle, version: u64) -> Result<UniquePtr<NodeHandle>>;
        fn db_get_history_length(db: &DbHandle) -> u64;
        
        // Finalized 版本管理（仅磁盘模式）
        fn db_update_finalized_version(db: Pin<&mut DbHandle>, version: u64) -> Result<()>;
        fn db_get_finalized_version(db: &DbHandle) -> u64;
        
        // Rollback & Prune（仅磁盘模式）
        fn db_rewind_to_version(db: Pin<&mut DbHandle>, version: u64) -> Result<()>;
        fn db_version_is_valid(db: &DbHandle, version: u64) -> bool;
        fn db_clear(db: Pin<&mut DbHandle>) -> Result<()>;
        
        // Node 操作
        fn node_clone(node: &NodeHandle) -> UniquePtr<NodeHandle>;
        fn node_has_value(node: &NodeHandle) -> bool;
        fn node_value_len(node: &NodeHandle) -> usize;
        fn node_copy_value(node: &NodeHandle, out: &mut [u8]) -> usize;
        fn node_data_len(node: &NodeHandle) -> usize;
        fn node_copy_data(node: &NodeHandle, out: &mut [u8]) -> usize;
        
        // Merkle 根哈希
        fn node_compute_root_hash(node: &NodeHandle, out: &mut [u8]) -> usize;
        
        // 性能优化
        fn db_prefetch(db: Pin<&mut DbHandle>, root: &NodeHandle) -> usize;
        fn db_is_read_only(db: &DbHandle) -> bool;
        unsafe fn db_get_stats(
            db: &DbHandle,
            latest_version: &mut u64,
            earliest_version: &mut u64,
            history_length: &mut u64,
            is_on_disk: &mut bool,
            is_read_only: &mut bool,
            finalized_version: &mut u64,
        );
    }
}
```

### 3.2 高级 Rust 封装

```rust
// src/db.rs

use crate::ffi;

pub struct Db {
    inner: cxx::UniquePtr<ffi::DbHandle>,
}

impl Db {
    pub fn open_rw(config: &DbConfig) -> Result<Self, Error> {
        let config_json = serde_json::to_string(config)?;
        let inner = ffi::db_open_rw(&config_json)?;
        Ok(Self { inner })
    }
    
    pub fn find(&self, key: &[u8], version: u64) -> Result<Option<Vec<u8>>, Error> {
        match ffi::db_find(&self.inner, key, version) {
            Ok(node) => {
                if ffi::node_has_value(&node) {
                    let len = ffi::node_value_len(&node);
                    let mut buf = vec![0u8; len];
                    ffi::node_copy_value(&node, &mut buf);
                    Ok(Some(buf))
                } else {
                    Ok(None)
                }
            }
            Err(e) if is_not_found(&e) => Ok(None),
            Err(e) => Err(e.into()),
        }
    }
    
    pub fn upsert(&mut self, root: &Node, updates: &[Update], version: u64) -> Result<Node, Error> {
        let raw_updates = updates.iter().map(|u| u.to_raw()).collect::<Vec<_>>();
        let new_root = ffi::db_upsert(
            self.inner.pin_mut(),
            &root.inner,
            &raw_updates,
            version,
        )?;
        Ok(Node { inner: new_root })
    }
}
```

### 3.3 Rollback & Prune API

MonadDB 支持版本回滚和历史数据修剪：

#### History Length (Prune 机制)

```
版本号:    0    1    2    3    ...   997   998   999   1000
           │    │    │    │           │     │     │     │
           │<── 被 prune 的旧版本 ──>│<─── history_length=1000 ───>│
           │                         │                              │
           └─────────────────────────┴──────────────────────────────┘
                                     ▲                              ▲
                              earliest_version              latest_version
```

当写入新版本且历史版本数超过 `history_length` 时，最旧的版本及其 **独占的节点** 会被自动回收。

#### FFI 接口

```cpp
// 获取历史相关信息
uint64_t db_get_history_length(const DbHandle& db);
uint64_t db_get_earliest_version(const DbHandle& db);
uint64_t db_get_latest_version(const DbHandle& db);
bool db_version_is_valid(const DbHandle& db, uint64_t version);

// Finalized 版本管理（仅磁盘模式）
void db_update_finalized_version(DbHandle& db, uint64_t version);
uint64_t db_get_finalized_version(const DbHandle& db);

// 回滚/Prune（仅磁盘模式）
void db_rewind_to_version(DbHandle& db, uint64_t version);
void db_clear(DbHandle& db);

// Merkle 根哈希
size_t node_compute_root_hash(const NodeHandle& node, rust::Slice<uint8_t> out);

// 性能优化
size_t db_prefetch(DbHandle& db, const NodeHandle& root);
bool db_is_read_only(const DbHandle& db);
void db_get_stats(
    const DbHandle& db,
    uint64_t& latest_version,
    uint64_t& earliest_version,
    uint64_t& history_length,
    bool& is_on_disk,
    bool& is_read_only,
    uint64_t& finalized_version
);
```

#### Rust API

```rust
impl Db {
    /// 获取历史保留长度
    pub fn history_length(&self) -> u64;
    
    /// 检查版本是否有效（在历史范围内）
    pub fn version_is_valid(&self, version: u64) -> bool;
    
    /// 更新 finalized 版本，触发 prune
    /// 当版本数超过 history_length 时，旧数据会被清理
    pub fn update_finalized_version(&mut self, version: u64) -> Result<()>;
    
    /// 获取 finalized 版本
    pub fn finalized_version(&self) -> u64;
    
    /// 回滚到指定版本（使用 CLI 工具实现完整 rollback）
    pub fn rewind_to_version(&mut self, version: u64) -> Result<()>;
}

impl Node {
    /// 计算节点的 Merkle 根哈希（32 字节 Keccak256）
    pub fn root_hash(&self) -> [u8; 32];
}

impl Db {
    /// 预加载节点到缓存（仅 RW 磁盘模式）
    pub fn prefetch(&mut self, root: &Node) -> usize;
    
    /// 检查数据库是否只读
    pub fn is_read_only(&self) -> bool;
    
    /// 获取数据库统计信息
    pub fn stats(&self) -> DbStats;
}

/// 数据库统计信息
#[derive(Debug, Clone, Copy)]
pub struct DbStats {
    pub latest_version: u64,
    pub earliest_version: u64,
    pub history_length: u64,
    pub is_on_disk: bool,
    pub is_read_only: bool,
    pub finalized_version: u64,
}
```

#### 完整 Rollback

FFI 提供的 `db_rewind_to_version` 调用 `update_finalized_version`，仅触发 prune。

要执行 **完整的 rollback**（丢弃指定版本之后的所有数据），使用 CLI 工具：

```bash
# 回滚到版本 12345
monad_mpt --rewind-to 12345 /path/to/database

# 重置历史长度（触发更激进的 prune）
monad_mpt --reset-history-length 1000 /path/to/database
```

#### 配置 history_length

```rust
// 打开数据库时配置
let db = Db::open(DbConfig::disk("/path/to/db")
    .with_history_length(10000)  // 保留 10000 个历史版本
)?;
```

---

## 阶段四：异步 API - ck_fifo 模型 (可选)

**目标**：使用 [Concurrency Kit](https://concurrencykit.org/) 的 `ck_fifo` 实现高并发异步接口。

### 4.1 为什么选择 ck_fifo

| 特性 | 自定义 Ring Buffer | ck_fifo |
|------|-------------------|---------|
| **成熟度** | 需要自己实现 | 久经考验 (BSD 许可) |
| **容量** | 固定大小（有界） | 无界（动态增长） |
| **背压** | 队列满时阻塞/失败 | 永不满 |
| **节点复用** | 不适用 | ✅ 支持 |
| **架构支持** | 需要手写内存屏障 | aarch64, x86_64 原生优化 |
| **SPSC 优化** | 需要实现 | ✅ `ck_fifo_spsc` |

**适用场景**：
- `find`：高频查询，无界队列避免阻塞
- `traverse`：产生大量结果，无界队列不会溢出

### 4.2 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                           Rust Side                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   // 提交请求（非阻塞）                                              │
│   let id = fifo.submit_find(key, version);                          │
│   let id = fifo.submit_traverse(prefix, version, limit);            │
│                                                                      │
│   // 轮询结果                                                        │
│   while let Some(result) = fifo.poll() { handle(result); }          │
│                                                                      │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
          ▼                ▼                ▼
   ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
   │ ck_fifo_spsc│  │ ck_fifo_spsc│  │ ck_fifo_spsc│
   │ Request SQ  │  │ Completion  │  │ Large Value │
   │ Rust → C++  │  │ CQ C++ → R  │  │ Pool        │
   └──────┬──────┘  └──────▲──────┘  └──────▲──────┘
          │                │                │
┌─────────▼────────────────┴────────────────┴─────────────────────────┐
│                           C++ Worker(s)                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   while (running) {                                                  │
│       if (ck_fifo_spsc_dequeue(&sq, &req)) {                        │
│           switch (req.type) {                                        │
│               case FIND:     process_find(req);     break;          │
│               case TRAVERSE: process_traverse(req); break;          │
│           }                                                          │
│       }                                                              │
│       ck_pr_stall();  // CPU-friendly 等待                           │
│   }                                                                  │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

**三队列设计**：
1. **Request SQ** (Rust → C++)：请求提交队列
2. **Completion CQ** (C++ → Rust)：完成通知队列（小值内联）
3. **Large Value Pool** (C++ → Rust)：大值传递队列（>256 字节）

### 4.3 C++ 数据结构

```cpp
// bridge_fifo.hpp
// 使用 Concurrency Kit (https://github.com/concurrencykit/ck)

#pragma once
#include <ck_fifo.h>
#include <ck_pr.h>
#include <atomic>
#include <thread>
#include <vector>

namespace monad::ffi {

// ============ 请求类型 ============
enum RequestType : uint8_t {
    REQ_FIND_VALUE = 1,     // 获取 value
    REQ_FIND_NODE = 2,      // 获取 Node（含 Merkle hash）
    REQ_TRAVERSE = 3,       // 遍历子树
    REQ_SHUTDOWN = 255,     // 关闭 worker
};

// ============ 请求结构 ============
struct Request {
    uint64_t id;            // 请求 ID
    RequestType type;       // 请求类型
    uint64_t version;       // block_id / version
    uint8_t key[32];        // key (bytes, not nibbles)
    uint8_t key_len;        // key 长度
    uint32_t traverse_limit;// traverse 最大返回数量
};

// FIFO 节点（ck_fifo 要求）
struct RequestNode {
    ck_fifo_mpmc_entry_t entry;  // ck_fifo 内部链接 (MPMC 为 24 字节)
    Request req;
};

// ============ 响应状态 ============
enum ResultStatus : uint8_t {
    STATUS_OK = 0,
    STATUS_NOT_FOUND = 1,
    STATUS_ERROR = 2,
    STATUS_TRAVERSE_MORE = 3,   // traverse 还有更多结果
    STATUS_TRAVERSE_END = 4,    // traverse 结束
};

// ============ 完成结构 ============
struct Completion {
    uint64_t id;            // 对应的请求 ID
    ResultStatus status;    // 结果状态
    uint32_t value_len;     // value 长度（0xFFFFFFFF 表示大值）
    uint8_t value[256];     // 内联小值
    uint8_t merkle_hash[32];// node.data()
};

struct CompletionNode {
    ck_fifo_mpmc_entry_t entry;
    Completion comp;
};

// ============ 大值节点 ============
struct LargeValueNode {
    ck_fifo_spsc_entry_t entry;
    uint64_t request_id;
    uint32_t len;
    uint8_t data[];  // 柔性数组
};

// ============ FIFO 管理器 ============
class FifoManager {
public:
    FifoManager(mpt::Db& db);
    ~FifoManager();
    
    // 启动/停止 worker
    void start(size_t num_workers = 1);
    void stop();
    
    // === Rust 侧调用 ===
    RequestNode* alloc_request();
    void free_request(RequestNode* node);
    void submit(RequestNode* node);
    
    CompletionNode* poll_completion();
    void free_completion(CompletionNode* node);
    
    LargeValueNode* poll_large_value();
    void free_large_value(LargeValueNode* node);
    
private:
    void worker_loop(size_t id);
    void process_find(const Request& req);
    void process_traverse(const Request& req);
    void post_completion(Completion&& comp);
    void post_large_value(uint64_t req_id, const uint8_t* data, size_t len);
    
    mpt::Db& db_;
    
    // 四个 MPMC FIFO（无界、lock-free）
    ck_fifo_mpmc_t request_fifo_;     // 多线程 Rust → 多 Fiber C++
    ck_fifo_mpmc_t completion_fifo_;  // 多 Fiber C++ → Rust (Find 小值)
    ck_fifo_mpmc_t traverse_fifo_;    // 多 Fiber C++ → Rust (Traverse 结果)
    ck_fifo_mpmc_t large_value_fifo_; // 多 Fiber C++ → Rust (大值)
    
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
};

// ============ FFI 接口 ============
extern "C" {
    FifoManager* fifo_create(DbHandle* db);
    void fifo_destroy(FifoManager* mgr);
    void fifo_start(FifoManager* mgr, size_t num_workers);
    void fifo_stop(FifoManager* mgr);
    
    RequestNode* fifo_alloc_request(FifoManager* mgr);
    void fifo_free_request(FifoManager* mgr, RequestNode* node);
    void fifo_submit(FifoManager* mgr, RequestNode* node);
    
    CompletionNode* fifo_poll_completion(FifoManager* mgr);
    void fifo_free_completion(FifoManager* mgr, CompletionNode* node);
    
    LargeValueNode* fifo_poll_large_value(FifoManager* mgr);
    void fifo_free_large_value(FifoManager* mgr, LargeValueNode* node);
}

} // namespace monad::ffi
```

### 4.4 C++ Worker 实现

```cpp
// bridge_fifo.cpp

#include "bridge_fifo.hpp"
#include <category/mpt/db.hpp>
#include <cstdlib>
#include <cstring>

namespace monad::ffi {

FifoManager::FifoManager(mpt::Db& db) : db_(db) {
    // 初始化三个 FIFO（每个需要一个 stub 节点）
    ck_fifo_spsc_init(&request_fifo_, malloc(sizeof(ck_fifo_spsc_entry_t)));
    ck_fifo_spsc_init(&completion_fifo_, malloc(sizeof(ck_fifo_spsc_entry_t)));
    ck_fifo_spsc_init(&large_value_fifo_, malloc(sizeof(ck_fifo_spsc_entry_t)));
}

FifoManager::~FifoManager() {
    stop();
    // 清理 FIFO 中残留的节点...
}

void FifoManager::start(size_t num_workers) {
    running_.store(true, std::memory_order_release);
    for (size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
    }
}

void FifoManager::stop() {
    running_.store(false, std::memory_order_release);
    // 提交 shutdown 请求唤醒所有 worker
    for (size_t i = 0; i < workers_.size(); ++i) {
        auto* node = alloc_request();
        node->req.type = REQ_SHUTDOWN;
        submit(node);
    }
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void FifoManager::worker_loop(size_t id) {
    while (running_.load(std::memory_order_acquire)) {
        ck_fifo_spsc_entry_t* entry;
        
        if (ck_fifo_spsc_dequeue(&request_fifo_, &entry)) {
            auto* node = reinterpret_cast<RequestNode*>(
                reinterpret_cast<char*>(entry) - offsetof(RequestNode, entry)
            );
            
            switch (node->req.type) {
                case REQ_FIND_VALUE:
                case REQ_FIND_NODE:
                    process_find(node->req);
                    break;
                case REQ_TRAVERSE:
                    process_traverse(node->req);
                    break;
                case REQ_SHUTDOWN:
                    free_request(node);
                    return;
            }
            free_request(node);
        } else {
            // 队列空，使用 ck_pr_stall() 让出 CPU
            ck_pr_stall();
        }
    }
}

void FifoManager::process_find(const Request& req) {
    NibblesView key(req.key, req.key_len * 2);
    auto result = db_.find(key, req.version);
    
    Completion comp{};
    comp.id = req.id;
    
    if (result.has_error() || !result.value().node) {
        comp.status = STATUS_NOT_FOUND;
    } else {
        auto& cursor = result.value();
        if (!cursor.node->has_value()) {
            comp.status = STATUS_NOT_FOUND;
        } else {
            comp.status = STATUS_OK;
            auto value = cursor.node->value();
            
            if (value.size() <= 256) {
                comp.value_len = value.size();
                std::memcpy(comp.value, value.data(), value.size());
            } else {
                // 大值通过 large_value_fifo 传递
                comp.value_len = 0xFFFFFFFF;  // 标记为大值
                post_large_value(req.id, value.data(), value.size());
            }
            
            // 如果是 FIND_NODE，复制 Merkle hash
            if (req.type == REQ_FIND_NODE) {
                auto data = cursor.node->data();
                if (data.size() == 32) {
                    std::memcpy(comp.merkle_hash, data.data(), 32);
                }
            }
        }
    }
    
    post_completion(std::move(comp));
}

void FifoManager::post_completion(Completion&& comp) {
    auto* node = static_cast<CompletionNode*>(malloc(sizeof(CompletionNode)));
    node->comp = std::move(comp);
    ck_fifo_spsc_enqueue(&completion_fifo_, &node->entry, node);
}

void FifoManager::post_large_value(uint64_t req_id, const uint8_t* data, size_t len) {
    auto* node = static_cast<LargeValueNode*>(malloc(sizeof(LargeValueNode) + len));
    node->request_id = req_id;
    node->len = len;
    std::memcpy(node->data, data, len);
    ck_fifo_spsc_enqueue(&large_value_fifo_, &node->entry, node);
}

// FFI 实现
extern "C" {
    FifoManager* fifo_create(DbHandle* db) {
        return db ? new FifoManager(db->get()) : nullptr;
    }
    void fifo_destroy(FifoManager* mgr) { delete mgr; }
    void fifo_start(FifoManager* mgr, size_t n) { if (mgr) mgr->start(n); }
    void fifo_stop(FifoManager* mgr) { if (mgr) mgr->stop(); }
    // ... 其他 FFI 函数 ...
}

} // namespace monad::ffi
```

### 4.5 Rust 侧接口

```rust
// src/async_fifo.rs

use std::ptr::NonNull;
use std::sync::atomic::{AtomicU64, Ordering};

// FFI 类型声明
#[repr(C)]
pub struct RequestNode { /* ... */ }

#[repr(C)]
pub struct CompletionNode {
    _entry: [u8; 16],
    pub id: u64,
    pub status: u8,
    pub value_len: u32,
    pub value: [u8; 256],
    pub merkle_hash: [u8; 32],
}

#[repr(C)]
pub struct LargeValueNode {
    _entry: [u8; 16],
    pub request_id: u64,
    pub len: u32,
    // data follows (flexible array)
}

extern "C" {
    fn fifo_create(db: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
    fn fifo_destroy(mgr: *mut std::ffi::c_void);
    fn fifo_start(mgr: *mut std::ffi::c_void, num_workers: usize);
    fn fifo_stop(mgr: *mut std::ffi::c_void);
    fn fifo_alloc_request(mgr: *mut std::ffi::c_void) -> *mut RequestNode;
    fn fifo_free_request(mgr: *mut std::ffi::c_void, node: *mut RequestNode);
    fn fifo_submit(mgr: *mut std::ffi::c_void, node: *mut RequestNode);
    fn fifo_poll_completion(mgr: *mut std::ffi::c_void) -> *mut CompletionNode;
    fn fifo_free_completion(mgr: *mut std::ffi::c_void, node: *mut CompletionNode);
    fn fifo_poll_large_value(mgr: *mut std::ffi::c_void) -> *mut LargeValueNode;
    fn fifo_free_large_value(mgr: *mut std::ffi::c_void, node: *mut LargeValueNode);
}

/// 异步 FIFO 通道
pub struct AsyncFifo {
    mgr: NonNull<std::ffi::c_void>,
    next_id: AtomicU64,
}

unsafe impl Send for AsyncFifo {}
unsafe impl Sync for AsyncFifo {}

impl AsyncFifo {
    /// 从 Db 创建异步 FIFO
    pub fn new(db: &mut crate::Db) -> Result<Self, String> {
        unsafe {
            let mgr = fifo_create(db.as_raw_ptr());
            if mgr.is_null() {
                return Err("Failed to create FifoManager".into());
            }
            Ok(Self {
                mgr: NonNull::new_unchecked(mgr),
                next_id: AtomicU64::new(1),
            })
        }
    }
    
    /// 启动 Worker 线程
    pub fn start(&self, num_workers: usize) {
        unsafe { fifo_start(self.mgr.as_ptr(), num_workers); }
    }
    
    /// 提交 find 请求（非阻塞，永不失败）
    pub fn submit_find(&self, key: &[u8], version: u64) -> u64 {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        unsafe {
            let node = fifo_alloc_request(self.mgr.as_ptr());
            // 填充 node->req...
            fifo_submit(self.mgr.as_ptr(), node);
        }
        id
    }
    
    /// 提交 traverse 请求
    pub fn submit_traverse(&self, prefix: &[u8], version: u64, limit: u32) -> u64 {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        unsafe {
            let node = fifo_alloc_request(self.mgr.as_ptr());
            // 填充 node->req (type = REQ_TRAVERSE)...
            fifo_submit(self.mgr.as_ptr(), node);
        }
        id
    }
    
    /// 轮询完成结果（非阻塞）
    pub fn poll(&self) -> Option<FindResult> {
        unsafe {
            let node = fifo_poll_completion(self.mgr.as_ptr());
            if node.is_null() { return None; }
            
            let result = FindResult {
                id: (*node).id,
                status: (*node).status,
                value: if (*node).value_len > 0 && (*node).value_len != 0xFFFFFFFF {
                    Some((*node).value[..(*node).value_len as usize].to_vec())
                } else {
                    None
                },
                has_large_value: (*node).value_len == 0xFFFFFFFF,
                merkle_hash: (*node).merkle_hash,
            };
            fifo_free_completion(self.mgr.as_ptr(), node);
            Some(result)
        }
    }
    
    /// 轮询大值（用于 >256 字节的值）
    pub fn poll_large_value(&self) -> Option<LargeValue> {
        unsafe {
            let node = fifo_poll_large_value(self.mgr.as_ptr());
            if node.is_null() { return None; }
            
            let data = std::slice::from_raw_parts(
                (node as *const u8).add(std::mem::size_of::<LargeValueNode>()),
                (*node).len as usize
            );
            let result = LargeValue {
                request_id: (*node).request_id,
                data: data.to_vec(),
            };
            fifo_free_large_value(self.mgr.as_ptr(), node);
            Some(result)
        }
    }
    
    /// 批量轮询
    pub fn poll_batch(&self, out: &mut Vec<FindResult>, max: usize) -> usize {
        let mut count = 0;
        while count < max {
            if let Some(result) = self.poll() {
                out.push(result);
                count += 1;
            } else {
                break;
            }
        }
        count
    }
}

impl Drop for AsyncFifo {
    fn drop(&mut self) {
        unsafe {
            fifo_stop(self.mgr.as_ptr());
            fifo_destroy(self.mgr.as_ptr());
        }
    }
}

#[derive(Debug)]
pub struct FindResult {
    pub id: u64,
    pub status: u8,
    pub value: Option<Vec<u8>>,
    pub has_large_value: bool,
    pub merkle_hash: [u8; 32],
}

#[derive(Debug)]
pub struct LargeValue {
    pub request_id: u64,
    pub data: Vec<u8>,
}
```

### 4.6 批量操作接口

批量操作可显著减少 FFI 调用开销和内存屏障次数。

#### C++ 批量 FFI 接口

```cpp
// bridge_fifo.hpp - 批量操作接口

extern "C" {
    // ============ 单个操作 ============
    RequestNode* fifo_alloc_request(FifoManager* mgr);
    void fifo_submit(FifoManager* mgr, RequestNode* node);
    CompletionNode* fifo_poll(FifoManager* mgr);
    void fifo_free_completion(FifoManager* mgr, CompletionNode* node);
    
    // ============ 批量操作 ============
    
    /// 批量分配请求节点
    size_t fifo_alloc_request_batch(
        FifoManager* mgr,
        RequestNode** out,
        size_t count
    );
    
    /// 批量提交请求（单次内存屏障）
    void fifo_submit_batch(
        FifoManager* mgr,
        RequestNode** nodes,
        size_t count
    );
    
    /// 批量轮询完成（非阻塞）
    size_t fifo_poll_batch(
        FifoManager* mgr,
        CompletionNode** out,
        size_t max_count
    );
    
    /// 批量释放完成节点
    void fifo_free_completion_batch(
        FifoManager* mgr,
        CompletionNode** nodes,
        size_t count
    );
}
```

#### 批量操作性能对比

| 操作 | 单个调用 (64次) | 批量 (64个) | 提升 |
|------|----------------|-------------|------|
| **FFI 开销** | 64 × ~50ns | 1 × ~50ns | **64x** |
| **内存屏障** | 64 次 | 1-2 次 | **32x** |
| **缓存命中** | 较差 | 较好 (连续内存) | **2-4x** |

### 4.7 Rust 批量 API

```rust
impl AsyncFifo {
    /// 批量提交 find 请求
    pub fn submit_find_batch(&self, requests: &[(&[u8], u64)]) -> Vec<u64> {
        let count = requests.len();
        let mut nodes: Vec<*mut RequestNode> = vec![std::ptr::null_mut(); count];
        let mut ids = Vec::with_capacity(count);
        
        unsafe {
            // 批量分配
            fifo_alloc_request_batch(self.mgr.as_ptr(), nodes.as_mut_ptr(), count);
            
            // 填充请求
            for (i, (key, version)) in requests.iter().enumerate() {
                let id = self.next_id.fetch_add(1, Ordering::Relaxed);
                ids.push(id);
                
                let node = &mut *nodes[i];
                node.req.id = id;
                node.req.req_type = 1; // REQ_FIND_VALUE
                node.req.version = *version;
                node.req.key_len = key.len().min(32) as u8;
                node.req.key[..node.req.key_len as usize]
                    .copy_from_slice(&key[..node.req.key_len as usize]);
            }
            
            // 批量提交（单次内存屏障）
            fifo_submit_batch(self.mgr.as_ptr(), nodes.as_ptr(), count);
        }
        ids
    }
    
    /// 批量轮询完成
    pub fn poll_batch(&self, max: usize) -> Vec<FindResult> {
        let mut nodes: Vec<*mut CompletionNode> = vec![std::ptr::null_mut(); max];
        let mut results = Vec::new();
        
        unsafe {
            let count = fifo_poll_batch(self.mgr.as_ptr(), nodes.as_mut_ptr(), max);
            
            for i in 0..count {
                let node = &*nodes[i];
                results.push(FindResult {
                    id: node.comp.id,
                    status: node.comp.status,
                    value: if node.comp.value_len > 0 && node.comp.value_len != 0xFFFFFFFF {
                        Some(node.comp.value[..node.comp.value_len as usize].to_vec())
                    } else {
                        None
                    },
                    has_large_value: node.comp.value_len == 0xFFFFFFFF,
                    merkle_hash: node.comp.merkle_hash,
                });
            }
            
            fifo_free_completion_batch(self.mgr.as_ptr(), nodes.as_ptr(), count);
        }
        results
    }
}
```

### 4.8 Go 语言支持 ✅ 已实现

Go 通过 CGO 调用 `core/` 目录的纯 C API (`nomad_mpt.h`)：

```go
package main

import (
    "fmt"
    "log"
    "github.com/monad/nomad-mpt-go/monaddb"
)

func main() {
    // 打开内存数据库
    db, err := monaddb.OpenMemory()
    if err != nil {
        log.Fatal(err)
    }
    defer db.Close()

    // 创建 32 字节 key（以太坊标准，推荐）
    key := make([]byte, 32)
    key[0] = 0x01

    // 插入数据
    root, err := db.Put(nil, key, []byte("hello world"), 1)
    if err != nil {
        log.Fatal(err)
    }
    
    // 获取 Merkle 根哈希
    hash, _ := root.HashHex()
    fmt.Printf("Root: %s\n", hash)

    // 查询数据（内存模式使用 FindFromRoot）
    node, err := db.FindFromRoot(root, key, 1)
    if err != nil {
        log.Fatal(err)
    }
    if node != nil {
        value, _ := node.Value()
        fmt.Printf("Value: %s\n", value)
    }

    // 批量更新
    updates := []monaddb.Update{
        {Type: monaddb.UpdatePut, Key: makeKey(0x10), Value: []byte("v1")},
        {Type: monaddb.UpdatePut, Key: makeKey(0x20), Value: []byte("v2")},
    }
    root, _ = db.Upsert(root, updates, 2)
}

func makeKey(b byte) []byte {
    key := make([]byte, 32)
    key[0] = b
    return key
}
```

#### Go 绑定实现状态

| API | 状态 | 说明 |
|-----|------|------|
| `OpenMemory()` | ✅ | 内存数据库 |
| `OpenDisk()` | ✅ | 磁盘数据库 |
| `Put/Delete/Upsert` | ✅ | 同步写操作 |
| `Find` | ✅ | 磁盘模式查询 |
| `FindFromRoot` | ✅ | 内存/磁盘通用查询 |
| `Node.Hash/Value` | ✅ | 节点操作 |
| `Fifo.*` | 🔬 | 实验性（stub 实现）|

#### 异步 FIFO (实验性)

Go 绑定中的 `Fifo` API 目前是**实验性**的，C API 层只提供 stub 实现。
如需高并发异步操作，请使用 Rust 绑定（`bridge_fifo.cpp` 有完整的 `ck_fifo` 实现）。

```go
// FIFO 接口保留用于 API 兼容，但当前未完整实现
fifo, _ := db.CreateFifo()
if !fifo.IsImplemented() {
    log.Println("Warning: FIFO not fully implemented in C API")
}
```

#### 构建方式

```bash
cd nomad-mpt/bindings/go
./scripts/build.sh        # 完整构建
./scripts/build.sh --quick  # 快速重建（跳过 Rust）
go test ./monaddb/...     # 运行测试
```

### 4.9 多语言架构

```
┌────────────────────────────────────────────────────────────────┐
│                    C++ 侧 (libmonad_ffi.so)                    │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │              Shared Memory Region                        │  │
│  │  ┌───────────────┐           ┌───────────────┐          │  │
│  │  │  Request FIFO │           │ Response FIFO │          │  │
│  │  │  (ck_fifo)    │           │  (ck_fifo)    │          │  │
│  │  │               │           │               │          │  │
│  │  │  Rust/Go 写 ──┼──►Worker──┼──►Rust/Go 读  │          │  │
│  │  │               │           │               │          │  │
│  │  └───────────────┘           └───────────────┘          │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │              Worker Thread(s) - 批量处理                 │  │
│  │   while(running) {                                       │  │
│  │       n = dequeue_batch(req_fifo, batch, 64);           │  │
│  │       for (i = 0; i < n; i++) process(batch[i]);        │  │
│  │       enqueue_batch(resp_fifo, results, n);             │  │
│  │   }                                                      │  │
│  └─────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘
          ▲                                    │
          │ FFI: 批量读写 FIFO                 │ FFI
          │                                    ▼
┌─────────────────────┐              ┌─────────────────────┐
│      Rust 客户端     │              │      Go 客户端      │
│                     │              │                     │
│ fifo.submit_batch() │              │ fifo.SubmitBatch() │
│ fifo.poll_batch()   │              │ fifo.PollBatch()   │
└─────────────────────┘              └─────────────────────┘
```

### 4.10 使用示例

```rust
// Rust - 批量操作
let mut db = Db::open_memory()?;
let fifo = AsyncFifo::new(&mut db)?;
fifo.start(4);

// 批量提交 1000 个请求
let requests: Vec<_> = (0..1000)
    .map(|i| (format!("key{}", i).as_bytes().to_vec(), 1u64))
    .collect();
let refs: Vec<_> = requests.iter().map(|(k, v)| (k.as_slice(), *v)).collect();
let ids = fifo.submit_find_batch(&refs);

// 批量收集结果
let mut all_results = Vec::new();
while all_results.len() < ids.len() {
    let batch = fifo.poll_batch(64);
    all_results.extend(batch);
    if batch.is_empty() {
        std::thread::yield_now();
    }
}
```

```go
// Go - 批量操作
fifo := NewAsyncFifo(db)
fifo.Start(4)

// 批量提交 1000 个请求
requests := make([]FindRequest, 1000)
for i := range requests {
    requests[i] = FindRequest{Key: []byte(fmt.Sprintf("key%d", i)), Version: 1}
}
ids := fifo.SubmitFindBatch(requests)

// 批量收集结果
var allResults []Completion
for len(allResults) < len(ids) {
    batch := fifo.PollBatch(64)
    allResults = append(allResults, batch...)
    if len(batch) == 0 {
        runtime.Gosched()
    }
}
```

### 4.11 构建集成

```bash
# 下载 Concurrency Kit
git clone https://github.com/concurrencykit/ck.git depend/ck
cd depend/ck
./configure
make
```

```rust
// build.rs 追加
fn build_ck() {
    let ck_dir = PathBuf::from("depend/ck");
    
    cc::Build::new()
        .file(ck_dir.join("src/ck_hs.c"))
        .file(ck_dir.join("src/ck_ht.c"))
        .file(ck_dir.join("src/ck_rhs.c"))
        .include(ck_dir.join("include"))
        .flag("-std=c11")
        .compile("ck");
    
    println!("cargo:rustc-link-lib=static=ck");
}
```

---

## 阶段五：性能验证 (Benchmarking)

**目标**：确保 FFI 层没有引入明显的性能损耗。

### 5.1 基准测试

移植 `mpt_bench.cpp` 到 Rust：

```rust
// benches/mpt_bench.rs

use criterion::{criterion_group, criterion_main, Criterion, BenchmarkId};
use nomad_mpt::{Db, DbConfig, AsyncFifo};

fn bench_upsert(c: &mut Criterion) {
    let mut group = c.benchmark_group("upsert");
    
    for n_accounts in [100, 1000, 10000] {
        group.bench_with_input(
            BenchmarkId::new("accounts", n_accounts),
            &n_accounts,
            |b, &n| {
                b.iter(|| {
                    // ... benchmark logic
                });
            },
        );
    }
}

fn bench_find_sync(c: &mut Criterion) {
    // 同步 find 基准
}

fn bench_find_async(c: &mut Criterion) {
    // 异步 ring 基准
}

criterion_group!(benches, bench_upsert, bench_find_sync, bench_find_async);
criterion_main!(benches);
```

### 5.2 对比指标

| 指标 | C++ Native | Rust FFI (Sync) | Rust FFI (ck_fifo) |
|------|------------|-----------------|----------------------|
| upsert slots/s | baseline | < 5% overhead | N/A |
| find latency (cached) | baseline | < 10% overhead | < 15% overhead |
| find latency (cold) | baseline | ~same | batch amortized |
| traverse throughput | baseline | N/A | ~80% of native |

---

## 阶段六：兼容性测试与 CI (Verification)

**目标**：通过 100% 的以太坊官方测试用例。

### 6.1 测试集成

```rust
// tests/ethereum_trie_tests.rs

use nomad_mpt::Db;
use serde::Deserialize;

#[derive(Deserialize)]
struct TrieTest {
    #[serde(rename = "in")]
    inputs: Vec<(String, Option<String>)>,
    root: String,
}

#[test]
fn test_ethereum_trie_any_order() {
    let test_file = include_str!("../third_party/ethereum-tests/TrieTests/trieanyorder.json");
    let tests: HashMap<String, TrieTest> = serde_json::from_str(test_file).unwrap();
    
    for (name, test) in tests {
        let db = Db::open_in_memory().unwrap();
        let mut root = db.empty_root();
        
        for (key, value) in &test.inputs {
            let updates = match value {
                Some(v) => vec![Update::insert(hex::decode(key).unwrap(), hex::decode(v).unwrap())],
                None => vec![Update::delete(hex::decode(key).unwrap())],
            };
            root = db.upsert(&root, &updates, 1).unwrap();
        }
        
        let actual_root = hex::encode(root.hash());
        assert_eq!(actual_root, test.root, "Test '{}' failed", name);
    }
}
```

### 6.2 CI 配置

```yaml
# .github/workflows/rust-ffi.yml

name: Rust FFI Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-24.04
    
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y liburing-dev libtbb-dev
          # hugetlbfs 在 CI 中可选
      
      - name: Setup Rust
        uses: dtolnay/rust-toolchain@stable
      
      - name: Build
        working-directory: nomad-mpt
        run: cargo build --release
      
      - name: Run tests
        working-directory: nomad-mpt
        run: cargo test --release
      
      - name: Run benchmarks
        working-directory: nomad-mpt
        run: cargo bench --no-run  # 只编译，不运行（CI 环境不稳定）
```

---

## 风险说明

### 已识别风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| **内存对齐** | MonadDB 依赖 4KB/2MB 对齐 | FFI 传参使用 C++ 分配的缓冲区 |
| **HugePages** | CI 环境不支持 | 编译时可选，运行时降级 |
| **Fiber 调度** | 与 Rust async 冲突 | 使用 ck_fifo 解耦 |
| **平台限制** | 仅支持 Linux | 不支持 macOS/Windows |
| **大值传递** | >256B 值需要额外处理 | Value Pool 机制 |
| **ARM 汇编兼容性** | OpenSSL 汇编可能有许可证问题 | 使用 Apache 2.0 许可的版本 |
| **ARM CPU 特性检测** | 新芯片 (M4+) 需要支持 | 直接检测 `HWCAP_SHA3` / `FEAT_SHA3` 特性，自动兼容新芯片 |

### 未来扩展

1. **Traverse 异步支持**：通过 ck_fifo 提交 traverse 任务
2. **多 Worker 并行**：充分利用多核
3. **Merkle Proof 生成**：暴露 `node.data()` 路径
5. **Windows ARM64**：未来考虑（需要 IOCP 替代）

---

## 开发路线图

```
Phase 0 (Week 1-2)     Phase 1 (Week 3-4)     Phase 2 (Week 5-6)
┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐
│ ARM64 Keccak     │   │ build.rs 完善     │   │ UpdateBuilder    │
│ NEON 基础实现     │──▶│ 多架构支持        │──▶│ StateMachine     │
│ SHA3 硬件加速     │   │ 依赖探测          │   │ NodeHandle       │
└──────────────────┘   └──────────────────┘   └──────────────────┘
                                                       │
                                                       ▼
Phase 3 (Week 7-8)     Phase 4 (Week 9-10)    Phase 5 (Week 11-12)
┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐
│ cxx bridge       │   │ ck_fifo 异步     │   │ criterion bench  │
│ 同步 API          │──▶│ Worker Thread    │──▶│ 性能对比          │
│ 高级封装          │   │ 异步 API          │   │ 优化             │
└──────────────────┘   └──────────────────┘   └──────────────────┘
                                                       │
                                                       ▼
                       Phase 6 (Week 13-14)
                       ┌──────────────────┐
                       │ Ethereum Tests   │
                       │ CI/CD (x86+ARM)  │
                       │ 文档              │
                       └──────────────────┘
```

---

## 快速开始

建议从以下顺序开始实现：

### 🔴 优先级 1：ARM64 支持（阶段零）
1. **获取 OpenSSL ARM Keccak 汇编**：下载 `keccak1600-armv8.pl` 并生成 `.S` 文件
2. **修改 `keccak_impl.S`**：添加多架构条件编译
3. **添加 CPU 特性检测**：直接检测 SHA3 特性（已验证 M4 支持）
4. **验证正确性**：在 M1/M2/M3/M4 Mac 或 Graviton 上运行 Keccak 测试

### 🟡 优先级 2：FFI 基础（阶段一~三）
5. **修复 `build.rs`**：添加平台检测，改用 submodule
6. **实现 `UpdateBuilder`**：这是最复杂的适配层
7. **最小化 cxx bridge**：先实现 `db_open` + `db_find` 同步版本
8. **验证正确性**：跑通一个 Ethereum trie test

### 🟢 优先级 3：性能优化（阶段四~六）
9. **实现 ck_fifo 异步**：使用 Concurrency Kit 实现异步 API
10. **基准测试**：对比 C++ native vs Rust FFI
11. **CI/CD**：同时测试 x86_64 和 ARM64
