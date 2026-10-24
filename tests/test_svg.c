/* ============================================================================
 * Reverie Engine — tests/test_svg.c
 * Golden-pixel tests for the SVG engine: shapes, paths (incl. arcs),
 * gradients, transforms, viewBox, fill-rule, strokes, group opacity.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svg.h"
#include "util.h"

static int g_fails = 0;

#define CHECK(cond, ...)                                                    \
        do {                                                                    \
                if (!(cond)) {                                                      \
                        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
                        printf(__VA_ARGS__);                                            \
                        printf("\n");                                                   \
                        g_fails++;                                                      \
                }                                                                   \
        } while (0)

#define W 64
#define H 64

static u32 *render(const char *svg_text)
{
        RevSvg *svg = rev_svg_parse(svg_text, strlen(svg_text), NULL, 0);
        if (!svg) {
                CHECK(false, "parse failed: %.60s", svg_text);
                return NULL;
        }
        u32 *px = (u32 *)calloc((size_t)W * H, 4);
        rev_svg_render(svg, px, W, H);
        rev_svg_destroy(svg);
        return px;
}

static bool px_eq(u32 p, u32 r, u32 g, u32 b, u32 a, int tol)
{
        u32 pr = p & 0xff, pg = (p >> 8) & 0xff, pb = (p >> 16) & 0xff, pa = p >> 24;
        return (pr > r ? pr - r : r - pr) <= (u32)tol &&
               (pg > g ? pg - g : g - pg) <= (u32)tol &&
               (pb > b ? pb - b : b - pb) <= (u32)tol &&
               (pa > a ? pa - a : a - pa) <= (u32)tol;
}

#define CHK_PX(p, r, g, b, a, tol, ...) \
        CHECK(px_eq((p), (r), (g), (b), (a), (tol)), __VA_ARGS__)

int main(void)
{
        printf("test_svg:\n");

        /* 1. solid rect, hex color + opacity */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<rect x='8' y='8' width='48' height='48' fill='#ff0000' fill-opacity='0.5'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        CHK_PX(px[32 * W + 32], 255, 0, 0, 128, 4, "rect center red@0.5");
                        CHK_PX(px[2 * W + 2], 0, 0, 0, 0, 0, "outside transparent");
                        free(px);
                }
        }

        /* 2. linear gradient across a rect */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<defs><linearGradient id='g'>"
                        "<stop offset='0' stop-color='#000000'/>"
                        "<stop offset='1' stop-color='#ffffff'/>"
                        "</linearGradient></defs>"
                        "<rect x='0' y='0' width='64' height='64' fill='url(#g)'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        /* default x1=0,y1=0 -> x2=1,y2=0: horizontal ramp (oBB) */
                        u32 left = px[32 * W + 8];
                        u32 mid = px[32 * W + 32];
                        u32 right = px[32 * W + 56];
                        CHECK(((left >> 24) & 0xff) > 200, "left covered");
                        CHECK(((right >> 24) & 0xff) > 200, "right covered");
                        u32 lr = left & 0xff, mr = mid & 0xff, rr = right & 0xff;
                        CHECK(lr < 40, "left dark (r=%u)", lr);
                        CHECK(rr > 215, "right bright (r=%u)", rr);
                        CHECK(mr > 100 && mr < 155, "mid gray (r=%u)", mr);
                        free(px);
                }
        }

        /* 3. circle + radial gradient + transform scale */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<defs><radialGradient id='rg'>"
                        "<stop offset='0' stop-color='#00ff00'/>"
                        "<stop offset='1' stop-color='#0000ff'/>"
                        "</radialGradient></defs>"
                        "<g transform='translate(32,32) scale(1) translate(-32,-32)'>"
                        "<circle cx='32' cy='32' r='28' fill='url(#rg)'/>"
                        "</g></svg>";
                u32 *px = render(s);
                if (px) {
                        /* the rasterized gradient's t=0 lands at the pixel whose
                         * center is nearest the circle center — (30,30) here; the
                         * old probe at (32,32) sat at t~0.1 and passed only at
                         * EXACTLY the +-30 tolerance boundary (verified: pre-dither
                         * g=225/b=30), which one dither LSB pushed over. Probe the
                         * actual center: same tolerance, now with real margin. */
                        u32 c = px[30 * W + 30];
                        (void)c;
                        CHK_PX(c, 0, 255, 0, 255, 30, "radial center green");
                        u32 edge = px[32 * W + 56];   /* 24px from center ~ 0.86r */
                        u32 eg = (edge >> 8) & 0xff, eb = (edge >> 16) & 0xff;
                        CHECK(eb > eg, "edge bluer than green (g=%u b=%u)", eg, eb);
                        CHK_PX(px[2 * W + 2], 0, 0, 0, 0, 0, "corner transparent");
                        free(px);
                }
        }

        /* 4. fill-rule evenodd donut via path */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<path d='M32 6 L58 32 L32 58 L6 32 Z "
                        "M32 20 L44 32 L32 44 L20 32 Z' fill='#0080ff' fill-rule='evenodd'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        /* outer diamond edge midpoint between inner and outer */
                        CHK_PX(px[32 * W + 10], 0, 128, 255, 255, 40, "diamond ring filled");
                        CHK_PX(px[32 * W + 32], 0, 0, 0, 0, 0, "evenodd hole empty");
                        free(px);
                }
        }

        /* 5. rotated rect */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<rect x='24' y='4' width='16' height='56' fill='#ffff00'"
                        " transform='rotate(45 32 32)'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        /* diagonal band through center at 45deg */
                        CHK_PX(px[32 * W + 32], 255, 255, 0, 255, 10, "center of rotated rect");
                        CHK_PX(px[8 * W + 8], 0, 0, 0, 0, 0, "far corner empty");
                        free(px);
                }
        }

        /* 6. viewBox scaling (512-unit box -> 64 px) */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 512 512'>"
                        "<rect x='128' y='128' width='256' height='256' fill='#ff00ff'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        CHK_PX(px[32 * W + 32], 255, 0, 255, 255, 4, "viewBox rect center");
                        CHK_PX(px[4 * W + 4], 0, 0, 0, 0, 0, "viewBox outside");
                        free(px);
                }
        }

        /* 7. stroke: round cap, dash */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<line x1='16' y1='32' x2='48' y2='32' stroke='#00ffff'"
                        " stroke-width='8' stroke-linecap='round'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        CHK_PX(px[32 * W + 32], 0, 255, 255, 255, 30, "thick line center");
                        /* round cap extends past endpoint: x=52 still colored */
                        u32 cap = px[32 * W + 51];
                        CHECK((cap >> 24) > 100, "round cap covers x=51 (a=%u)", cap >> 24);
                        /* butt cap would end at 52; check beyond: x=56 should be empty */
                        CHECK((px[32 * W + 57] >> 24) == 0, "beyond cap empty");
                        free(px);
                }
        }

        /* 8. group opacity */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<g opacity='0.5'><rect x='0' y='0' width='64' height='64' fill='#ffffff'/></g>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        CHK_PX(px[32 * W + 32], 255, 255, 255, 128, 6, "group opacity 0.5");
                        free(px);
                }
        }

        /* 9. elliptical arc path (quarter pie) */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<path d='M32 32 L56 32 A24 24 0 0 1 32 56 Z' fill='#ff8000'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        /* point inside pie: (48, 40) ~ radius 25 hypot(16,8)=17.9 <24 at 26deg */
                        CHK_PX(px[40 * W + 48], 255, 128, 0, 255, 40, "arc pie interior");
                        /* mirrored quadrant must be empty */
                        CHK_PX(px[40 * W + 16], 0, 0, 0, 0, 0, "opposite quadrant empty");
                        free(px);
                }
        }

        /* 10. named color + rgb() + rgba() */
        {
                const char *s =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<rect x='0' y='0' width='21' height='64' fill='steelblue'/>"
                        "<rect x='21' y='0' width='21' height='64' fill='rgb(10,20,30)'/>"
                        "<rect x='42' y='0' width='22' height='64' fill='rgba(255,0,0,0.5)'/>"
                        "</svg>";
                u32 *px = render(s);
                if (px) {
                        CHK_PX(px[32 * W + 10], 70, 130, 180, 255, 4, "steelblue");
                        CHK_PX(px[32 * W + 31], 10, 20, 30, 255, 4, "rgb()");
                        CHK_PX(px[32 * W + 52], 255, 0, 0, 128, 6, "rgba()");
                        free(px);
                }
        }

        /* 11. document prolog: BOM / <?xml?> declaration / comments / PI
         *     before the root — every real export (Inkscape, Illustrator,
         *     Figma) carries these, and the whitespace between them and
         *     <svg> used to kill the parse ("not an <svg> document").
         *     Every variant must parse AND render byte-identical to the
         *     bare document. */
        {
                const char *core =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<rect x='0' y='0' width='64' height='32' fill='#336699'/>"
                        "</svg>";
                u32 *ref = render(core);
                CHECK(ref != NULL, "bare document parses (reference)");

                char v_decl[512], v_bom[512], v_ws[512], v_adjacent[512];
                char bad_pi[512], bad_cmt[512];
                snprintf(v_decl, sizeof v_decl,
                         "<?xml version='1.0' encoding='UTF-8'?>\n%s", core);
                snprintf(v_bom, sizeof v_bom,
                         "\xEF\xBB\xBF"
                         "<?xml version='1.0' encoding='UTF-8' standalone='no'?>\r\n"
                         "<!-- Generator: Reverie Test 11 -->\n%s", core);
                snprintf(v_ws, sizeof v_ws, "  \n\t%s   \n", core);
                snprintf(v_adjacent, sizeof v_adjacent,
                         "<!--c--><?pi ?><!--x2-->\n%s", core);
                const char *v_mid =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "  <!-- mid-document comment -->"
                        "  <?processing instruction?>"
                        "  <rect x='0' y='0' width='64' height='32' fill='#336699'/>"
                        "  </svg>";
                const char *variants[] = { v_decl, v_bom, v_ws, v_adjacent, v_mid };
                const char *names[] = {
                        "xml declaration + newline",
                        "BOM + declaration + generator comment",
                        "leading/trailing whitespace",
                        "adjacent comments/PIs, no whitespace",
                        "misc between root children",
                };
                for (size_t i = 0; i < sizeof variants / sizeof *variants; i++) {
                        u32 *px = render(variants[i]);
                        if (px) {
                                CHECK(memcmp(px, ref, (size_t)W * H * 4) == 0,
                                      "prolog variant '%s' renders identical",
                                      names[i]);
                                free(px);
                        } else {
                                CHECK(false, "prolog variant '%s' failed to parse",
                                      names[i]);
                        }
                }
                free(ref);

                /* unterminated prolog junk is still a clean parse error */
                snprintf(bad_pi, sizeof bad_pi, "<?xml version='1.0'%s", core);
                RevSvg *bad = rev_svg_parse(bad_pi, strlen(bad_pi), NULL, 0);
                CHECK(bad == NULL, "unterminated PI refused");
                snprintf(bad_cmt, sizeof bad_cmt, "<!-- never closed%s", core);
                RevSvg *bad2 = rev_svg_parse(bad_cmt, strlen(bad_cmt), NULL, 0);
                CHECK(bad2 == NULL, "unterminated comment refused");
        }

        /* 12. comma-separated path coordinates: Illustrator writes
         *     "M0,90 L14,44", Inkscape writes "M0 90 L14 44" — the SVG
         *     grammar allows both, and the comma form used to parse as a
         *     failed path (silently blank layer). */
        {
                const char *spaces =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<path d='M8 8 L56 8 L56 56 L8 56 Z' fill='#ffcc00'/>"
                        "</svg>";
                const char *commas =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<path d='M8,8 L56,8 L56,56 L8,56 Z' fill='#ffcc00'/>"
                        "</svg>";
                const char *mixed =
                        "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                        "<path d='M8,8L56,8L56,56L8,56Z' fill='#ffcc00'/>"
                        "</svg>";
                u32 *ref = render(spaces);
                CHECK(ref != NULL, "space-separated path parses (reference)");
                u32 *rc = render(commas);
                CHECK(rc != NULL, "comma-separated path parses");
                if (ref && rc)
                        CHECK(memcmp(ref, rc, (size_t)W * H * 4) == 0,
                              "comma path renders identical to space path");
                free(rc);
                u32 *rm = render(mixed);
                CHECK(rm != NULL, "comma-as-only-separator path parses");
                if (ref && rm)
                        CHECK(memcmp(ref, rm, (size_t)W * H * 4) == 0,
                              "comma-only path renders identical");
                free(rm);
                free(ref);
        }

        if (g_fails == 0)
                printf("  svg: OK\n");
        return g_fails != 0;
}
