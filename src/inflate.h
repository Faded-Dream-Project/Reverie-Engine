/* ============================================================================
 * Reverie Engine — inflate.h
 * A complete, self-contained DEFLATE (RFC 1951) inflater. This is the piece
 * that removes zlib from the dependency list: PNG IDAT streams are inflated
 * by this file alone.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_INFLATE_H
#define REV_INFLATE_H

#include "rev_types.h"

/* Error codes returned by rev_inflate_raw(). */
enum {
        REV_INF_OK          = 0,
        REV_INF_TRUNCATED   = -1,  /* input ended mid-stream            */
        REV_INF_BADBLOCK    = -2,  /* invalid block type / huffman data */
        REV_INF_BADDIST     = -3,  /* back-reference before start       */
        REV_INF_BADLEN      = -4,  /* stored block LEN/NLEN mismatch    */
        REV_INF_NOMEM       = -5,
        REV_INF_BADSYM      = -6,  /* symbol has no valid code          */
        REV_INF_TOOBIG      = -7   /* output exceeded caller cap        */
};

const char *rev_inflate_strerror(int err);

/* Inflates a raw DEFLATE stream.
 *
 *   expected: exact output size if known — enforced as a HARD CAP: any
 *             byte past it fails with REV_INF_TOOBIG instead of growing
 *             the buffer (the decompression-bomb stop). PNG always knows
 *             the exact scanline budget. 0 = unknown size, buffer grows
 *             geometrically from 64 KiB (no cap).
 *   out:      receives a malloc'd buffer (caller frees). Unchanged on error.
 *   out_len:  receives the inflated size (may be < expected; the caller's
 *             size check catches short streams).
 */
int rev_inflate_raw(const u8 *in, size_t in_len, size_t expected,
                    u8 **out, size_t *out_len);

/* Inflates a zlib (RFC 1950) wrapped stream; verifies the Adler-32 trailer.
 * CM must be 8; FDIST dictionaries are rejected. */
int rev_inflate_zlib(const u8 *in, size_t in_len, size_t expected,
                     u8 **out, size_t *out_len);

/* Adler-32 (RFC 1950). Exposed for PNG trailer verification elsewhere. */
u32 rev_adler32(const u8 *data, size_t len);

#endif /* REV_INFLATE_H */
