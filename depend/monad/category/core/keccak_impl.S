// MonadDB Keccak256 multi-architecture assembly wrapper
// Supports: Linux x86_64 (AVX2/AVX512), Linux ARM64 (NEON + optional SHA3)
// NOTE: Only Linux is supported. macOS and Windows are NOT supported.

#if !defined(__linux__)
    #error "Only Linux is supported. macOS and Windows are NOT supported."
#endif

#if defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

// ============ x86_64: AVX2 or AVX512 ============
#if defined(__x86_64__) || defined(_M_X64)
    #if defined(__AVX512F__)
        #include <crypto/sha/asm/keccak1600-avx512.S>
    #elif defined(__AVX2__)
        #include <crypto/sha/asm/keccak1600-avx2.S>
    #else
        #error x86_64 requires AVX2 or AVX512 support
    #endif

// ============ ARM64: NEON + optional ARMv8.2 SHA3 (Linux only) ============
#elif defined(__aarch64__) || defined(__arm64__)
    #include <crypto/sha/asm/keccak1600-armv8.S>

#else
    #error Unsupported architecture. Supported: x86_64 (AVX2/AVX512), aarch64 (NEON/SHA3)
#endif
