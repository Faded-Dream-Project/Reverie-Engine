/* ============================================================================
 * Reverie Engine — geom.h
 * 2D vector geometry: affine transforms, path containers (with quadratic and
 * cubic segments), adaptive curve flattening, and SVG arc conversion.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_GEOM_H
#define REV_GEOM_H

#include "arena.h"
#include "rev_types.h"

/* ---- affine transform -------------------------------------------------------
 * Column-vector convention, same as SVG's matrix(a,b,c,d,e,f):
 *   x' = a*x + c*y + e
 *   y' = b*x + d*y + f
 * ---------------------------------------------------------------------------*/
typedef struct { f64 a, b, c, d, e, f; } RMat;

RMat rmat_identity(void);
RMat rmat_mul(RMat A, RMat B);            /* A after B (apply B, then A)  */
RMat rmat_invert(RMat M);                 /* identity-ish if singular     */
Vec2 rmat_apply(RMat M, Vec2 p);
RMat rmat_translate(f64 tx, f64 ty);
RMat rmat_scale(f64 sx, f64 sy);
RMat rmat_rotate(f64 radians);
RMat rmat_rotate_at(f64 radians, f64 cx, f64 cy);
RMat rmat_skew_x(f64 radians);
RMat rmat_skew_y(f64 radians);

/* ---- path --------------------------------------------------------------------
 * Paths are sequences of segments; curve segments reference the implicit
 * current point (standard vector-path model). Stored in an arena owned by
 * the caller (the SVG document arena).
 * ---------------------------------------------------------------------------*/
enum {
        RSEG_MOVE = 0,
        RSEG_LINE,
        RSEG_QUAD,    /* p[0]=ctrl, p[1]=end            */
        RSEG_CUBIC,   /* p[0]=ctrl1, p[1]=ctrl2, p[2]=end */
        RSEG_CLOSE
};

typedef struct { u8 kind; Vec2 p[3]; } RSeg;

typedef struct {
        RArena *ar;   /* arena backing the segment array */
        RSeg   *v;
        size_t  n, cap;
} RPath;

RPath *rpath_new(RArena *arena);
void   rpath_move_to(RPath *p, Vec2 pt);
void   rpath_line_to(RPath *p, Vec2 pt);
void   rpath_quad_to(RPath *p, Vec2 ctrl, Vec2 end);
void   rpath_cubic_to(RPath *p, Vec2 c1, Vec2 c2, Vec2 end);
void   rpath_close(RPath *p);
/* SVG elliptical arc, endpoint parameterization, appended as cubics. */
void   rpath_arc_to(RPath *p, f64 rx, f64 ry, f64 xrot_deg,
                    bool large_arc, bool sweep, Vec2 end);

/* Transform every segment's points by M (in place). */
void   rpath_transform(RPath *p, RMat m);

/* Control-point bounding box (upper bound, exact for lines). */
void   rpath_bbox(const RPath *p, bool *has, Vec2 *lo, Vec2 *hi);

/* ---- flattening ----------------------------------------------------------------
 * Converts curves to polylines with a device-space tolerance; CLOSE markers
 * become explicit closing lines. Fill users treat every contour as closed.
 * ---------------------------------------------------------------------------*/
typedef struct {
        Vec2  *pts;
        size_t n;
        bool   closed;   /* was terminated by Z */
} RContour;

typedef struct {
        RContour *cs;
        size_t    n;
} RPoly;

RPoly *rpath_flatten(const RPath *p, RArena *arena, f64 tol);

/* ---- small utilities --------------------------------------------------------*/
f64   rev_vec_dist(Vec2 a, Vec2 b);
/* Total polyline length (for dash patterns). */
f64   rev_polyline_len(const Vec2 *pts, size_t n, bool closed);

#endif /* REV_GEOM_H */
