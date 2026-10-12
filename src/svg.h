/* ============================================================================
 * Reverie Engine — svg.h
 * Self-contained SVG renderer for the "core set": paths (all commands
 * including elliptical arcs), basic shapes, groups, transforms, viewBox,
 * fill/stroke with opacity, fill-rule, dash patterns, and linear/radial
 * gradients. Hand-rolled XML parser; no external libraries.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_SVG_H
#define REV_SVG_H

#include "rev_types.h"

typedef struct RevSvg RevSvg;

/* Parse from memory (NUL-terminated or with explicit length). */
RevSvg *rev_svg_parse(const char *text, size_t len, char *err, size_t errcap);
RevSvg *rev_svg_load(const char *path, char *err, size_t errcap);
void    rev_svg_destroy(RevSvg *svg);

/* Declared intrinsic size (from width/height or viewBox), in user units. */
void    rev_svg_intrinsic_size(const RevSvg *svg, f64 *w, f64 *h);

/* Render into a w x h RGBA8 (straight alpha) buffer. Existing content is
 * composited over (background shows through transparent areas). */
void    rev_svg_render(const RevSvg *svg, u32 *px, u32 w, u32 h);

#endif /* REV_SVG_H */
