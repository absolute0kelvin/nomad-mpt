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

// ARM CPU feature detection for Keccak/SHA3 hardware acceleration
// Reference: OpenSSL crypto/armcap.c

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)

#include <stdint.h>

// Global capability flags, used by OpenSSL assembly code
// This variable is referenced by keccak1600-armv8.S
unsigned int OPENSSL_armcap_P = 0;

// Capability bits - MUST match OpenSSL definitions in arm_arch.h exactly!
// See: third_party/openssl/crypto/sha/asm/arm_arch.h
#define ARMV7_NEON      (1 << 0)
#define ARMV7_TICK      (1 << 1)
#define ARMV8_AES       (1 << 2)
#define ARMV8_SHA1      (1 << 3)
#define ARMV8_SHA256    (1 << 4)
#define ARMV8_PMULL     (1 << 5)
#define ARMV8_SHA512    (1 << 6)
#define ARMV8_CPUID     (1 << 7)
#define ARMV8_RNG       (1 << 8)
#define ARMV8_SM3       (1 << 9)
#define ARMV8_SM4       (1 << 10)
#define ARMV8_SHA3      (1 << 11)  // ARMv8.2 SHA3 extension - NOTE: bit 11, not 10!

// ============ Linux ARM64 Detection ============
#if defined(__linux__)

#include <sys/auxv.h>

// HWCAP bits for ARM64 (from asm/hwcap.h)
#ifndef HWCAP_AES
#define HWCAP_AES       (1 << 3)
#endif
#ifndef HWCAP_SHA1
#define HWCAP_SHA1      (1 << 5)
#endif
#ifndef HWCAP_SHA2
#define HWCAP_SHA2      (1 << 6)
#endif
#ifndef HWCAP_SHA3
#define HWCAP_SHA3      (1 << 17)
#endif
#ifndef HWCAP_SHA512
#define HWCAP_SHA512    (1 << 21)
#endif

__attribute__((constructor))
static void monad_detect_arm_features(void)
{
    unsigned long hwcap = getauxval(AT_HWCAP);

    // All ARM64 Linux systems have NEON
    OPENSSL_armcap_P |= ARMV7_NEON;

    if (hwcap & HWCAP_AES) {
        OPENSSL_armcap_P |= ARMV8_AES;
    }
    if (hwcap & HWCAP_SHA1) {
        OPENSSL_armcap_P |= ARMV8_SHA1;
    }
    if (hwcap & HWCAP_SHA2) {
        OPENSSL_armcap_P |= ARMV8_SHA256;
    }
    if (hwcap & HWCAP_SHA3) {
        OPENSSL_armcap_P |= ARMV8_SHA3;
    }
    if (hwcap & HWCAP_SHA512) {
        OPENSSL_armcap_P |= ARMV8_SHA512;
    }
}

// ============ Other Platforms: Not Supported ============
#else
#error "ARM64 is only supported on Linux. macOS and Windows are not supported."
#endif // Platform detection

#endif // __aarch64__

