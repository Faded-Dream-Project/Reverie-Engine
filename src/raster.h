/* ============================================================================
 * Reverie Engine — raster.h
 * Scanline polygon rasterizer with antialiasing (exact horizontal coverage,
 * 4x vertical supersampling), nonzero/evenodd winding, solid + linear +
 * radial gradient paints, and a vector stroker (butt/round/square caps,
 * miter/round/bevel joins, dash patterns).
 *
 * All pixel buffers are RGBA8, straight alpha, packed u32 (rev_pack_rgba),
 * composited with source-over.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_RASTER_H
#define REV_RASTER_H

#include "arena.h"
#include "geom.h"
#include "rev_types.h"

/* ---- paints ---------------------------------------------------------------- */

enum { RPAINT_SOLID = 0, RPAINT_LINEAR, RPAINT_RADIAL };
enum { RSPREAD_PAD = 0, RSPREAD_REFLECT, RSPREAD_REPEAT };

typedef struct { f64 off; RColor c; } RGradStop;

typedef struct {
	u8   kind;              /* RPAINT_* */
	RColor solid;
	/* gradient geometry, DEVICE space */
	Vec2 lp0, lp1;          /* linear endpoints   */
	Vec2 rc, rf;            /* radial center, focus */
	f64  rr;                /* radial radius       */
	RGradStop *stops;
	int  nstops;
	u8   spread;            /* RSPREAD_* */
} RPaint;

/* ---- strokes ---------------------------------------------------------------- */

enum { RCAP_BUTT = 0, RCAP_ROUND, RCAP_SQUARE };
enum { RJOIN_MITER = 0, RJOIN_ROUND, RJOIN_BEVEL };

typedef struct {
	RPaint paint;
	f64 width;
	u8 cap;                 /* RCAP_*  */
	u8 join;                /* RJOIN_* */
	f64 miterlimit;         /* default 4 */
	f64 dash[16];           /* dash pattern, user units */
	int  ndash;
	f64 dashoff;
	bool dashed;
} RStroke;

/* ---- operations -------------------------------------------------------------- */

/* Fill polygon (all contours implicitly closed) with paint. */
void rev_raster_fill(u32 *px, u32 w, u32 h, const RPoly *poly,
                     const RPaint *paint, bool evenodd, RArena *scratch);

/* Fill a whole w x h rectangle with a paint, directly. Same LUT, spread
 * and gradient geometry as rev_raster_fill — identical to rasterizing a
 * full-canvas rectangle into a ZEROED buffer — but without the scanline
 * machinery, and every pixel is WRITTEN, never blended over prior buffer
 * contents (animated gradients rebuilt in place must not smear the
 * previous frame through their transparent stops). This is the hot path
 * for scene gradient layers; rev_raster_fill stays the path for shapes. */
void rev_paint_fill_rect(u32 *px, u32 w, u32 h, const RPaint *p);

/* Stroke polygon contours with the given stroke style. */
void rev_raster_stroke(u32 *px, u32 w, u32 h, const RPoly *poly,
                       const RStroke *st, RArena *scratch);

#endif /* REV_RASTER_H */
