/* ============================================================================
 * Reverie Engine — composite.h
 * Pixel compositing: blend modes, blur, resampling, blits.
 * All buffers are RGBA8 straight alpha, packed u32 (rev_pack_rgba).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_COMPOSITE_H
#define REV_COMPOSITE_H

#include "rev_types.h"

typedef struct {
	u32 *px;
	u32 w, h;
} RFb;

enum {
	REV_BLEND_NORMAL = 0,
	REV_BLEND_ADD,
	REV_BLEND_SCREEN,
	REV_BLEND_MULTIPLY,
	REV_BLEND_OVERLAY,
	REV_BLEND_COUNT
};

const char *rev_blend_name(int mode);

/* Blends src over dst (both w x h) with a W3C-style separable blend function
 * and a global opacity multiplier. */
void rev_comp_blend(RFb *dst, const RFb *src, int mode, f64 opacity);

/* Same, but src is placed at (dx, dy) into dst with clipping. */
void rev_comp_blend_at(RFb *dst, const RFb *src, int dx, int dy,
                       int mode, f64 opacity);

/* Theme-switch fade: multiplies RGB by (1 - black), alpha untouched.
 * black=0 is a no-op, 1 turns every pixel its own alpha with zero color —
 * the dip-through-black transition (see docs/IPC.md). Fixed-point integer
 * math, SSE2 when available: it runs every frame for the fade's duration. */
void rev_comp_fade(RFb *dst, f64 black);

/* 3-pass box blur (approximates a gaussian), premultiplied internally. */
void rev_blur(RFb *fb, f64 radius);

/* Bilinear resample. */
void rev_resample_bilinear(const u32 *src, u32 sw, u32 sh, u32 *dst, u32 dw, u32 dh);
/* Nearest neighbor (pixel art). */
void rev_resample_nearest(const u32 *src, u32 sw, u32 sh, u32 *dst, u32 dw, u32 dh);

/* Straight-copy blit with clipping and a 0..255 alpha multiplier. */
void rev_blit(RFb *dst, const RFb *src, int dx, int dy, u32 alpha_mul);

#endif /* REV_COMPOSITE_H */
