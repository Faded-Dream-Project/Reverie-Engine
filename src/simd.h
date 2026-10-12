/* ============================================================================
 * Reverie Engine — simd.h
 * CPU feature detection. The engine is pure C; SSE2 is baseline on x86-64 and
 * used via intrinsics guarded by __SSE2__, while AVX2 kernels use GCC function
 * multi-versioning attributes and are dispatched at runtime after cpuid.
 * Nothing outside the compiler itself is required.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_SIMD_H
#define REV_SIMD_H

#include "rev_types.h"

/* Feature flags (bitmask). */
#define REV_CPU_SSE2 (1u << 0)   /* guaranteed on x86-64 */
#define REV_CPU_SSSE3 (1u << 1)
#define REV_CPU_SSE41 (1u << 2)
#define REV_CPU_AVX2 (1u << 3)

u32  rev_cpu_features(void);

#endif /* REV_SIMD_H */
