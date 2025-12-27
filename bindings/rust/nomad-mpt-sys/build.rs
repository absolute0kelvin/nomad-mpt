use std::env;
use std::path::PathBuf;

fn main() {
    // ============================================================
    // 1. 平台检测 - 仅支持 Linux
    // ============================================================
    #[cfg(not(target_os = "linux"))]
    compile_error!("nomad-mpt-sys only supports Linux (requires io_uring)");

    let _out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());  // 保留备用
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // 目录结构: nomad-mpt/bindings/rust/nomad-mpt-sys/
    // depend 目录在 nomad-mpt/depend/
    let nomad_mpt_dir = manifest_dir
        .parent().unwrap()  // nomad-mpt/bindings/rust/
        .parent().unwrap()  // nomad-mpt/bindings/
        .parent().unwrap(); // nomad-mpt/
    let cpp_source_dir = nomad_mpt_dir.join("depend").join("monad");

    // ============================================================
    // 2. 系统库探测 (使用 pkg-config)
    // ============================================================
    
    // liburing - 必需
    pkg_config::probe_library("liburing")
        .expect("liburing not found. Install with: apt install liburing-dev");
    
    // TBB - 尝试探测，如果失败则跳过（新版本可能不需要）
    let _has_tbb = pkg_config::probe_library("tbb").is_ok();
    
    // hugetlbfs - 直接检测（没有 pkg-config 文件）
    let has_hugetlbfs = std::path::Path::new("/usr/include/hugetlbfs.h").exists() 
        || std::path::Path::new("/usr/local/include/hugetlbfs.h").exists();
    
    if has_hugetlbfs {
        println!("cargo:rustc-cfg=feature=\"hugetlbfs\"");
        println!("cargo:warning=hugetlbfs support enabled");
    } else {
        println!("cargo:warning=hugetlbfs not found, huge page support disabled");
    }

    // ============================================================
    // 3. 架构检测
    // ============================================================
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    match target_arch.as_str() {
        "x86_64" => {
            println!("cargo:rustc-cfg=keccak_asm_x86");
            println!("cargo:warning=Building for x86_64 with AVX2/AVX512 Keccak");
        }
        "aarch64" => {
            println!("cargo:rustc-cfg=keccak_asm_arm64");
            println!("cargo:warning=Building for ARM64 with NEON/SHA3 Keccak");
        }
        _ => {
            println!("cargo:warning=Unknown architecture: {}, using portable Keccak", target_arch);
        }
    }

    // ============================================================
    // 4. CMake 构建
    // ============================================================
    let mut cmake_config = cmake::Config::new(&cpp_source_dir);
    
    cmake_config
        .define("CMAKE_BUILD_TYPE", "Release")
        .define("MONAD_COMPILER_LLVM", "OFF")
        .define("MONAD_COMPILER_TESTING", "OFF")
        .define("MONAD_COMPILER_BENCHMARKS", "OFF");

    // 编译器配置 - 优先使用 clang-19
    let cc = env::var("CC").unwrap_or_else(|_| {
        if std::path::Path::new("/usr/bin/clang-19").exists() {
            "/usr/bin/clang-19".to_string()
        } else {
            "clang".to_string()
        }
    });
    let cxx = env::var("CXX").unwrap_or_else(|_| {
        if std::path::Path::new("/usr/bin/clang++-19").exists() {
            "/usr/bin/clang++-19".to_string()
        } else {
            "clang++".to_string()
        }
    });
    
    cmake_config
        .define("CMAKE_C_COMPILER", &cc)
        .define("CMAKE_CXX_COMPILER", &cxx);

    // 只构建，不安装（避免某些第三方库的安装问题）
    let dst = cmake_config
        .build_target("monad_ffi")
        .build();

    // ============================================================
    // 5. 链接库
    // ============================================================
    
    // 获取构建目录
    let build_dir = dst.join("build");
    
    // 静态库搜索路径
    println!("cargo:rustc-link-search=native={}", dst.display());
    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-search=native={}/category/core", build_dir.display());
    println!("cargo:rustc-link-search=native={}/category/async", build_dir.display());
    println!("cargo:rustc-link-search=native={}/category/mpt", build_dir.display());
    println!("cargo:rustc-link-search=native={}/third_party/asmjit", build_dir.display());
    println!("cargo:rustc-link-search=native={}/third_party/quill/quill", build_dir.display());
    println!("cargo:rustc-link-search=native={}/third_party/silkpre", build_dir.display());
    println!("cargo:rustc-link-search=native={}/third_party/ethash/lib/keccak", build_dir.display());
    println!("cargo:rustc-link-search=native={}/third_party/BLAKE3/c", build_dir.display());
    println!("cargo:rustc-link-search=native={}/third_party/silkpre/third_party", build_dir.display());
    println!("cargo:rustc-link-search=native={}/third_party/silkpre/third_party/libff/libff", build_dir.display());
    
    // 链接 Monad FFI 库 (包含 core + async + trie)
    println!("cargo:rustc-link-lib=static=monad_ffi");
    
    // 第三方静态库
    println!("cargo:rustc-link-lib=static=quill");
    println!("cargo:rustc-link-lib=static=blake3");
    println!("cargo:rustc-link-lib=static=keccak");
    
    // 系统动态库
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-lib=dylib=uring");
    println!("cargo:rustc-link-lib=dylib=gmp");
    println!("cargo:rustc-link-lib=dylib=crypto");
    println!("cargo:rustc-link-lib=dylib=zstd");
    println!("cargo:rustc-link-lib=dylib=archive");
    println!("cargo:rustc-link-lib=dylib=boost_stacktrace_backtrace");
    println!("cargo:rustc-link-lib=dylib=boost_fiber");
    println!("cargo:rustc-link-lib=dylib=boost_context");
    println!("cargo:rustc-link-search=native=/usr/lib/gcc/aarch64-linux-gnu/13");
    println!("cargo:rustc-link-lib=static=backtrace");
    
    if has_hugetlbfs {
        println!("cargo:rustc-link-lib=dylib=hugetlbfs");
    }

    // ============================================================
    // 6. Concurrency Kit (ck) 库
    // ============================================================
    let ck_dir = nomad_mpt_dir.join("depend/ck");
    
    // 编译 ck_wrapper.c（C 语言包装层）
    cc::Build::new()
        .file("src/ck_wrapper.c")
        .include(ck_dir.join("include"))
        .flag("-std=c11")
        .compile("ck_wrapper");
    
    // 链接 ck 静态库
    println!("cargo:rustc-link-search=native={}/src", ck_dir.display());
    println!("cargo:rustc-link-lib=static=ck");
    
    // ============================================================
    // 7. CXX 桥接代码生成
    // ============================================================
    let third_party = cpp_source_dir.join("third_party");
    
    cxx_build::bridge("src/lib.rs")
        .file("src/bridge.cpp")
        .file("src/bridge_fifo.cpp")  // 异步 FIFO 实现
        // 项目源目录（用于 bridge.hpp）
        .include(&manifest_dir)
        .include(&cpp_source_dir)
        .include(cpp_source_dir.join("category"))
        .include(&third_party)
        // Concurrency Kit
        .include(ck_dir.join("include"))
        // 第三方库 include 路径
        .include(third_party.join("intx/include"))
        .include(third_party.join("evmc/include"))
        .include(third_party.join("ethash/include"))
        .include(third_party.join("nlohmann_json/single_include"))
        .include(third_party.join("immer"))
        .include(third_party.join("quill/quill/include"))
        .include(third_party.join("cthash/include"))
        .include(third_party.join("ankerl"))
        .include(third_party.join("concurrentqueue"))
        .include(third_party.join("unordered_dense/include"))
        .flag_if_supported("-std=c++23")
        .flag_if_supported("-Wno-unused-parameter")
        .flag_if_supported("-Wno-deprecated-declarations")
        .compile("nomad-mpt-bridge");

    // 重新编译触发条件
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=src/bridge.cpp");
    println!("cargo:rerun-if-changed=src/bridge_fifo.cpp");
    println!("cargo:rerun-if-changed=src/bridge_fifo.hpp");
    println!("cargo:rerun-if-changed={}", cpp_source_dir.join("CMakeLists.txt").display());
    println!("cargo:rerun-if-changed={}", ck_dir.display());
}
