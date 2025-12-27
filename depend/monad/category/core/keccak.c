// Copyright (C) 2025 Category Labs, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <category/core/keccak.h>

#include <stddef.h>
#include <stdint.h>

#define BLOCK_SIZE ((1600 - 2 * 256) / 8)

// Base NEON implementation (all ARM64)
extern size_t
SHA3_absorb(uint64_t A[5][5], unsigned char const *inp, size_t len, size_t r);

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)

// ARMv8.2 SHA3 crypto extension implementation (Apple M1+, Graviton 3+, etc.)
extern size_t
SHA3_absorb_cext(uint64_t A[5][5], unsigned char const *inp, size_t len, size_t r);

extern void
SHA3_squeeze_cext(uint64_t A[5][5], unsigned char *out, size_t len, size_t r, int next);

// ARM64 OpenSSL assembly has a 5th 'next' parameter for SHA3_squeeze
// next=0 means this is the first squeeze call (normal case for Keccak256)
extern void
SHA3_squeeze(uint64_t A[5][5], unsigned char *out, size_t len, size_t r, int next);

// Capability flag from arm_cpu_detect.c - matches OpenSSL arm_arch.h
extern unsigned int OPENSSL_armcap_P;
#define ARMV8_SHA3 (1 << 11)

// Runtime dispatch based on CPU capabilities
static inline size_t sha3_absorb(uint64_t A[5][5], unsigned char const *inp, size_t len, size_t r)
{
    if (OPENSSL_armcap_P & ARMV8_SHA3) {
        return SHA3_absorb_cext(A, inp, len, r);
    }
    return SHA3_absorb(A, inp, len, r);
}

static inline void sha3_squeeze(uint64_t A[5][5], unsigned char *out, size_t len, size_t r)
{
    if (OPENSSL_armcap_P & ARMV8_SHA3) {
        SHA3_squeeze_cext(A, out, len, r, 0);
    } else {
        SHA3_squeeze(A, out, len, r, 0);
    }
}

#else  // x86_64 or other

extern void
SHA3_squeeze(uint64_t A[5][5], unsigned char *out, size_t len, size_t r);

static inline size_t sha3_absorb(uint64_t A[5][5], unsigned char const *inp, size_t len, size_t r)
{
    return SHA3_absorb(A, inp, len, r);
}

static inline void sha3_squeeze(uint64_t A[5][5], unsigned char *out, size_t len, size_t r)
{
    SHA3_squeeze(A, out, len, r);
}

#endif  // __aarch64__

void keccak256(
    unsigned char const *const in, unsigned long const len,
    unsigned char out[KECCAK256_SIZE])
{
    uint64_t A[5][5];
    unsigned char blk[BLOCK_SIZE];

    __builtin_memset(A, 0, sizeof(A));

    size_t const rem = sha3_absorb(A, in, len, BLOCK_SIZE);
    if (rem > 0) {
        __builtin_memcpy(blk, &in[len - rem], rem);
    }
    __builtin_memset(&blk[rem], 0, BLOCK_SIZE - rem);
    blk[rem] = 0x01;
    blk[BLOCK_SIZE - 1] |= 0x80;
    (void)sha3_absorb(A, blk, BLOCK_SIZE, BLOCK_SIZE);

    sha3_squeeze(A, out, 32, BLOCK_SIZE);
}
