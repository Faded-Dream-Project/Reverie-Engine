/* ============================================================================
 * Reverie Engine — tests/test_png.c
 * Decodes every fixture PNG (all color types, depths, filters, Adam7) and
 * compares byte-exact against oracle .expect files; round-trips the encoder;
 * drives every malformed-input refusal path (bad signature, chunk overruns,
 * CRC mismatch, no IDAT, palette without PLTE, tRNS on RGB+A, bad dims,
 * color type 1, interlace 2, filter type 5, size mismatch, Adler mismatch,
 * and the committed decompression bomb — see scripts/mk_png_neg_fixture.py).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"
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

/* ---- hand-built malformed streams --------------------------------------- */

static const u8 PNG_SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

typedef struct { u8 *p; size_t n, cap; } Bytes;

static void bput(Bytes *b, const void *ptr, size_t cnt)
{
        if (cnt == 0)
                return;    /* memcpy(src=NULL, 0) is UB per nonnull attrs */
        if (b->n + cnt > b->cap) {
                size_t nc = b->cap ? b->cap * 2 : 256;
                while (nc < b->n + cnt)
                        nc *= 2;
                u8 *nb = (u8 *)realloc(b->p, nc);
                if (!nb) { printf("  FATAL OOM\n"); exit(2); }
                b->p = nb;
                b->cap = nc;
        }
        memcpy(b->p + b->n, ptr, cnt);
        b->n += cnt;
}

/* Appends a chunk with a CORRECT CRC; returns the offset of the CRC field
 * so callers can corrupt it deliberately. The CRC covers TYPE+DATA as one
 * contiguous region (as in the file layout), so the type bytes are copied
 * ahead of the data here — passing a string literal plus a separate data
 * pointer to rev_crc32 would hash the gap between them instead. */
static size_t bchunk(Bytes *b, const char *type, const void *data, size_t cnt)
{
        u8 td[4 + 512];
        size_t tn = 4 + (cnt > 512 ? 512 : cnt);
        memcpy(td, type, 4);
        if (cnt)
                memcpy(td + 4, data, cnt < 512 ? cnt : 512);
        u8 lb[4] = { (u8)(cnt >> 24), (u8)(cnt >> 16), (u8)(cnt >> 8), (u8)cnt };
        bput(b, lb, 4);
        bput(b, type, 4);
        bput(b, data, cnt);
        u32 crc = rev_crc32(td, tn);
        u8 cb[4] = { (u8)(crc >> 24), (u8)(crc >> 16), (u8)(crc >> 8), (u8)crc };
        bput(b, cb, 4);
        return b->n - 4;
}

/* One stored-block zlib wrapper around raw (n <= 65535). */
static u8 *zwrap(const u8 *raw, size_t n, size_t *zn)
{
        u8 *z = (u8 *)malloc(n + 11);
        if (!z) { printf("  FATAL OOM\n"); exit(2); }
        z[0] = 0x78; z[1] = 0x01;
        z[2] = 1;    /* BFINAL, stored */
        z[3] = (u8)(n & 0xff); z[4] = (u8)(n >> 8);
        z[5] = (u8)(~n & 0xff); z[6] = (u8)((~n >> 8) & 0xff);
        memcpy(z + 7, raw, n);
        u32 ad = rev_adler32(raw, n);
        z[7 + n] = (u8)(ad >> 24); z[8 + n] = (u8)(ad >> 16);
        z[9 + n] = (u8)(ad >> 8); z[10 + n] = (u8)ad;
        *zn = n + 11;
        return z;
}

static void ihdr_of(u8 out[13], u32 w, u32 h, u8 depth, u8 ctype, u8 ilace)
{
        out[0] = (u8)(w >> 24); out[1] = (u8)(w >> 16);
        out[2] = (u8)(w >> 8);  out[3] = (u8)w;
        out[4] = (u8)(h >> 24); out[5] = (u8)(h >> 16);
        out[6] = (u8)(h >> 8);  out[7] = (u8)h;
        out[8] = depth; out[9] = ctype;
        out[10] = 0; out[11] = 0; out[12] = ilace;
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

        /* ---- malformed-input refusals ------------------------------------
         * Every case must fail with the SPECIFIC reason, never crash. The
         * streams are hand-assembled (chunk builder + stored-block zlib), so
         * the negative paths do not depend on the encoder's output. */
        {
                RevImage im;
                char e[256];
                u8 ih[13];
                ihdr_of(ih, 1, 1, 8, 0, 0);
                size_t zn = 0;
                u8 *z = zwrap((const u8 *)"\x00\x80", 2, &zn);

                /* canonical minimal stream: decodes */
                Bytes b = { 0 };
                bput(&b, PNG_SIG, 8);
                bchunk(&b, "IHDR", ih, 13);
                bchunk(&b, "IDAT", z, zn);
                bchunk(&b, "IEND", NULL, 0);
                free(z);
                CHECK(rev_png_decode(b.p, b.n, &im, e, sizeof e) &&
                      im.w == 1 && im.h == 1,
                      "minimal hand-built 1x1 gray decodes");
                if (im.px)
                        CHECK((im.px[0] >> 24) == 0xFF && (im.px[0] & 0xFF) == 0x80,
                              "minimal 1x1 gray pixel value");
                rev_image_free(&im);

                /* a. bad signature */
                b.p[0] = 0x88;
                CHECK(!rev_png_decode(b.p, b.n, &im, e, sizeof e) &&
                      strstr(e, "bad signature"), "refuses bad signature");
                b.p[0] = 137;

                /* b. chunk length overruns the file (IHDR claims 0x7FFFFFFF) */
                b.p[8] = 0x7F; b.p[9] = 0xFF; b.p[10] = 0xFF; b.p[11] = 0xFF;
                CHECK(!rev_png_decode(b.p, b.n, &im, e, sizeof e) &&
                      strstr(e, "overruns"), "refuses chunk overrunning the file");
                b.p[8] = 0; b.p[9] = 0; b.p[10] = 0; b.p[11] = 13;

                /* c. corrupted chunk CRC (the IHDR CRC lives at byte 29) */
                b.p[8 + 8 + 13] ^= 0xFF;
                CHECK(!rev_png_decode(b.p, b.n, &im, e, sizeof e) &&
                      strstr(e, "CRC mismatch"), "refuses corrupted chunk CRC");
                b.p[8 + 8 + 13] ^= 0xFF;

                /* d. no IDAT at all */
                {
                        Bytes bd = { 0 };
                        bput(&bd, PNG_SIG, 8);
                        bchunk(&bd, "IHDR", ih, 13);
                        bchunk(&bd, "IEND", NULL, 0);
                        CHECK(!rev_png_decode(bd.p, bd.n, &im, e, sizeof e) &&
                              strstr(e, "no IDAT"), "refuses stream without IDAT");
                        free(bd.p);
                }

                /* e. palette image without PLTE */
                {
                        u8 ih3[13];
                        ihdr_of(ih3, 1, 1, 8, 3, 0);
                        Bytes bp = { 0 };
                        bput(&bp, PNG_SIG, 8);
                        bchunk(&bp, "IHDR", ih3, 13);
                        u8 *zp = zwrap((const u8 *)"\x00\x00", 2, &zn);
                        bchunk(&bp, "IDAT", zp, zn);
                        bchunk(&bp, "IEND", NULL, 0);
                        CHECK(!rev_png_decode(bp.p, bp.n, &im, e, sizeof e) &&
                              strstr(e, "without PLTE"),
                              "refuses palette image without PLTE");
                        free(zp);
                        free(bp.p);
                }

                /* f. tRNS on color type 6 */
                {
                        u8 ih6[13];
                        ihdr_of(ih6, 1, 1, 8, 6, 0);
                        Bytes bt = { 0 };
                        bput(&bt, PNG_SIG, 8);
                        bchunk(&bt, "IHDR", ih6, 13);
                        bchunk(&bt, "tRNS", "\x00\x00", 2);
                        CHECK(!rev_png_decode(bt.p, bt.n, &im, e, sizeof e) &&
                              strstr(e, "tRNS not allowed"), "refuses tRNS on RGBA");
                        free(bt.p);
                }

                /* g. zero width / over-limit width / over-cap pixel count */
                {
                        u8 ihw[13];
                        ihdr_of(ihw, 0, 1, 8, 0, 0);
                        Bytes bw = { 0 };
                        bput(&bw, PNG_SIG, 8);
                        bchunk(&bw, "IHDR", ihw, 13);
                        CHECK(!rev_png_decode(bw.p, bw.n, &im, e, sizeof e) &&
                              strstr(e, "bad dimensions"), "refuses zero width");
                        free(bw.p);
                }
                {
                        u8 ihw[13];
                        ihdr_of(ihw, 20000, 20000, 8, 0, 0);
                        Bytes bw = { 0 };
                        bput(&bw, PNG_SIG, 8);
                        bchunk(&bw, "IHDR", ihw, 13);
                        CHECK(!rev_png_decode(bw.p, bw.n, &im, e, sizeof e) &&
                              strstr(e, "bad dimensions"), "refuses width over 16384");
                        free(bw.p);
                }
                {
                        u8 ihw[13];
                        ihdr_of(ihw, 16384, 16384, 8, 0, 0);   /* 268 MP */
                        Bytes bw = { 0 };
                        bput(&bw, PNG_SIG, 8);
                        bchunk(&bw, "IHDR", ihw, 13);
                        CHECK(!rev_png_decode(bw.p, bw.n, &im, e, sizeof e) &&
                              strstr(e, "too large"), "refuses over-cap pixel count");
                        free(bw.p);
                }

                /* h. invalid color type 1 / interlace method 2 */
                {
                        u8 ihx[13];
                        ihdr_of(ihx, 1, 1, 8, 1, 0);
                        Bytes bx = { 0 };
                        bput(&bx, PNG_SIG, 8);
                        bchunk(&bx, "IHDR", ihx, 13);
                        CHECK(!rev_png_decode(bx.p, bx.n, &im, e, sizeof e) &&
                              strstr(e, "bad color type"),
                              "refuses invalid color type 1");
                        free(bx.p);
                }
                {
                        u8 ihx[13];
                        ihdr_of(ihx, 1, 1, 8, 0, 2);
                        Bytes bx = { 0 };
                        bput(&bx, PNG_SIG, 8);
                        bchunk(&bx, "IHDR", ihx, 13);
                        CHECK(!rev_png_decode(bx.p, bx.n, &im, e, sizeof e) &&
                              strstr(e, "interlace"), "refuses interlace method 2");
                        free(bx.p);
                }

                /* i. filter type 5 on row 0 */
                {
                        Bytes bf = { 0 };
                        bput(&bf, PNG_SIG, 8);
                        bchunk(&bf, "IHDR", ih, 13);
                        u8 *zf = zwrap((const u8 *)"\x05\x80", 2, &zn);
                        bchunk(&bf, "IDAT", zf, zn);
                        bchunk(&bf, "IEND", NULL, 0);
                        CHECK(!rev_png_decode(bf.p, bf.n, &im, e, sizeof e) &&
                              strstr(e, "bad filter type"), "refuses filter type 5");
                        free(zf);
                        free(bf.p);
                }

                /* j. inflated size mismatch (declares 2x1, carries 1x1) */
                {
                        u8 ih2[13];
                        ihdr_of(ih2, 2, 1, 8, 0, 0);
                        Bytes bs = { 0 };
                        bput(&bs, PNG_SIG, 8);
                        bchunk(&bs, "IHDR", ih2, 13);
                        u8 *zs = zwrap((const u8 *)"\x00\x80", 2, &zn);
                        bchunk(&bs, "IDAT", zs, zn);
                        bchunk(&bs, "IEND", NULL, 0);
                        CHECK(!rev_png_decode(bs.p, bs.n, &im, e, sizeof e) &&
                              strstr(e, "size mismatch"),
                              "refuses short inflated stream");
                        free(zs);
                        free(bs.p);
                }

                /* k. Adler-32 mismatch: the chunk CRC is rebuilt over the
                 * corrupted payload, so the deflate layer's own checksum is
                 * what catches it (flipping the byte after building would
                 * fail at the chunk CRC instead — a different refusal) */
                {
                        Bytes ba = { 0 };
                        bput(&ba, PNG_SIG, 8);
                        bchunk(&ba, "IHDR", ih, 13);
                        u8 *za = zwrap((const u8 *)"\x00\x80", 2, &zn);
                        za[7 + 1] = 0x81;   /* pixel byte, Adler now stale */
                        bchunk(&ba, "IDAT", za, zn);
                        bchunk(&ba, "IEND", NULL, 0);
                        CHECK(!rev_png_decode(ba.p, ba.n, &im, e, sizeof e) &&
                              strstr(e, "inflate failed"),
                              "refuses Adler-32 mismatch");
                        free(za);
                        free(ba.p);
                }

                /* l. the committed decompression bomb (274 bytes -> 200000):
                 * must die at the declared-size cap, not at the OOM killer */
                {
                        RevImage bi;
                        char be[256];
                        CHECK(!rev_png_decode_file("tests/fixtures/png_bomb.png",
                                                   &bi, be, sizeof be),
                              "decompression bomb refused");
                        CHECK(strstr(be, "too large"),
                              "bomb refusal names the output cap (got: %s)", be);
                }

                /* m. missing IEND is a warning, not an error (leniency) */
                {
                        Bytes bm = { 0 };
                        bput(&bm, PNG_SIG, 8);
                        bchunk(&bm, "IHDR", ih, 13);
                        u8 *zm = zwrap((const u8 *)"\x00\x80", 2, &zn);
                        bchunk(&bm, "IDAT", zm, zn);
                        CHECK(rev_png_decode(bm.p, bm.n, &im, e, sizeof e) &&
                              im.w == 1,
                              "stream without IEND still decodes (warned)");
                        rev_image_free(&im);
                        free(zm);
                        free(bm.p);
                }

                free(b.p);
        }

        if (g_fails == 0)
                printf("  png: OK\n");
        return g_fails != 0;
}
