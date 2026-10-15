/* ============================================================================
 * Reverie Engine — tests/test_scale.c
 * Unit tests for rev_scale_place(): the --video media placement math
 * (fill / fit / stretch / center), including crop offsets (negative dx/dy
 * are legal and mean "cropped by the viewport") and the extreme-aspect
 * bound that keeps the placed-rect arithmetic inside u32/i32.
 *
 * The render integration (letterbox bars, sub-rect conversion) is driven
 * end-to-end by scripts/e2e.sh's video-mode legs and pixel-checked there.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>

#include "../src/cli.h"
#include "../src/geom.h"

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

static int place_ok(int mode, u32 W, u32 H, u32 iw, u32 ih,
                    u32 ew, u32 eh, i32 edx, i32 edy, const char *msg)
{
        RScaleRect r = rev_scale_place(mode, W, H, iw, ih);
        int ok = r.w == ew && r.h == eh && r.dx == edx && r.dy == edy;
        if (!ok)
                printf("        got %ux%u at %d,%d, want %ux%u at %d,%d\n",
                       r.w, r.h, r.dx, r.dy, ew, eh, edx, edy);
        CHECK(ok, msg);
        return ok;
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);

        /* 16:9 source into a 4:3 viewport — the classic letterbox case */
        place_ok(REV_SCALE_FILL, 640, 480, 320, 180,
                 853, 480, -106, 0, "fill: cover 16:9 -> 4:3 (crops width)");
        place_ok(REV_SCALE_FIT, 640, 480, 320, 180,
                 640, 360, 0, 60, "fit: contain 16:9 -> 4:3 (pillarbox-free)");
        place_ok(REV_SCALE_STRETCH, 640, 480, 320, 180,
                 640, 480, 0, 0, "stretch: exact viewport, aspect ignored");
        place_ok(REV_SCALE_CENTER, 640, 480, 320, 180,
                 320, 180, 160, 150, "center: native size, centered");

        /* 4:3 source into 16:9 viewport — pillarbox this time */
        place_ok(REV_SCALE_FILL, 1920, 1080, 640, 480,
                 1920, 1440, 0, -180, "fill: cover 4:3 -> 16:9 (crops height)");
        place_ok(REV_SCALE_FIT, 1920, 1080, 640, 480,
                 1440, 1080, 240, 0, "fit: contain 4:3 -> 16:9 (pillarbox)");

        /* center with an oversized source: negative offsets = symmetric crop */
        place_ok(REV_SCALE_CENTER, 640, 480, 800, 600,
                 800, 600, -80, -60, "center: oversized source crops symmetrically");

        /* exact fits are identity */
        place_ok(REV_SCALE_FILL, 320, 240, 320, 240,
                 320, 240, 0, 0, "fill: same aspect = identity");
        place_ok(REV_SCALE_FIT, 320, 240, 320, 240,
                 320, 240, 0, 0, "fit: same aspect = identity");

        /* extreme aspect ratio under fill: the placed rect is huge but the
         * arithmetic stays in range (the CPU tier converts the visible
         * sub-rect, never this rect — see place_media_rgba) */
        place_ok(REV_SCALE_FILL, 1920, 1080, 100, 10000,
                 1920, 192000, 0, -95460,
                 "fill: extreme aspect keeps arithmetic bounded");

        /* degenerate inputs: zero viewport or source */
        {
                RScaleRect r = rev_scale_place(REV_SCALE_FILL, 0, 480, 320, 180);
                CHECK(r.w == 0 && r.h == 0 && r.dx == 0 && r.dy == 0,
                      "zero viewport -> empty placement");
                r = rev_scale_place(REV_SCALE_CENTER, 640, 480, 320, 0);
                CHECK(r.w == 0 && r.h == 0, "zero source -> empty placement");
        }

        /* unknown mode falls back to fill (defensive default) */
        {
                RScaleRect r = rev_scale_place(99, 640, 480, 320, 180);
                RScaleRect f = rev_scale_place(REV_SCALE_FILL, 640, 480, 320, 180);
                CHECK(r.w == f.w && r.h == f.h && r.dx == f.dx && r.dy == f.dy,
                      "unknown mode degrades to fill");
        }

        printf(failures ? "scale: %d FAILURE(S)\n" : "scale: OK\n", failures);
        return failures ? 1 : 0;
}
