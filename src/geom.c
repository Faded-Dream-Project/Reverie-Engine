/* ============================================================================
 * Reverie Engine — geom.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "geom.h"

#include <math.h>
#include <string.h>

#define REV_PI 3.14159265358979323846

/* --------------------------------------------------------------------------
 * transforms
 * ------------------------------------------------------------------------ */

RMat rmat_identity(void)
{
	RMat m = { 1, 0, 0, 1, 0, 0 };
	return m;
}

RMat rmat_mul(RMat A, RMat B)
{
	/* (A after B) */
	RMat r;
	r.a = A.a * B.a + A.c * B.b;
	r.b = A.b * B.a + A.d * B.b;
	r.c = A.a * B.c + A.c * B.d;
	r.d = A.b * B.c + A.d * B.d;
	r.e = A.a * B.e + A.c * B.f + A.e;
	r.f = A.b * B.e + A.d * B.f + A.f;
	return r;
}

RMat rmat_invert(RMat m)
{
	f64 det = m.a * m.d - m.b * m.c;
	if (fabs(det) < 1e-12)
		return rmat_identity();
	f64 inv = 1.0 / det;
	RMat r;
	r.a =  m.d * inv;
	r.b = -m.b * inv;
	r.c = -m.c * inv;
	r.d =  m.a * inv;
	r.e = (m.c * m.f - m.d * m.e) * inv;
	r.f = (m.b * m.e - m.a * m.f) * inv;
	return r;
}

Vec2 rmat_apply(RMat m, Vec2 p)
{
	Vec2 r;
	r.x = m.a * p.x + m.c * p.y + m.e;
	r.y = m.b * p.x + m.d * p.y + m.f;
	return r;
}

RMat rmat_translate(f64 tx, f64 ty)     { RMat m = { 1, 0, 0, 1, tx, ty }; return m; }
RMat rmat_scale(f64 sx, f64 sy)         { RMat m = { sx, 0, 0, sy, 0, 0 }; return m; }
RMat rmat_rotate(f64 rad)
{
	f64 c = cos(rad), s = sin(rad);
	RMat m = { c, s, -s, c, 0, 0 };
	return m;
}

RMat rmat_rotate_at(f64 rad, f64 cx, f64 cy)
{
	return rmat_mul(rmat_translate(cx, cy),
	                rmat_mul(rmat_rotate(rad), rmat_translate(-cx, -cy)));
}

RMat rmat_skew_x(f64 rad)
{
	RMat m = { 1, 0, tan(rad), 1, 0, 0 };
	return m;
}

RMat rmat_skew_y(f64 rad)
{
	RMat m = { 1, tan(rad), 0, 1, 0, 0 };
	return m;
}

/* --------------------------------------------------------------------------
 * path construction
 * ------------------------------------------------------------------------ */

RPath *rpath_new(RArena *arena)
{
	RPath *p = (RPath *)arena_zalloc(arena, sizeof(RPath));
	p->ar = arena;
	return p;
}

static void rpath_grow(RPath *p)
{
	size_t ncap = p->cap ? p->cap * 2 : 64;
	RSeg *nv = (RSeg *)arena_alloc(p->ar, ncap * sizeof(RSeg));
	if (p->n)
		memcpy(nv, p->v, p->n * sizeof(RSeg));
	p->v = nv;
	p->cap = ncap;
}

static void rpath_add(RPath *p, u8 kind, Vec2 a, Vec2 b, Vec2 c)
{
	if (p->n == p->cap)
		rpath_grow(p);
	RSeg *s = &p->v[p->n++];
	s->kind = kind;
	s->p[0] = a;
	s->p[1] = b;
	s->p[2] = c;
}

void rpath_move_to(RPath *p, Vec2 pt)         { rpath_add(p, RSEG_MOVE, pt, pt, pt); }
void rpath_line_to(RPath *p, Vec2 pt)         { rpath_add(p, RSEG_LINE, pt, pt, pt); }
void rpath_quad_to(RPath *p, Vec2 c, Vec2 e)  { rpath_add(p, RSEG_QUAD, c, e, e); }
void rpath_cubic_to(RPath *p, Vec2 c1, Vec2 c2, Vec2 e) { rpath_add(p, RSEG_CUBIC, c1, c2, e); }

void rpath_close(RPath *p)
{
	/* A marker only; flatten/stroke track the subpath start themselves. */
	Vec2 z = { 0, 0 };
	rpath_add(p, RSEG_CLOSE, z, z, z);
}

void rpath_transform(RPath *p, RMat m)
{
	for (size_t i = 0; i < p->n; i++) {
		RSeg *s = &p->v[i];
		for (int k = 0; k < 3; k++)
			s->p[k] = rmat_apply(m, s->p[k]);
	}
}

void rpath_bbox(const RPath *p, bool *has, Vec2 *lo, Vec2 *hi)
{
	*has = false;
	for (size_t i = 0; i < p->n; i++) {
		const RSeg *s = &p->v[i];
		int npt = (s->kind == RSEG_CUBIC) ? 3 : (s->kind == RSEG_QUAD ? 2 : 1);
		for (int k = 0; k < npt; k++) {
			Vec2 v = s->p[k];
			if (!*has) {
				*lo = *hi = v;
				*has = true;
			} else {
				if (v.x < lo->x) lo->x = v.x;
				if (v.y < lo->y) lo->y = v.y;
				if (v.x > hi->x) hi->x = v.x;
				if (v.y > hi->y) hi->y = v.y;
			}
		}
	}
}

/* --------------------------------------------------------------------------
 * curve flattening
 * ------------------------------------------------------------------------ */

static f64 point_line_dist(Vec2 p, Vec2 a, Vec2 b)
{
	f64 dx = b.x - a.x, dy = b.y - a.y;
	f64 len2 = dx * dx + dy * dy;
	if (len2 < 1e-18)
		return rev_vec_dist(p, a);
	f64 t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
	t = rev_clampf(t, 0.0, 1.0);
	Vec2 q = { a.x + t * dx, a.y + t * dy };
	return rev_vec_dist(p, q);
}

typedef struct {
	Vec2 *pts;
	size_t n, cap;
	RArena *ar;
} PtBuf;

static void ptbuf_push(PtBuf *b, Vec2 v)
{
	if (b->n == b->cap) {
		size_t ncap = b->cap ? b->cap * 2 : 256;
		Vec2 *nv = (Vec2 *)arena_alloc(b->ar, ncap * sizeof(Vec2));
		if (b->n)
			memcpy(nv, b->pts, b->n * sizeof(Vec2));
		b->pts = nv;
		b->cap = ncap;
	}
	b->pts[b->n++] = v;
}

static void flatten_cubic(PtBuf *b, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, f64 tol)
{
	f64 d1 = point_line_dist(p1, p0, p3);
	f64 d2 = point_line_dist(p2, p0, p3);
	if (d1 * d1 + d2 * d2 <= tol * tol) {
		ptbuf_push(b, p3);
		return;
	}
	/* De Casteljau split at t=0.5 */
	Vec2 ab = { (p0.x + p1.x) * .5, (p0.y + p1.y) * .5 };
	Vec2 bc = { (p1.x + p2.x) * .5, (p1.y + p2.y) * .5 };
	Vec2 cd = { (p2.x + p3.x) * .5, (p2.y + p3.y) * .5 };
	Vec2 abc = { (ab.x + bc.x) * .5, (ab.y + bc.y) * .5 };
	Vec2 bcd = { (bc.x + cd.x) * .5, (bc.y + cd.y) * .5 };
	Vec2 mid = { (abc.x + bcd.x) * .5, (abc.y + bcd.y) * .5 };
	flatten_cubic(b, p0, ab, abc, mid, tol);
	flatten_cubic(b, mid, bcd, cd, p3, tol);
}

static void flatten_quad(PtBuf *b, Vec2 p0, Vec2 p1, Vec2 p2, f64 tol)
{
	f64 d = point_line_dist(p1, p0, p2);
	if (d * d <= tol * tol) {
		ptbuf_push(b, p2);
		return;
	}
	Vec2 a = { (p0.x + p1.x) * .5, (p0.y + p1.y) * .5 };
	Vec2 c = { (p1.x + p2.x) * .5, (p1.y + p2.y) * .5 };
	Vec2 m = { (a.x + c.x) * .5, (a.y + c.y) * .5 };
	flatten_quad(b, p0, a, m, tol);
	flatten_quad(b, m, c, p2, tol);
}

RPoly *rpath_flatten(const RPath *p, RArena *arena, f64 tol)
{
	RPoly *poly = (RPoly *)arena_zalloc(arena, sizeof(RPoly));
	size_t ncont = 0;
	for (size_t i = 0; i < p->n; i++)
		if (p->v[i].kind == RSEG_MOVE || p->v[i].kind == RSEG_CLOSE)
			ncont++;
	if (ncont == 0 && p->n > 0)
		ncont = 1;
	if (ncont == 0)
		return poly;

	RContour *cs = (RContour *)arena_alloc(arena, ncont * sizeof(RContour));
	memset(cs, 0, ncont * sizeof(RContour));
	poly->cs = cs;
	poly->n = 0;

	Vec2 cur = { 0, 0 };
	bool in_contour = false;
	PtBuf buf = { 0 };
	buf.ar = arena;

	for (size_t i = 0; i < p->n; i++) {
		const RSeg *s = &p->v[i];
		switch (s->kind) {
		case RSEG_MOVE:
			if (in_contour && buf.n > 1) {
				cs[poly->n].pts = buf.pts;
				cs[poly->n].n = buf.n;
				cs[poly->n].closed = false;
				poly->n++;
			}
			buf.pts = NULL; buf.n = 0; buf.cap = 0;
			cur = s->p[0];
			ptbuf_push(&buf, cur);
			in_contour = true;
			break;
		case RSEG_LINE:
			if (!in_contour) break;
			ptbuf_push(&buf, s->p[0]);
			cur = s->p[0];
			break;
		case RSEG_QUAD:
			if (!in_contour) break;
			flatten_quad(&buf, cur, s->p[0], s->p[1], tol);
			cur = s->p[1];
			break;
		case RSEG_CUBIC:
			if (!in_contour) break;
			flatten_cubic(&buf, cur, s->p[0], s->p[1], s->p[2], tol);
			cur = s->p[2];
			break;
		case RSEG_CLOSE:
			if (in_contour && buf.n > 1) {
				cs[poly->n].pts = buf.pts;
				cs[poly->n].n = buf.n;
				cs[poly->n].closed = true;
				poly->n++;
			}
			buf.pts = NULL; buf.n = 0; buf.cap = 0;
			in_contour = false;
			break;
		}
	}
	if (in_contour && buf.n > 1) {
		cs[poly->n].pts = buf.pts;
		cs[poly->n].n = buf.n;
		cs[poly->n].closed = false;
		poly->n++;
	}
	return poly;
}

f64 rev_vec_dist(Vec2 a, Vec2 b)
{
	return hypot(b.x - a.x, b.y - a.y);
}

f64 rev_polyline_len(const Vec2 *pts, size_t n, bool closed)
{
	f64 L = 0;
	for (size_t i = 1; i < n; i++)
		L += rev_vec_dist(pts[i - 1], pts[i]);
	if (closed && n > 1)
		L += rev_vec_dist(pts[n - 1], pts[0]);
	return L;
}

/* --------------------------------------------------------------------------
 * SVG arc -> cubic bezier (endpoint parameterization, SVG spec appendix F)
 * ------------------------------------------------------------------------ */

static Vec2 path_current_pt(const RPath *p)
{
	if (p->n == 0)
		return (Vec2){ 0, 0 };
	const RSeg *last = &p->v[p->n - 1];
	return (last->kind == RSEG_CUBIC) ? last->p[2]
	     : (last->kind == RSEG_QUAD)  ? last->p[1]
	     : last->p[0];
}

void rpath_arc_to(RPath *p, f64 rx, f64 ry, f64 xrot_deg,
                  bool large_arc, bool sweep, Vec2 end)
{
	Vec2 start = path_current_pt(p);

	if (rx == 0 || ry == 0 || (start.x == end.x && start.y == end.y)) {
		rpath_line_to(p, end);
		return;
	}
	rx = fabs(rx);
	ry = fabs(ry);

	f64 phi = xrot_deg * REV_PI / 180.0;
	f64 cosphi = cos(phi), sinphi = sin(phi);

	f64 dx2 = (start.x - end.x) / 2.0;
	f64 dy2 = (start.y - end.y) / 2.0;
	f64 x1p =  cosphi * dx2 + sinphi * dy2;
	f64 y1p = -sinphi * dx2 + cosphi * dy2;

	/* correct out-of-range radii */
	f64 lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
	if (lambda > 1.0) {
		f64 s = sqrt(lambda);
		rx *= s;
		ry *= s;
	}

	f64 sign = (large_arc != sweep) ? 1.0 : -1.0;
	f64 num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
	f64 den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
	f64 co = (num < 0 || den <= 0) ? 0.0 : sign * sqrt(num / den);

	f64 cxp = co * rx * y1p / ry;
	f64 cyp = -co * ry * x1p / rx;

	f64 cx = cosphi * cxp - sinphi * cyp + (start.x + end.x) / 2.0;
	f64 cy = sinphi * cxp + cosphi * cyp + (start.y + end.y) / 2.0;

	f64 ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
	f64 vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;

	f64 theta1 = atan2(uy, ux);
	f64 dtheta = atan2(vy, vx) - theta1;

	/* normalize so that: sweep => positive in (0, 2pi), !sweep => negative */
	if (!sweep && dtheta > 0)
		dtheta -= 2.0 * REV_PI;
	else if (sweep && dtheta < 0)
		dtheta += 2.0 * REV_PI;

	/* split into segments of <= 90 degrees */
	int nseg = (int)ceil(fabs(dtheta) / (REV_PI / 2.0));
	if (nseg < 1) nseg = 1;
	f64 delta = dtheta / (f64)nseg;
	f64 t = 4.0 / 3.0 * tan(delta / 4.0);

	f64 ang = theta1;
	for (int i = 0; i < nseg; i++) {
		f64 ang2 = ang + delta;
		f64 s1 = sin(ang),  c1 = cos(ang);
		f64 s2 = sin(ang2), c2 = cos(ang2);

		Vec2 e1 = { cx + rx * c1 * cosphi - ry * s1 * sinphi,
		            cy + rx * c1 * sinphi + ry * s1 * cosphi };
		Vec2 e2 = { cx + rx * c2 * cosphi - ry * s2 * sinphi,
		            cy + rx * c2 * sinphi + ry * s2 * cosphi };

		Vec2 d1 = { -rx * s1, ry * c1 };
		Vec2 d2 = { -rx * s2, ry * c2 };
		Vec2 rd1 = { d1.x * cosphi - d1.y * sinphi, d1.x * sinphi + d1.y * cosphi };
		Vec2 rd2 = { d2.x * cosphi - d2.y * sinphi, d2.x * sinphi + d2.y * cosphi };

		Vec2 c1p = { e1.x + t * rd1.x, e1.y + t * rd1.y };
		Vec2 c2p = { e2.x - t * rd2.x, e2.y - t * rd2.y };

		rpath_cubic_to(p, c1p, c2p, e2);
		ang = ang2;
	}
}
