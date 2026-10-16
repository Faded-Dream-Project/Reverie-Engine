/* ============================================================================
 * Reverie Engine — inflate.c
 *
 * Single-level 15-bit Huffman lookup tables (128 KiB workspace, malloc'd
 * once per stream) give fast symbol decode without zlib.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "inflate.h"

#include <stdlib.h>
#include <string.h>
#ifdef REV_INF_TRACE
#include <stdio.h>
#define TR(...) fprintf(stderr, __VA_ARGS__)
#else
#define TR(...) ((void)0)
#endif

#define MAXBITS   15
#define TBL_SZ    (1u << MAXBITS)

/* lit/len base lengths and extra bits, codes 257..285 */
static const u16 LBASE[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u8 LEXTRA[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
/* distance bases and extra bits, codes 0..29 */
static const u16 DBASE[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
};
static const u8 DEXTRA[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* order in which code-length code lengths are stored */
static const u8 CLEN_ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

typedef struct {
        /* input */
        const u8 *in;
        size_t    in_len, in_pos;
        u64       bitbuf;
        unsigned  bitcnt;
        /* output */
        u8       *out;
        size_t    out_pos, out_cap;
        /* hard output cap: when non-zero (caller passed an exact `expected`),
         * any write past it fails with REV_INF_TOOBIG instead of growing the
         * buffer. This is the decompression-bomb stop: a 600-byte IDAT that
         * claims to inflate to gigabytes dies at the first byte past the
         * PNG's declared scanline budget, not at the OOM killer. */
        size_t    hard_cap;
        bool      cap_hit;
        /* two huffman table slots (lit, dist); code-length table reuses slot 0 */
        u32      *tbl_a;
        u32      *tbl_b;
} Infl;

#ifdef REV_INF_TRACE
static size_t bitpos(const Infl *s){ return s->in_pos*8 - s->bitcnt; }
#endif

/* --------------------------------------------------------------------------
 * bit-level input
 * ------------------------------------------------------------------------ */

/* Returns -1 when the stream is exhausted. `need` must be <= 16. */
static int getbits(Infl *s, unsigned need)
{
        if (need == 0)
                return 0;
        while (s->bitcnt < need) {
                if (s->in_pos >= s->in_len)
                        return -1;
                s->bitbuf |= (u64)s->in[s->in_pos++] << s->bitcnt;
                s->bitcnt += 8;
        }
        int val = (int)(s->bitbuf & (((u64)1 << need) - 1));
        s->bitbuf >>= need;
        s->bitcnt -= need;
        return val;
}

/* Ensures at least one bit is buffered; returns false at end of input. */
static bool prime(Infl *s)
{
        while (s->bitcnt < MAXBITS) {
                if (s->in_pos >= s->in_len)
                        return s->bitcnt > 0;
                s->bitbuf |= (u64)s->in[s->in_pos++] << s->bitcnt;
                s->bitcnt += 8;
        }
        return true;
}

/* --------------------------------------------------------------------------
 * output
 * ------------------------------------------------------------------------ */

static bool out_reserve(Infl *s, size_t extra)
{
        if (s->out_pos + extra <= s->out_cap)
                return true;
        if (s->hard_cap) {
                /* exact-size contract: never grow past the caller's cap */
                s->cap_hit = true;
                return false;
        }
        size_t ncap = s->out_cap ? s->out_cap * 2 : (size_t)64 * 1024;
        while (ncap < s->out_pos + extra)
                ncap *= 2;
        u8 *nb = (u8 *)realloc(s->out, ncap);
        if (!nb)
                return false;
        s->out = nb;
        s->out_cap = ncap;
        return true;
}

static bool out_byte(Infl *s, u8 b)
{
        if (!out_reserve(s, 1))
                return false;
        s->out[s->out_pos++] = b;
        return true;
}

/* --------------------------------------------------------------------------
 * huffman table construction
 *
 * Entry layout (u32): bits 0..8 symbol, bits 9..13 code length, 0 = invalid.
 * Table is indexed by the next MAXBITS bits of the stream in bit-reversed
 * (i.e. stream) order, so a single load resolves any code of length <= 15.
 * ------------------------------------------------------------------------ */

static int build_table(u32 *tbl, const u8 *lengths, int n)
{
        memset(tbl, 0, TBL_SZ * sizeof(u32));

        unsigned count[MAXBITS + 1] = { 0 };
        for (int i = 0; i < n; i++) {
                if (lengths[i] > MAXBITS)
                        return REV_INF_BADBLOCK;
                count[lengths[i]]++;
        }
        count[0] = 0;   /* RFC 1951: bl_count[0] must not shift the code space */

        /* over-subscription check */
        int left = 1;
        for (unsigned l = 1; l <= MAXBITS; l++) {
                left <<= 1;
                left -= (int)count[l];
                if (left < 0)
                        return REV_INF_BADBLOCK;
        }
        /* an empty table (no codes at all) is left as all-invalid deliberately */

        unsigned nextcode[MAXBITS + 2] = { 0 };
        unsigned code = 0;
        for (unsigned l = 1; l <= MAXBITS; l++) {
                code = (code + count[l - 1]) << 1;
                nextcode[l] = code;
        }

        for (int sym = 0; sym < n; sym++) {
                unsigned l = lengths[sym];
                if (l == 0)
                        continue;
                unsigned c = nextcode[l]++;
                /* bit-reverse c (l bits) to match stream order */
                unsigned rev = 0;
                for (unsigned k = 0; k < l; k++) {
                        rev = (rev << 1) | (c & 1u);
                        c >>= 1;
                }
                u32 entry = ((u32)l << 9) | (u32)sym;
                for (unsigned idx = rev; idx < TBL_SZ; idx += (1u << l))
                        tbl[idx] = entry;
        }
        return REV_INF_OK;
}

/* Decodes one symbol; returns -1 on truncation, -2 on invalid code. */
static int decode_sym(Infl *s, const u32 *tbl)
{
        if (!prime(s))
                return -1;
        u32 e = tbl[s->bitbuf & (TBL_SZ - 1)];
        unsigned len = e >> 9;
        if (len == 0 || len > s->bitcnt)
                return -2;
        s->bitbuf >>= len;
        s->bitcnt -= len;
        return (int)(e & 511);
}

/* --------------------------------------------------------------------------
 * block decoding
 * ------------------------------------------------------------------------ */

static int inflate_stored(Infl *s)
{
        /* drop to byte boundary */
        unsigned drop = s->bitcnt & 7;
        s->bitbuf >>= drop;
        s->bitcnt -= drop;

        /* LEN / NLEN, read a byte at a time through the bit buffer so buffered
         * stream bytes are honored in order. */
        u8 lenb[4];
        for (int i = 0; i < 4; i++) {
                int b = getbits(s, 8);
                if (b < 0)
                        return REV_INF_TRUNCATED;
                lenb[i] = (u8)b;
        }
        u16 len = (u16)(lenb[0] | (lenb[1] << 8));
        u16 nlen = (u16)(lenb[2] | (lenb[3] << 8));
        if ((u16)~nlen != len)
                return REV_INF_BADLEN;

        if (!out_reserve(s, len))
                return s->cap_hit ? REV_INF_TOOBIG : REV_INF_NOMEM;

        /* drain any bytes still sitting in the bit buffer */
        while (len > 0 && s->bitcnt >= 8) {
                s->out[s->out_pos++] = (u8)(s->bitbuf & 0xff);
                s->bitbuf >>= 8;
                s->bitcnt -= 8;
                len--;
        }
        if (len > 0) {
                if (s->in_pos + len > s->in_len)
                        return REV_INF_TRUNCATED;
                memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
                s->in_pos += len;
                s->out_pos += len;
        }
        return REV_INF_OK;
}

static int inflate_codes(Infl *s, const u32 *lit_tbl, const u32 *dist_tbl)
{
        for (;;) {
                int sym = decode_sym(s, lit_tbl);
                if (sym < 0)
                        return sym == -1 ? REV_INF_TRUNCATED : REV_INF_BADBLOCK;

                if (sym < 256) {
                        TR("LIT %d bitpos=%zu\n", sym, bitpos(s));
                        if (!out_byte(s, (u8)sym))
                                return s->cap_hit ? REV_INF_TOOBIG : REV_INF_NOMEM;
                        continue;
                }
                if (sym == 256) {
                        TR("EOB bitpos=%zu\n", bitpos(s));
                        return REV_INF_OK;   /* end of block */
                }

                sym -= 257;
                if (sym >= 29)
                        return REV_INF_BADSYM;
                unsigned length = LBASE[sym];
                if (LEXTRA[sym]) {
                        int ex = getbits(s, LEXTRA[sym]);
                        if (ex < 0)
                                return REV_INF_TRUNCATED;
                        length += (unsigned)ex;
                }

                int dsym = decode_sym(s, dist_tbl);
                if (dsym < 0)
                        return dsym == -1 ? REV_INF_TRUNCATED : REV_INF_BADBLOCK;
                if (dsym >= 30)
                        return REV_INF_BADSYM;
                unsigned dist = DBASE[dsym];
                if (DEXTRA[dsym]) {
                        int ex = getbits(s, DEXTRA[dsym]);
                        if (ex < 0)
                                return REV_INF_TRUNCATED;
                        dist += (unsigned)ex;
                }

                TR("MATCH len=%u dist=%u (lcode=%d dcode=%d) bitpos=%zu\n", length, dist, sym+257, dsym, bitpos(s));
                if ((size_t)dist > s->out_pos)
                        return REV_INF_BADDIST;
                if (!out_reserve(s, length))
                        return s->cap_hit ? REV_INF_TOOBIG : REV_INF_NOMEM;

                u8 *dst = s->out + s->out_pos;
                const u8 *src = s->out + s->out_pos - dist;
                /* overlapping copy must be byte-wise forward */
                for (unsigned k = 0; k < length; k++)
                        dst[k] = src[k];
                s->out_pos += length;
        }
}

static int inflate_fixed(Infl *s)
{
        u8 lengths[288 + 30];
        /* literals: 0..143 -> 8, 144..255 -> 9, 256..279 -> 7, 280..287 -> 8 */
        for (int i = 0; i < 144; i++) lengths[i] = 8;
        for (int i = 144; i < 256; i++) lengths[i] = 9;
        for (int i = 256; i < 280; i++) lengths[i] = 7;
        for (int i = 280; i < 288; i++) lengths[i] = 8;
        int rc = build_table(s->tbl_a, lengths, 288);
        if (rc != REV_INF_OK)
                return rc;
        for (int i = 0; i < 30; i++) lengths[i] = 5;
        rc = build_table(s->tbl_b, lengths, 30);
        if (rc != REV_INF_OK)
                return rc;
        return inflate_codes(s, s->tbl_a, s->tbl_b);
}

static int inflate_dynamic(Infl *s)
{
        int hlit = getbits(s, 5);
        int hdist = getbits(s, 5);
        int hclen = getbits(s, 4);
        if (hlit < 0 || hdist < 0 || hclen < 0)
                return REV_INF_TRUNCATED;
        unsigned nlit = (unsigned)hlit + 257;
        unsigned ndist = (unsigned)hdist + 1;
        TR("DYN hlit=%u hdist=%u\n", nlit, ndist);
        unsigned nclen = (unsigned)hclen + 4;
        if (nlit > 286 || ndist > 30)
                return REV_INF_BADBLOCK;

        u8 cl_lengths[19] = { 0 };
        for (unsigned i = 0; i < nclen; i++) {
                int v = getbits(s, 3);
                if (v < 0)
                        return REV_INF_TRUNCATED;
                cl_lengths[CLEN_ORDER[i]] = (u8)v;
        }
        /* the code-length code always has at least one symbol */
        int rc = build_table(s->tbl_a, cl_lengths, 19);
        if (rc != REV_INF_OK)
                return rc;

        u8 lengths[286 + 30];
        unsigned total = nlit + ndist;
        unsigned i = 0;
        while (i < total) {
                int sym = decode_sym(s, s->tbl_a);
                if (sym < 0)
                        return sym == -1 ? REV_INF_TRUNCATED : REV_INF_BADBLOCK;
                if (sym < 16) {
                        lengths[i++] = (u8)sym;
                } else {
                        unsigned rep, val = 0;
                        if (sym == 16) {
                                if (i == 0)
                                        return REV_INF_BADBLOCK;
                                val = lengths[i - 1];
                                int ex = getbits(s, 2);
                                if (ex < 0) return REV_INF_TRUNCATED;
                                rep = 3 + (unsigned)ex;
                        } else if (sym == 17) {
                                int ex = getbits(s, 3);
                                if (ex < 0) return REV_INF_TRUNCATED;
                                rep = 3 + (unsigned)ex;
                        } else {
                                int ex = getbits(s, 7);
                                if (ex < 0) return REV_INF_TRUNCATED;
                                rep = 11 + (unsigned)ex;
                        }
                        if (i + rep > total)
                                return REV_INF_BADBLOCK;
                        while (rep--)
                                lengths[i++] = (u8)val;
                }
        }
        /* EOB code (256) must exist */
        if (lengths[256] == 0)
                return REV_INF_BADBLOCK;

        rc = build_table(s->tbl_a, lengths, (int)nlit);
        if (rc != REV_INF_OK)
                return rc;
        rc = build_table(s->tbl_b, lengths + nlit, (int)ndist);
        if (rc != REV_INF_OK)
                return rc;
        return inflate_codes(s, s->tbl_a, s->tbl_b);
}

/* --------------------------------------------------------------------------
 * public API
 * ------------------------------------------------------------------------ */

static int inflate_stream(Infl *s)
{
        int final = 0;
        while (!final) {
                int bfinal = getbits(s, 1);
                int btype = getbits(s, 2);
                if (bfinal < 0 || btype < 0)
                        return REV_INF_TRUNCATED;
                final = bfinal;

                int rc;
                switch (btype) {
                case 0: rc = inflate_stored(s); break;
                case 1: rc = inflate_fixed(s);  break;
                case 2: rc = inflate_dynamic(s); break;
                default: rc = REV_INF_BADBLOCK;  break;
                }
                if (rc != REV_INF_OK)
                        return rc;
        }
        return REV_INF_OK;
}

int rev_inflate_raw(const u8 *in, size_t in_len, size_t expected,
                    u8 **out, size_t *out_len)
{
        if (!in || !out || !out_len)
                return REV_INF_BADBLOCK;

        Infl s;
        memset(&s, 0, sizeof s);
        s.in = in;
        s.in_len = in_len;
        /* exact expected size = hard cap (bomb stop); 0 = unknown size,
         * geometric growth (see inflate.h) */
        s.hard_cap = expected;

        if (expected) {
                s.out = (u8 *)malloc(expected ? expected : 1);
                s.out_cap = expected;
        } else {
                s.out = (u8 *)malloc((size_t)64 * 1024);
                s.out_cap = (size_t)64 * 1024;
        }
        s.tbl_a = (u32 *)malloc((size_t)TBL_SZ * sizeof(u32));
        s.tbl_b = (u32 *)malloc((size_t)TBL_SZ * sizeof(u32));
        if (!s.out || !s.tbl_a || !s.tbl_b) {
                free(s.out); free(s.tbl_a); free(s.tbl_b);
                return REV_INF_NOMEM;
        }

        int rc = inflate_stream(&s);
        if (rc == REV_INF_OK) {
                *out = s.out;
                *out_len = s.out_pos;
        } else {
                free(s.out);
        }
        free(s.tbl_a);
        free(s.tbl_b);
        return rc;
}

/* Adler-32 (RFC 1950) — also used to validate PNG IDAT streams. */
u32 rev_adler32(const u8 *data, size_t len)
{
        u32 a = 1, b = 0;
        size_t i = 0;
        while (i < len) {
                size_t chunk = len - i;
                if (chunk > 5550)
                        chunk = 5550;
                for (size_t k = 0; k < chunk; k++) {
                        a += data[i + k];
                        b += a;
                }
                a %= 65521;
                b %= 65521;
                i += chunk;
        }
        return (b << 16) | a;
}

int rev_inflate_zlib(const u8 *in, size_t in_len, size_t expected,
                     u8 **out, size_t *out_len)
{
        if (in_len < 6)
                return REV_INF_TRUNCATED;
        u8 cmf = in[0], flg = in[1];
        if ((cmf & 0x0f) != 8)
                return REV_INF_BADBLOCK;
        if (((u32)cmf * 256 + (u32)flg) % 31 != 0)
                return REV_INF_BADBLOCK;
        if (flg & 0x20)
                return REV_INF_BADBLOCK;   /* preset dictionary: unsupported */

        u8 *buf = NULL;
        size_t buflen = 0;
        int rc = rev_inflate_raw(in + 2, in_len - 6, expected, &buf, &buflen);
        if (rc != REV_INF_OK)
                return rc;

        /* trailer: Adler-32 big-endian */
        u32 want = ((u32)in[in_len - 4] << 24) | ((u32)in[in_len - 3] << 16) |
                   ((u32)in[in_len - 2] << 8) | (u32)in[in_len - 1];
        if (rev_adler32(buf, buflen) != want) {
                free(buf);
                return REV_INF_BADBLOCK;
        }
        *out = buf;
        *out_len = buflen;
        return REV_INF_OK;
}

const char *rev_inflate_strerror(int err)
{
        switch (err) {
        case REV_INF_OK:        return "ok";
        case REV_INF_TRUNCATED: return "stream truncated";
        case REV_INF_BADBLOCK:  return "invalid block/huffman data";
        case REV_INF_BADDIST:   return "back-reference before stream start";
        case REV_INF_BADLEN:    return "stored block length mismatch";
        case REV_INF_NOMEM:     return "out of memory";
        case REV_INF_BADSYM:    return "invalid symbol";
        case REV_INF_TOOBIG:    return "output too large";
        default:                return "unknown error";
        }
}
