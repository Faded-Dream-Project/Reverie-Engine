/* ============================================================================
 * Reverie Engine — tests/test_composite.c
 * rev_comp_fade exact-value tests: the theme-switch darkening must multiply
 * RGB by (1-black) with the SAME rounding on the SSE2 path (n >= 4) and the
 * scalar tail, and leave alpha untouched.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/composite.h"

static int failures;

#define CHECK(cond, msg)                                                \
        do {                                                            \
                if (cond) {                                             \
                        printf("  ok: %s\n", msg);                      \
                } else {                                                \
                        printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
                        failures++;                                     \
                }                                                       \
        } while (0)

/* engine pixel order: 0xAABBGGRR */
#define PX(a, b, g, r) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))

static u32 expect_scale(u32 c, u32 keep)
{
        return (c * keep + 128u) >> 8;
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);

        /* 12 pixels: exercises the SSE2 4-lane path (n>=4) AND the scalar
         * tail (12 % 4 = 0 -> make it 13 to also cover the tail) */
        u32 px[13] = {
                PX(255, 200, 150, 100),
                PX(255, 0, 255, 0),
                PX(128, 1, 2, 3),
                PX(255, 255, 255, 255),
                PX(200, 10, 20, 30),
                PX(0, 0, 0, 0),
                PX(255, 128, 128, 128),
                PX(255, 255, 0, 0),
                PX(255, 0, 0, 255),
                PX(90, 77, 66, 55),
                PX(255, 200, 150, 100),
                PX(1, 1, 1, 1),
                PX(255, 50, 60, 70),
        };
        u32 orig[13];
        memcpy(orig, px, sizeof px);

        /* fade=0 must be a no-op */
        {
                RFb fb = { px, 13, 1 };
                rev_comp_fade(&fb, 0.0);
                CHECK(memcmp(px, orig, sizeof px) == 0, "fade 0.0 is a no-op");
        }

        /* fade=1: fully black, alpha preserved */
        {
                u32 p2[13];
                memcpy(p2, orig, sizeof orig);
                RFb fb = { p2, 13, 1 };
                rev_comp_fade(&fb, 1.0);
                bool ok = true;
                for (int i = 0; i < 13; i++)
                        if (p2[i] != (orig[i] & 0xFF000000u))
                                ok = false;
                CHECK(ok, "fade 1.0: RGB zero, alpha preserved (SSE + tail)");
        }

        /* fade=0.5: keep = 128; every channel = (c*128+128)>>8 — exact
         * expected values computed the same way as the scalar reference */
        {
                u32 p2[13];
                memcpy(p2, orig, sizeof orig);
                RFb fb = { p2, 13, 1 };
                rev_comp_fade(&fb, 0.5);
                u32 keep = 128u;
                bool ok = true;
                for (int i = 0; i < 13; i++) {
                        u32 want = (orig[i] & 0xFF000000u) |
                                   expect_scale((orig[i] >> 16) & 0xFF, keep) << 16 |
                                   expect_scale((orig[i] >> 8) & 0xFF, keep) << 8 |
                                   expect_scale(orig[i] & 0xFF, keep);
                        if (p2[i] != want) {
                                printf("    px %d: want %08x got %08x\n", i, want, p2[i]);
                                ok = false;
                        }
                }
                CHECK(ok, "fade 0.5: exact (c*128+128)>>8 on every pixel (SSE lanes + scalar tail)");
        }

        /* a 3-pixel buffer: scalar path only (n < 4) — same math */
        {
                u32 p3[3] = { PX(255, 200, 150, 100), PX(128, 9, 8, 7), PX(255, 0, 255, 0) };
                u32 o3[3];
                memcpy(o3, p3, sizeof p3);
                RFb fb = { p3, 3, 1 };
                rev_comp_fade(&fb, 0.25);
                u32 keep = 192u;    /* 1 - 0.25 -> k=64, keep=192 */
                bool ok = true;
                for (int i = 0; i < 3; i++) {
                        u32 want = (o3[i] & 0xFF000000u) |
                                   expect_scale((o3[i] >> 16) & 0xFF, keep) << 16 |
                                   expect_scale((o3[i] >> 8) & 0xFF, keep) << 8 |
                                   expect_scale(o3[i] & 0xFF, keep);
                        if (p3[i] != want) {
                                printf("    px %d: want %08x got %08x\n", i, want, p3[i]);
                                ok = false;
                        }
                }
                CHECK(ok, "scalar-only buffer (n=3): same exact values");
        }

        /* out-of-range fades clamp instead of exploding */
        {
                u32 p2[4];
                memcpy(p2, orig, sizeof p2);
                RFb fb = { p2, 4, 1 };
                rev_comp_fade(&fb, 7.0);
                bool ok = true;
                for (int i = 0; i < 4; i++)
                        if (p2[i] != (orig[i] & 0xFF000000u))
                                ok = false;
                CHECK(ok, "fade 7.0 clamps to full black");
        }

        if (failures == 0) {
                printf("composite: OK\n");
                return 0;
        }
        printf("composite: %d failure(s)\n", failures);
        return 1;
}
