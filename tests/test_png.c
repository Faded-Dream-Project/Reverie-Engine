/* ============================================================================
 * Reverie Engine — tests/test_png.c
 * Decodes every fixture PNG (all color types, depths, filters, Adam7) and
 * compares byte-exact against oracle .expect files; round-trips the encoder.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"
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

typedef struct {
        u32 w, h;
        u32 *px;
        u8 *raw;   /* owning buffer; px points into it */
} Expect;

static bool load_expect(const char *path, Expect *e)
{
        size_t len = 0;
        u8 *d = rev_read_file(path, &len);
        if (!d || len < 12 || memcmp(d, "REVE", 4) != 0) {
                printf("  FATAL bad expect file %s\n", path);
                exit(2);
        }
        e->w = d[4] | (d[5] << 8) | (d[6] << 16) | ((u32)d[7] << 24);
        e->h = d[8] | (d[9] << 8) | (d[10] << 16) | ((u32)d[11] << 24);
        if (len != 12 + (size_t)e->w * e->h * 4) {
                printf("  FATAL expect size mismatch %s\n", path);
                exit(2);
        }
        e->px = (u32 *)(d + 12);
        e->raw = d;
        return true;
}

static void test_png(const char *pngpath)
{
        char epath[256];
        snprintf(epath, sizeof epath, "%s", pngpath);
        size_t n = strlen(epath);
        strcpy(epath + n - 3, "expect");

        RevImage img;
        char err[256];
        if (!rev_png_decode_file(pngpath, &img, err, sizeof err)) {
                CHECK(false, "%s: decode failed: %s", pngpath, err);
                return;
        }

        Expect e;
        load_expect(epath, &e);

        CHECK(img.w == e.w && img.h == e.h, "%s: dims %ux%u != %ux%u",
              pngpath, img.w, img.h, e.w, e.h);

        size_t np = (size_t)img.w * img.h;
        int bad = 0;
        for (size_t i = 0; i < np && bad < 4; i++) {
                if (img.px[i] != e.px[i]) {
                        CHECK(false, "%s: px[%zu] = %08x != %08x", pngpath, i, img.px[i], e.px[i]);
                        bad++;
                }
        }

        rev_image_free(&img);
        free(e.raw);
}

int main(void)
{
        printf("test_png:\n");

        const char *fixtures[] = {
                "png_g1_i0", "png_g2_i0", "png_g4_i0", "png_g8_i0", "png_g16_i0",
                "png_gray8key_i0", "png_rgb8key_i0",
                "png_rgb8_i0", "png_rgb16_i0", "png_rgb8_13x7_i1", "png_rgb8_40x25_i1",
                "png_pal4_i0", "png_pal8_i0", "png_pal4_i1",
                "png_ga8_i0", "png_ga16_i0",
                "png_rgba8_i0", "png_rgba16_i0", "png_rgba8_i1",
        };
        for (size_t i = 0; i < sizeof fixtures / sizeof *fixtures; i++) {
                char p[256];
                snprintf(p, sizeof p, "tests/fixtures/%s.png", fixtures[i]);
                test_png(p);
        }

        /* encoder round-trip */
        {
                RevImage img;
                img.w = 61;
                img.h = 37;
                img.px = (u32 *)malloc((size_t)img.w * img.h * sizeof(u32));
                for (u32 y = 0; y < img.h; y++)
                        for (u32 x = 0; x < img.w; x++)
                                img.px[(size_t)y * img.w + x] =
                                    rev_pack_rgba(rev_rgba((u8)(x * 4), (u8)(y * 6), (u8)((x ^ y) * 3), (u8)(255 - x)));

                u8 *enc = NULL;
                size_t enclen = 0;
                char err[256];
                CHECK(rev_png_encode(&img, &enc, &enclen, err, sizeof err),
                      "encode failed: %s", err);

                RevImage back;
                CHECK(rev_png_decode(enc, enclen, &back, err, sizeof err),
                      "decode of own encode failed: %s", err);
                CHECK(back.w == img.w && back.h == img.h, "roundtrip dims");
                CHECK(memcmp(back.px, img.px, (size_t)img.w * img.h * 4) == 0,
                      "roundtrip pixels differ");

                rev_image_free(&back);
                rev_image_free(&img);
                free(enc);
        }

        if (g_fails == 0)
                printf("  png: OK\n");
        return g_fails != 0;
}
