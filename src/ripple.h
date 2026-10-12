/* ============================================================================
 * Reverie Engine — ripple.h
 * Interactive water: a wave-equation height field driven by the pointer,
 * rendered as refraction of the composite below plus specular highlights.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_RIPPLE_H
#define REV_RIPPLE_H

#include "rev_types.h"

typedef struct RevRipple RevRipple;

RevRipple *ripple_new(u32 w, u32 h);
void ripple_free(RevRipple *r);

void ripple_configure(RevRipple *r, f64 strength, f64 damping, f64 radius);

/* Pointer in layer-relative [0..1] coordinates. */
void ripple_pointer(RevRipple *r, f64 x, f64 y, bool pressed);

void ripple_step(RevRipple *r, f64 dt);

/* Applies refraction + highlights: `below` is the composited content under
 * this layer (modified in place is NOT done; output written to `out`,
 * which may be `below` itself only if aliased intentionally). */
void ripple_apply(const RevRipple *r, const u32 *below, u32 *out);

#endif /* REV_RIPPLE_H */
