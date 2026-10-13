/* ============================================================================
 * Reverie Engine — png.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "png.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"
#include "log.h"
#include "util.h"

#define PNG_SIG_LEN 8
static const u8 PNG_SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

/* Adam7 pass geometry */
static const u8 XOFF[7]  = { 0, 4, 0, 2, 0, 1, 0 };
static const u8 YOFF[7]  = { 0, 0, 4, 0, 2, 0, 1 };
static const u8 XSTEP[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const u8 YSTEP[7] = { 8, 8, 8, 4, 4, 2, 2 };

#define MAX_DIM   16384u
#define MAX_PIXELS (100u * 1000u * 1000u)

void rev_image_free(RevImage *img)
{
        if (img) {
                free(img->px);
                img->px = NULL;
                img->w = img->h = 0;
        }
}

/* --------------------------------------------------------------------------
 * CRC-32 (reflected, poly 0xEDB88320) and chunk helpers
 * ------------------------------------------------------------------------ */

u32 rev_crc32(const u8 *data, size_t len)
{
        static u32 table[256];
        static bool built = false;
        if (!built) {
                for (u32 n = 0; n < 256; n++) {
                        u32 c = n;
                        for (int k = 0; k < 8; k++)
                                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                        table[n] = c;
                }
                built = true;
        }
        u32 crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; i++)
                crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
}

/* --------------------------------------------------------------------------
 * decoding
 * ------------------------------------------------------------------------ */

static bool fail(char *err, size_t errcap, const char *fmt, ...)
{
        if (errcap == 0)
                return false;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errcap, fmt, ap);
        va_end(ap);
        return false;
}

typedef struct {
        u32 w, h;
        int depth, ctype, interlace;
        const u8 *plte;
        size_t plte_n;          /* palette entries */
        const u8 *trns;
        size_t trns_n;
        bool has_key;           /* tRNS transparent-color key (CT 0/2) */
        u32 key[3];             /* native-depth key samples */
} PngInfo;

static int channels_of(int ctype)
{
        switch (ctype) {
        case 0: return 1;   /* gray      */
        case 2: return 3;   /* rgb       */
        case 3: return 1;   /* palette   */
        case 4: return 2;   /* gray+a    */
        case 6: return 4;   /* rgba      */
        default: return 0;
        }
}

static bool depth_valid(int ctype, int depth)
{
        switch (ctype) {
        case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
        case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
        default: return depth == 8 || depth == 16;
        }
}

static u8 paeth(u8 a, u8 b, u8 c)
{
        int p = (int)a + (int)b - (int)c;
        int pa = p > (int)a ? p - (int)a : (int)a - p;
        int pb = p > (int)b ? p - (int)b : (int)b - p;
        int pc = p > (int)c ? p - (int)c : (int)c - p;
        if (pa <= pb && pa <= pc) return a;
        if (pb <= pc) return b;
        return c;
}

/* Reconstruct one filtered scanline in place. bpp = filter unit (bytes). */
static void unfilter_row(u8 *row, const u8 *prev, size_t rowbytes, size_t bpp)
{
        u8 ft = row[0];
        u8 *d = row + 1;
        switch (ft) {
        case 0: /* None */
                break;
        case 1: /* Sub */
                for (size_t i = bpp; i < rowbytes; i++)
                        d[i] = (u8)(d[i] + d[i - bpp]);
                break;
        case 2: /* Up */
                if (prev)
                        for (size_t i = 0; i < rowbytes; i++)
                                d[i] = (u8)(d[i] + prev[i]);
                break;
        case 3: /* Average */
                for (size_t i = 0; i < rowbytes; i++) {
                        int x = d[i];
                        int a = (i >= bpp) ? d[i - bpp] : 0;
                        int b = prev ? prev[i] : 0;
                        d[i] = (u8)(x + ((a + b) >> 1));
                }
                break;
        case 4: /* Paeth */
                for (size_t i = 0; i < rowbytes; i++) {
                        int a = (i >= bpp) ? d[i - bpp] : 0;
                        int b = prev ? prev[i] : 0;
                        int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
                        d[i] = (u8)(d[i] + paeth((u8)a, (u8)b, (u8)c));
                }
                break;
        default:
                break; /* validated earlier */
        }
}

/* Extracts pixel `x` from a reconstructed scanline as straight RGBA8. */
static void extract_px(const PngInfo *pi, const u8 *d, u32 x, u32 *out)
{
        int ch = channels_of(pi->ctype);
        int depth = pi->depth;
        u32 r = 0, g = 0, b = 0, a = 255;

        if (depth == 16) {
                size_t o = (size_t)x * (size_t)ch * 2;
                switch (pi->ctype) {
                case 0:
                        r = g = b = d[o];
                        if (pi->has_key) {
                                u32 native = ((u32)d[o] << 8) | d[o + 1];
                                a = (native == pi->key[0]) ? 0 : 255;
                        }
                        break;
                case 2:
                        r = d[o];     g = d[o + 2]; b = d[o + 4];
                        if (pi->has_key) {
                                u32 nr = ((u32)d[o] << 8)     | d[o + 1];
                                u32 ng = ((u32)d[o + 2] << 8) | d[o + 3];
                                u32 nb = ((u32)d[o + 4] << 8) | d[o + 5];
                                a = (nr == pi->key[0] && ng == pi->key[1] && nb == pi->key[2]) ? 0 : 255;
                        }
                        break;
                case 4:
                        r = g = b = d[o];
                        a = d[o + 2];
                        break;
                case 6:
                        r = d[o]; g = d[o + 2]; b = d[o + 4]; a = d[o + 6];
                        break;
                }
        } else if (depth == 8) {
                size_t o = (size_t)x * (size_t)ch;
                switch (pi->ctype) {
                case 0:
                        r = g = b = d[o];
                        if (pi->has_key)
                                a = ((u32)d[o] == pi->key[0]) ? 0 : 255;
                        break;
                case 2:
                        r = d[o]; g = d[o + 1]; b = d[o + 2];
                        if (pi->has_key)
                                a = (r == pi->key[0] && g == pi->key[1] && b == pi->key[2]) ? 0 : 255;
                        break;
                case 3: {
                        u32 idx = d[o];
                        if (idx >= pi->plte_n) {
                                r = g = b = 0; a = 255;   /* out-of-range index: black */
                        } else {
                                const u8 *e = pi->plte + (size_t)idx * 3;
                                r = e[0]; g = e[1]; b = e[2];
                                a = (idx < pi->trns_n) ? pi->trns[idx] : 255;
                        }
                        break;
                }
                case 4:
                        r = g = b = d[o];
                        a = d[o + 1];
                        break;
                case 6:
                        r = d[o]; g = d[o + 1]; b = d[o + 2]; a = d[o + 3];
                        break;
                }
        } else {
                /* sub-byte gray (1/2/4) or palette (1/2/4) */
                u32 bit = (u32)x * (u32)depth;
                u32 byte = d[bit >> 3];
                u32 shift = 8 - (u32)depth - (bit & 7);
                u32 mask = (1u << depth) - 1;
                u32 val = (byte >> shift) & mask;

                if (pi->ctype == 3) {
                        if (val >= pi->plte_n) {
                                r = g = b = 0; a = 255;
                        } else {
                                const u8 *e = pi->plte + (size_t)val * 3;
                                r = e[0]; g = e[1]; b = e[2];
                                a = (val < pi->trns_n) ? pi->trns[val] : 255;
                        }
                } else {
                        u32 scale = 255 / mask;   /* 1->255, 2->85, 4->17 */
                        r = g = b = val * scale;
                        if (pi->has_key)
                                a = (val == pi->key[0]) ? 0 : 255;
                }
        }
        *out = (a << 24) | (b << 16) | (g << 8) | r;
}

static size_t rowbytes_of(const PngInfo *pi, u32 w)
{
        size_t ch = (size_t)channels_of(pi->ctype);
        return (size_t)((u64)w * (u64)ch * (u64)pi->depth + 7) / 8;
}

/* Decodes the (already inflated) filtered scanlines into `px`. */
static bool decode_scanlines(const PngInfo *pi, const u8 *raw, size_t raw_len,
                             u32 *px, char *err, size_t errcap)
{
        size_t bpp = (size_t)((u64)channels_of(pi->ctype) * (u64)pi->depth + 7) / 8;
        if (bpp < 1)
                bpp = 1;

        if (pi->interlace == 0) {
                size_t rb = rowbytes_of(pi, pi->w);
                if (raw_len != (rb + 1) * (size_t)pi->h)
                        return fail(err, errcap, "PNG: inflated size mismatch (got %zu, want %zu)",
                                    raw_len, (rb + 1) * (size_t)pi->h);

                u8 *row = NULL;
                const u8 *prev = NULL;
                for (u32 y = 0; y < pi->h; y++) {
                        row = (u8 *)(uintptr_t)(raw + (size_t)y * (rb + 1));
                        if (row[0] > 4)
                                return fail(err, errcap, "PNG: bad filter type %u on row %u", row[0], y);
                        unfilter_row(row, prev, rb, bpp);
                        for (u32 x = 0; x < pi->w; x++)
                                extract_px(pi, row + 1, x, &px[(size_t)y * pi->w + x]);
                        prev = row + 1;
                }
                return true;
        }

        /* Adam7 */
        size_t need = 0;
        for (int p = 0; p < 7; p++) {
                u32 pw = (pi->w > XOFF[p]) ? (pi->w - XOFF[p] + XSTEP[p] - 1) / XSTEP[p] : 0;
                u32 ph = (pi->h > YOFF[p]) ? (pi->h - YOFF[p] + YSTEP[p] - 1) / YSTEP[p] : 0;
                if (pw && ph)
                        need += (rowbytes_of(pi, pw) + 1) * (size_t)ph;
        }
        if (raw_len != need)
                return fail(err, errcap, "PNG: inflated size mismatch (interlaced; got %zu, want %zu)",
                            raw_len, need);

        size_t pos = 0;
        for (int p = 0; p < 7; p++) {
                u32 pw = (pi->w > XOFF[p]) ? (pi->w - XOFF[p] + XSTEP[p] - 1) / XSTEP[p] : 0;
                u32 ph = (pi->h > YOFF[p]) ? (pi->h - YOFF[p] + YSTEP[p] - 1) / YSTEP[p] : 0;
                if (pw == 0 || ph == 0)
                        continue;
                size_t rb = rowbytes_of(pi, pw);
                const u8 *prev = NULL;
                for (u32 yy = 0; yy < ph; yy++) {
                        u8 *row = (u8 *)(uintptr_t)(raw + pos);
                        pos += rb + 1;
                        if (row[0] > 4)
                                return fail(err, errcap, "PNG: bad filter type %u in pass %d", row[0], p);
                        unfilter_row(row, prev, rb, bpp);
                        u32 y = YOFF[p] + yy * YSTEP[p];
                        for (u32 xx = 0; xx < pw; xx++) {
                                u32 x = XOFF[p] + xx * XSTEP[p];
                                extract_px(pi, row + 1, xx, &px[(size_t)y * pi->w + x]);
                        }
                        prev = row + 1;
                }
        }
        return true;
}

bool rev_png_decode(const u8 *data, size_t len, RevImage *out,
                    char *err, size_t errcap)
{
        if (err && errcap)
                err[0] = '\0';
        memset(out, 0, sizeof *out);
        if (!data || len < PNG_SIG_LEN || memcmp(data, PNG_SIG, PNG_SIG_LEN) != 0)
                return fail(err, errcap, "PNG: bad signature");

        PngInfo pi;
        memset(&pi, 0, sizeof pi);

        u8 *idat = NULL;
        size_t idat_len = 0, idat_cap = 0;
        bool got_ihdr = false, got_iend = false;

        size_t pos = PNG_SIG_LEN;
        while (pos + 8 <= len) {
                u32 clen = ((u32)data[pos] << 24) | ((u32)data[pos + 1] << 16) |
                           ((u32)data[pos + 2] << 8) | (u32)data[pos + 3];
                const u8 *type = data + pos + 4;
                if ((u64)pos + 12 + (u64)clen > (u64)len)
                        return fail(err, errcap, "PNG: chunk overruns file");
                const u8 *cdata = type + 4;
                u32 want_crc = ((u32)cdata[clen] << 24) | ((u32)cdata[clen + 1] << 16) |
                               ((u32)cdata[clen + 2] << 8) | (u32)cdata[clen + 3];
                u32 have_crc = rev_crc32(type, clen + 4);
                if (have_crc != want_crc)
                        return fail(err, errcap, "PNG: chunk CRC mismatch (%.4s)", (const char *)type);

                if (memcmp(type, "IHDR", 4) == 0) {
                        if (clen != 13)
                                return fail(err, errcap, "PNG: bad IHDR length");
                        pi.w = ((u32)cdata[0] << 24) | ((u32)cdata[1] << 16) | ((u32)cdata[2] << 8) | cdata[3];
                        pi.h = ((u32)cdata[4] << 24) | ((u32)cdata[5] << 16) | ((u32)cdata[6] << 8) | cdata[7];
                        pi.depth = cdata[8];
                        pi.ctype = cdata[9];
                        int compression = cdata[10], filter = cdata[11];
                        pi.interlace = cdata[12];
                        if (pi.w == 0 || pi.h == 0 || pi.w > MAX_DIM || pi.h > MAX_DIM)
                                return fail(err, errcap, "PNG: bad dimensions %ux%u", pi.w, pi.h);
                        if ((u64)pi.w * (u64)pi.h > MAX_PIXELS)
                                return fail(err, errcap, "PNG: image too large (%u x %u)", pi.w, pi.h);
                        if (!depth_valid(pi.ctype, pi.depth))
                                return fail(err, errcap, "PNG: bad depth %d for color type %d", pi.depth, pi.ctype);
                        if (compression != 0 || filter != 0)
                                return fail(err, errcap, "PNG: unknown compression/filter method");
                        if (pi.interlace != 0 && pi.interlace != 1)
                                return fail(err, errcap, "PNG: bad interlace method %d", pi.interlace);
                        got_ihdr = true;
                } else if (memcmp(type, "PLTE", 4) == 0) {
                        if (clen % 3 != 0 || clen / 3 > 256)
                                return fail(err, errcap, "PNG: bad PLTE length %u", clen);
                        pi.plte = cdata;
                        pi.plte_n = clen / 3;
                } else if (memcmp(type, "tRNS", 4) == 0) {
                        if (pi.ctype == 3) {
                                if (clen > 256)
                                        return fail(err, errcap, "PNG: bad tRNS length");
                                pi.trns = cdata;
                                pi.trns_n = clen;
                        } else if (pi.ctype == 0) {
                                if (clen != 2)
                                        return fail(err, errcap, "PNG: bad tRNS length");
                                pi.has_key = true;
                                pi.key[0] = ((u32)cdata[0] << 8) | cdata[1];
                        } else if (pi.ctype == 2) {
                                if (clen != 6)
                                        return fail(err, errcap, "PNG: bad tRNS length");
                                pi.has_key = true;
                                pi.key[0] = ((u32)cdata[0] << 8) | cdata[1];
                                pi.key[1] = ((u32)cdata[2] << 8) | cdata[3];
                                pi.key[2] = ((u32)cdata[4] << 8) | cdata[5];
                        } else {
                                return fail(err, errcap, "PNG: tRNS not allowed for color type %d", pi.ctype);
                        }
                } else if (memcmp(type, "IDAT", 4) == 0) {
                        if (idat_len + clen > idat_cap) {
                                size_t ncap = idat_cap ? idat_cap * 2 : 4096;
                                while (ncap < idat_len + clen)
                                        ncap *= 2;
                                u8 *nb = (u8 *)realloc(idat, ncap);
                                if (!nb) {
                                        free(idat);
                                        return fail(err, errcap, "PNG: out of memory");
                                }
                                idat = nb;
                                idat_cap = ncap;
                        }
                        memcpy(idat + idat_len, cdata, clen);
                        idat_len += clen;
                } else if (memcmp(type, "IEND", 4) == 0) {
                        got_iend = true;
                        pos += 12 + clen;
                        break;
                }
                /* ancillary chunks are skipped */

                pos += 12 + clen;
        }

        if (!got_ihdr)
                return fail(err, errcap, "PNG: missing IHDR");
        if (!got_iend)
                LOGW("PNG stream has no IEND; decoding anyway");
        if (idat_len == 0) {
                free(idat);
                return fail(err, errcap, "PNG: no IDAT chunks");
        }
        if (pi.ctype == 3 && pi.plte == NULL) {
                free(idat);
                return fail(err, errcap, "PNG: palette image without PLTE");
        }

        /* exact expected inflated size */
        size_t expected = 0;
        if (pi.interlace == 0) {
                expected = (rowbytes_of(&pi, pi.w) + 1) * (size_t)pi.h;
        } else {
                for (int p = 0; p < 7; p++) {
                        u32 pw = (pi.w > XOFF[p]) ? (pi.w - XOFF[p] + XSTEP[p] - 1) / XSTEP[p] : 0;
                        u32 ph = (pi.h > YOFF[p]) ? (pi.h - YOFF[p] + YSTEP[p] - 1) / YSTEP[p] : 0;
                        if (pw && ph)
                                expected += (rowbytes_of(&pi, pw) + 1) * (size_t)ph;
                }
        }

        u8 *raw = NULL;
        size_t raw_len = 0;
        int rc = rev_inflate_zlib(idat, idat_len, expected, &raw, &raw_len);
        free(idat);
        if (rc != REV_INF_OK)
                return fail(err, errcap, "PNG: inflate failed: %s", rev_inflate_strerror(rc));

        out->w = pi.w;
        out->h = pi.h;
        out->px = (u32 *)malloc((size_t)pi.w * pi.h * sizeof(u32));
        if (!out->px) {
                free(raw);
                rev_image_free(out);
                return fail(err, errcap, "PNG: out of memory");
        }

        bool ok = decode_scanlines(&pi, raw, raw_len, out->px, err, errcap);
        free(raw);
        if (!ok)
                rev_image_free(out);
        return ok;
}

bool rev_png_decode_file(const char *path, RevImage *out, char *err, size_t errcap)
{
        size_t len = 0;
        u8 *data = rev_read_file(path, &len);
        if (!data)
                return fail(err, errcap, "PNG: cannot read '%s'", path);
        bool ok = rev_png_decode(data, len, out, err, errcap);
        free(data);
        return ok;
}

/* --------------------------------------------------------------------------
 * encoding (RGBA8, stored DEFLATE blocks)
 * ------------------------------------------------------------------------ */

static void put_u32be(u8 *p, u32 v)
{
        p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

typedef struct {
        u8  *buf;
        size_t len, cap;
        bool oom;
} Buf;

static void buf_put(Buf *b, const void *p, size_t n)
{
        /* n == 0 is a no-op append — and must not reach memcpy with a NULL
         * source (IEND chunks pass NULL/0): glibc declares memcpy params
         * __nonnull, so even a zero-length copy from NULL is UB. */
        if (b->oom || n == 0) return;
        if (b->len + n > b->cap) {
                size_t ncap = b->cap ? b->cap * 2 : 1024;
                while (ncap < b->len + n) ncap *= 2;
                u8 *nb = (u8 *)realloc(b->buf, ncap);
                if (!nb) { b->oom = true; return; }
                b->buf = nb;
                b->cap = ncap;
        }
        memcpy(b->buf + b->len, p, n);
        b->len += n;
}

static void buf_chunk(Buf *b, const char *type, const void *data, size_t n)
{
        u8 lenb[4];
        put_u32be(lenb, (u32)n);
        buf_put(b, lenb, 4);
        size_t crc_start = b->len;
        buf_put(b, type, 4);
        buf_put(b, data, n);
        if (b->oom) return;
        u32 crc = rev_crc32(b->buf + crc_start, n + 4);
        u8 crcb[4];
        put_u32be(crcb, crc);
        buf_put(b, crcb, 4);
}

bool rev_png_encode(const RevImage *img, u8 **out, size_t *out_len,
                    char *err, size_t errcap)
{
        if (err && errcap) err[0] = '\0';
        if (!img || !img->px || img->w == 0 || img->h == 0)
                return fail(err, errcap, "PNG encode: empty image");

        /* filtered raw stream: filter byte 0 + row bytes, per row */
        size_t rowb = (size_t)img->w * 4;
        size_t raw_len = (rowb + 1) * (size_t)img->h;
        u8 *raw = (u8 *)malloc(raw_len);
        if (!raw)
                return fail(err, errcap, "PNG encode: out of memory");
        for (u32 y = 0; y < img->h; y++) {
                u8 *dst = raw + (size_t)y * (rowb + 1);
                dst[0] = 0;   /* filter: None */
                memcpy(dst + 1, img->px + (size_t)y * img->w, rowb);
        }

        /* zlib stream with stored blocks */
        Buf z = { 0 };
        u8 zh[2] = { 0x78, 0x01 };   /* CM=8, 32K window, check bits valid */
        buf_put(&z, zh, 2);

        size_t pos = 0;
        while (pos < raw_len) {
                size_t n = raw_len - pos;
                if (n > 65535) n = 65535;
                u8 hdr[5];
                hdr[0] = (pos + n >= raw_len) ? 1 : 0;   /* BFINAL on last block */
                hdr[1] = (u8)(n & 0xff);
                hdr[2] = (u8)(n >> 8);
                hdr[3] = (u8)(~n & 0xff);
                hdr[4] = (u8)((~n >> 8) & 0xff);
                buf_put(&z, hdr, 5);
                buf_put(&z, raw + pos, n);
                pos += n;
        }
        u8 adb[4];
        u32 adler = rev_adler32(raw, raw_len);
        put_u32be(adb, adler);
        buf_put(&z, adb, 4);
        free(raw);

        if (z.oom) {
                free(z.buf);
                return fail(err, errcap, "PNG encode: out of memory");
        }

        Buf outb = { 0 };
        buf_put(&outb, PNG_SIG, PNG_SIG_LEN);
        u8 ihdr[13];
        u8 *p = ihdr;
        put_u32be(p, img->w);        p += 4;
        put_u32be(p, img->h);        p += 4;
        *p++ = 8;                    /* depth */
        *p++ = 6;                    /* RGBA */
        *p++ = 0;                    /* deflate */
        *p++ = 0;                    /* adaptive filtering */
        *p++ = 0;                    /* no interlace */
        buf_chunk(&outb, "IHDR", ihdr, 13);
        buf_chunk(&outb, "IDAT", z.buf, z.len);
        buf_chunk(&outb, "IEND", NULL, 0);
        free(z.buf);

        if (outb.oom) {
                free(outb.buf);
                return fail(err, errcap, "PNG encode: out of memory");
        }
        *out = outb.buf;
        *out_len = outb.len;
        return true;
}

bool rev_png_encode_file(const char *path, const RevImage *img,
                         char *err, size_t errcap)
{
        u8 *data = NULL;
        size_t len = 0;
        if (!rev_png_encode(img, &data, &len, err, errcap))
                return false;
        bool ok = rev_write_file(path, data, len);
        if (!ok)
                fail(err, errcap, "PNG: cannot write '%s'", path);
        free(data);
        return ok;
}
