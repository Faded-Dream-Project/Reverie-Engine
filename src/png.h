/* ============================================================================
 * Reverie Engine — png.h
 * A complete PNG codec (RFC 2083) implemented on top of our own inflater:
 * nothing from libpng. Decodes every standard combination of color type,
 * bit depth and Adam7 interlacing into straight-alpha RGBA8; encodes RGBA8
 * with stored DEFLATE blocks (valid, uncompressed, ideal for tooling).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_PNG_H
#define REV_PNG_H

#include "rev_types.h"

/* RGBA8 image, straight alpha, row-major. */
typedef struct {
	u32 w, h;
	u32 *px;    /* w * h entries, rev_pack_rgba layout */
} RevImage;

void rev_image_free(RevImage *img);

/* Decodes a PNG from memory. On failure returns false and writes a
 * human-readable reason into `err` (if errcap > 0). */
bool rev_png_decode(const u8 *data, size_t len, RevImage *out,
                    char *err, size_t errcap);
bool rev_png_decode_file(const char *path, RevImage *out,
                         char *err, size_t errcap);

/* Encodes an RGBA8 image as a valid PNG. */
bool rev_png_encode(const RevImage *img, u8 **out, size_t *out_len,
                    char *err, size_t errcap);
bool rev_png_encode_file(const char *path, const RevImage *img,
                         char *err, size_t errcap);

/* CRC-32 (as used by PNG chunks). */
u32 rev_crc32(const u8 *data, size_t len);

#endif /* REV_PNG_H */
