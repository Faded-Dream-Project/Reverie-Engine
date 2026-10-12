/* ============================================================================
 * Reverie Engine — simd.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "simd.h"

#if defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>
#  define X86_CPUID
#endif

#if defined(X86_CPUID)
/* xgetbv via inline asm — safe after verifying CPUID.1 ECX.ECX[27] (OSXSAVE). */
static inline u64 rev_xgetbv(u32 idx)
{
        u32 lo, hi;
        __asm__ volatile ("xgetbv" : "=a" (lo), "=d" (hi) : "c" (idx));
        return ((u64)hi << 32) | lo;
}
#endif

u32 rev_cpu_features(void)
{
#if defined(X86_CPUID)
        /* Cache the result; feature bits don't change under us. */
        static u32 cached = 0;
        static bool done = false;
        if (done)
                return cached;

        u32 feat = REV_CPU_SSE2;  /* x86-64 baseline */

        u32 a = 0, b = 0, c = 0, d = 0;
        if (__get_cpuid(1, &a, &b, &c, &d)) {
                if (c & bit_SSSE3) feat |= REV_CPU_SSSE3;
                if (c & bit_SSE4_1) feat |= REV_CPU_SSE41;
                bool osxsave = (c & bit_OSXSAVE) != 0;
                bool cpu_avx  = (c & bit_AVX) != 0;
                if (osxsave && cpu_avx) {
                        u64 xcr0 = rev_xgetbv(0);
                        if ((xcr0 & 0x6) == 0x6) {
                                u32 a7 = 0, b7 = 0, c7 = 0, d7 = 0;
                                if (__get_cpuid_count(7, 0, &a7, &b7, &c7, &d7)) {
                                        if (b7 & bit_AVX2)
                                                feat |= REV_CPU_AVX2;
                                }
                        }
                }
        }

        cached = feat;
        done = true;
        return feat;
#else
        /* Portable fallback: plain C everywhere (also the -O2 auto-vectorizer
         * still kicks in without intrinsics). */
        return 0;
#endif
}
