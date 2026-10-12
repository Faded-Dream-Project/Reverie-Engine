/* ============================================================================
 * Reverie Engine — raster.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef RASTER_TRACE
#include <stdio.h>
#endif

#define SUBSAMPLE 4   /* vertical supersampling factor */

/* --------------------------------------------------------------------------
 * pixel compositing (straight alpha, source-over)
 * ------------------------------------------------------------------------ */

static inline u32 pack(u8 r, u8 g, u8 b, u8 a)
{
	return ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

static inline void blend_px(u32 *dst, u8 r, u8 g, u8 b, u8 a)
{
	if (a == 0)
		return;
	u32 d = *dst;
	u8 da = (u8)(d >> 24);
	if (da == 0) {
		*dst = pack(r, g, b, a);
		return;
	}
	u32 sa = a, da32 = da;
	u32 oa = sa + (u32)(((u64)da32 * (255 - sa) + 127) >> 8);
	if (oa > 255) oa = 255;

	u32 sr = (u32)r, sg = (u32)g, sb = (u32)b;
	u32 dr = (d & 0xff), dg = ((d >> 8) & 0xff), db = ((d >> 16) & 0xff);

	/* out = (s*sa + d*da*(1-sa)) / out_a, per channel, with rounding */
	u64 den = (u64)oa * 255;
	u64 numr = (u64)sr * sa * 255 + (u64)dr * da32 * (255 - sa);
	u64 numg = (u64)sg * sa * 255 + (u64)dg * da32 * (255 - sa);
	u64 numb = (u64)sb * sa * 255 + (u64)db * da32 * (255 - sa);
	u64 vr = numr / den, vg = numg / den, vb = numb / den;
	u8 or_ = (u8)(vr > 255 ? 255 : vr);
	u8 og = (u8)(vg > 255 ? 255 : vg);
	u8 ob = (u8)(vb > 255 ? 255 : vb);
	*dst = pack(or_, og, ob, (u8)oa);
}

/* --------------------------------------------------------------------------
 * gradient LUT
 * ------------------------------------------------------------------------ */

typedef struct {
	u32 entries[256];      /* straight RGBA */
	bool valid;
} GradLut;

static f64 apply_spread(f64 t, u8 spread)
{
	if (spread == RSPREAD_PAD)
		return rev_clampf(t, 0.0, 1.0);
	if (spread == RSPREAD_REPEAT) {
		t = fmod(t, 1.0);
		if (t < 0) t += 1.0;
		return t;
	}
	/* reflect */
	t = fmod(t, 2.0);
	if (t < 0) t += 2.0;
	return t > 1.0 ? 2.0 - t : t;
}

static void build_lut(const RPaint *p, GradLut *lut)
{
	if (p->kind == RPAINT_SOLID || p->nstops <= 0)
		return;
	lut->valid = true;

	/* ensure sorted stops */
	RGradStop st[64];
	memset(st, 0, sizeof st);
	int n = p->nstops > 64 ? 64 : p->nstops;
	for (int i = 0; i < n; i++)
		st[i] = p->stops[i];
	for (int i = 1; i < n; i++) {
		RGradStop tmp = st[i];
		int j = i - 1;
		while (j >= 0 && st[j].off > tmp.off) {
			st[j + 1] = st[j];
			j--;
		}
		st[j + 1] = tmp;
	}
	if (st[0].off > 0) {   /* implicit first stop at 0 in first color */
		for (int i = n; i > 0; i--) st[i] = st[i - 1];
		st[0].off = 0.0; st[0].c = st[1].c;
		n++;
	}
	if (st[n - 1].off < 1) {
		st[n].off = 1.0; st[n].c = st[n - 1].c;
		n++;
	}
	if (n > 64) n = 64;

	for (int i = 0; i < 256; i++) {
		f64 t = apply_spread(i / 255.0, p->spread);
		RColor c = st[0].c;
		for (int k = 1; k < n; k++) {
			if (t <= st[k].off) {
				f64 span = st[k].off - st[k - 1].off;
				f64 f = span > 1e-12 ? (t - st[k - 1].off) / span : 1.0;
				c = rev_lerp_color(st[k - 1].c, st[k].c, rev_clampf(f, 0.0, 1.0));
				break;
			}
			c = st[k].c;
		}
		lut->entries[i] = pack(c.r, c.g, c.b, c.a);
	}
#ifdef RASTER_TRACE
	fprintf(stderr, "build_lut: n=%d stop0=(%f,#%02x%02x%02x) stopN=(%f,#%02x%02x%02x) lut[0]=%08x lut[128]=%08x lut[255]=%08x\n",
	        n, st[0].off, st[0].c.r, st[0].c.g, st[0].c.b,
	        st[n-1].off, st[n-1].c.r, st[n-1].c.g, st[n-1].c.b,
	        lut->entries[0], lut->entries[128], lut->entries[255]);
#endif
}

/* gradient parameter t at a device-space point */
static inline f64 paint_t(const RPaint *p, f64 x, f64 y)
{
	if (p->kind == RPAINT_LINEAR) {
		f64 dx = p->lp1.x - p->lp0.x, dy = p->lp1.y - p->lp0.y;
		f64 len2 = dx * dx + dy * dy;
		if (len2 < 1e-18)
			return 0.0;
		return ((x - p->lp0.x) * dx + (y - p->lp0.y) * dy) / len2;
	}
	/* radial with focus: t = |FP| / |FQ| where Q is the circle intersection
	 * of the ray F->P */
	f64 px = x - p->rf.x, py = y - p->rf.y;
	f64 dlen = hypot(px, py);
	if (dlen < 1e-12 || p->rr < 1e-12)
		return 0.0;
	f64 ux = px / dlen, uy = py / dlen;
	/* solve |F + u*t' - C| = r for t' (take the positive root) */
	f64 fx = p->rf.x - p->rc.x, fy = p->rf.y - p->rc.y;
	f64 b = fx * ux + fy * uy;
	f64 c = fx * fx + fy * fy - p->rr * p->rr;
	f64 disc = b * b - c;
	if (disc <= 0)
		return 1.0;
	f64 tp = -b + sqrt(disc);
	if (tp <= 1e-12)
		return 1.0;
	return dlen / tp;
}

/* --------------------------------------------------------------------------
 * scanline fill
 * ------------------------------------------------------------------------ */

typedef struct { f64 x; i32 sign; } Xing;

static int xing_cmp(const void *a, const void *b)
{
	const Xing *xa = (const Xing *)a, *xb = (const Xing *)b;
	if (xa->x < xb->x) return -1;
	if (xa->x > xb->x) return 1;
	return 0;
}

typedef struct {
	Vec2 a, b;      /* a.y < b.y (normalized downward or upward kept by sign) */
	i32 sign;       /* +1 if original edge went downward */
} Edge;

/* The fill core. `py` is the device y of the pixel row (used for gradients). */
static void fill_row(u32 *rowpx, u32 w, const f64 *acc, f64 py,
                     const RPaint *p, const GradLut *lut)
{
	u32 sr = 0, sg = 0, sb = 0, sa = 0;
	if (p->kind == RPAINT_SOLID) {
		sr = p->solid.r; sg = p->solid.g; sb = p->solid.b; sa = p->solid.a;
	}

	for (u32 i = 0; i < w; i++) {
		f64 cov = acc[i];
		if (cov <= 1.0 / 4096.0)
			continue;
		if (cov > 1.0) cov = 1.0;

		if (p->kind == RPAINT_SOLID) {
			u8 a = (u8)((sa * cov) + 0.5);
			if (a == 0) continue;
			blend_px(&rowpx[i], (u8)sr, (u8)sg, (u8)sb, a);
		} else {
			f64 t = apply_spread(paint_t(p, (f64)i + 0.5, py + 0.5), p->spread);
			u32 e = lut->entries[(int)(t * 255.0 + 0.5) & 0xff];
			u8 er = (u8)(e & 0xff), eg = (u8)((e >> 8) & 0xff);
			u8 eb = (u8)((e >> 16) & 0xff), ea = (u8)(e >> 24);
			u8 a = (u8)(((u32)ea * cov) + 0.5);
			if (a == 0) continue;
			blend_px(&rowpx[i], er, eg, eb, a);
		}
	}
}

void rev_raster_fill(u32 *px, u32 w, u32 h, const RPoly *poly,
                     const RPaint *paint, bool evenodd, RArena *scratch)
{
	if (paint->kind == RPAINT_SOLID) {
		if (paint->solid.a == 0)
			return;
	} else if (paint->nstops == 0) {
		return;
	}

	GradLut lut;
	memset(&lut, 0, sizeof lut);
	build_lut(paint, &lut);

	/* flatten all contours into an edge array */
	size_t nedges = 0;
	for (size_t c = 0; c < poly->n; c++)
		nedges += poly->cs[c].n;
	if (nedges == 0)
		return;

	Edge *edges = (Edge *)arena_alloc(scratch, nedges * sizeof(Edge));
	size_t ne = 0;
	f64 miny = 1e30, maxy = -1e30;
	for (size_t c = 0; c < poly->n; c++) {
		const Vec2 *pts = poly->cs[c].pts;
		size_t n = poly->cs[c].n;
		for (size_t i = 0; i < n; i++) {
			Vec2 a = pts[i];
			Vec2 b = pts[(i + 1) % n];   /* fill always closes */
			if (a.y == b.y)
				continue;
			Edge e;
			if (a.y < b.y) {
				e.a = a; e.b = b; e.sign = 1;
			} else {
				e.a = b; e.b = a; e.sign = -1;
			}
			edges[ne++] = e;
			if (e.a.y < miny) miny = e.a.y;
			if (e.b.y > maxy) maxy = e.b.y;
		}
	}
	if (ne == 0)
		return;

	i32 j0 = (i32)floor(miny);
	i32 j1 = (i32)ceil(maxy);
	if (j0 < 0) j0 = 0;
	if (j1 > (i32)h) j1 = (i32)h;

	f64 *acc = (f64 *)arena_alloc(scratch, (size_t)w * sizeof(f64));
	Xing *xings = (Xing *)arena_alloc(scratch, (ne + 1) * sizeof(Xing));

	for (i32 j = j0; j < j1; j++) {
		memset(acc, 0, (size_t)w * sizeof(f64));

		for (int s = 0; s < SUBSAMPLE; s++) {
			f64 ys = (f64)j + (s + 0.5) / SUBSAMPLE;
			size_t nx = 0;
			for (size_t e = 0; e < ne; e++) {
				if (edges[e].a.y <= ys && ys < edges[e].b.y) {
					f64 t = (ys - edges[e].a.y) / (edges[e].b.y - edges[e].a.y);
					xings[nx].x = edges[e].a.x + t * (edges[e].b.x - edges[e].a.x);
					xings[nx].sign = edges[e].sign;
					nx++;
				}
			}
			if (nx < 2)
				continue;
			qsort(xings, nx, sizeof(Xing), xing_cmp);

			/* sweep */
			i32 wind = 0;
			f64 xl = xings[0].x;
			i32 w0 = xings[0].sign;
			wind += w0;
			for (size_t k = 1; k < nx; k++) {
				bool inside = evenodd ? (wind & 1) : (wind != 0);
				f64 xr = xings[k].x;
				if (inside && xr > xl) {
					/* accumulate span [xl, xr) */
					i32 i0 = (i32)floor(xl);
					i32 i1 = (i32)ceil(xr);
					if (i0 < 0) i0 = 0;
					if (i1 > (i32)w) i1 = (i32)w;
					for (i32 i = i0; i < i1; i++) {
						f64 a = (f64)i, b2 = (f64)i + 1.0;
						f64 lo = xl > a ? xl : a;
						f64 hi = xr < b2 ? xr : b2;
						if (hi > lo)
							acc[i] += (hi - lo) / SUBSAMPLE;
					}
				}
				wind += xings[k].sign;
				xl = xings[k].x;
			}
		}

		fill_row(px + (size_t)j * w, w, acc, (f64)j, paint, &lut);
	}
}

/* --------------------------------------------------------------------------
 * stroker
 * ------------------------------------------------------------------------ */

typedef struct {
	Vec2 *pts;
	size_t n, cap;
	RArena *ar;
} VBuf;

static void vb_push(VBuf *b, Vec2 v)
{
	if (b->n == b->cap) {
		size_t ncap = b->cap ? b->cap * 2 : 64;
		Vec2 *nv = (Vec2 *)arena_alloc(b->ar, ncap * sizeof(Vec2));
		if (b->n)
			memcpy(nv, b->pts, b->n * sizeof(Vec2));
		b->pts = nv;
		b->cap = ncap;
	}
	b->pts[b->n++] = v;
}

typedef struct {
	VBuf *bufs;
	size_t n, cap;
	RArena *ar;
} PolyB;

static VBuf *polyb_add(PolyB *pb)
{
	if (pb->n == pb->cap) {
		size_t ncap = pb->cap ? pb->cap * 2 : 16;
		VBuf *nv = (VBuf *)arena_alloc(pb->ar, ncap * sizeof(VBuf));
		if (pb->n)
			memcpy(nv, pb->bufs, pb->n * sizeof(VBuf));
		pb->bufs = nv;
		pb->cap = ncap;
	}
	VBuf *b = &pb->bufs[pb->n++];
	memset(b, 0, sizeof *b);
	b->ar = pb->ar;
	return b;
}

/* triangle with forced positive (CCW) orientation so overlapping pieces
 * accumulate winding instead of canceling under the nonzero rule */
static void emit_tri(PolyB *pb, Vec2 a, Vec2 b, Vec2 c)
{
	f64 cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	if (cross < 0) {
		Vec2 t = b;
		b = c;
		c = t;
	}
	VBuf *v = polyb_add(pb);
	vb_push(v, a);
	vb_push(v, b);
	vb_push(v, c);
}

/* closed quad, forced positive orientation */
static void emit_quad(PolyB *pb, Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
	f64 cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	if (cross < 0) {
		VBuf *v = polyb_add(pb);
		vb_push(v, d); vb_push(v, c); vb_push(v, b); vb_push(v, a);
	} else {
		VBuf *v = polyb_add(pb);
		vb_push(v, a); vb_push(v, b); vb_push(v, c); vb_push(v, d);
	}
}

/* angular arc fan around `center` from angle a0 to a1 (radius r), covering the
 * same arc regardless of sweep sign, always positive orientation */
static void emit_arc(PolyB *pb, Vec2 center, f64 a0, f64 a1, f64 r, int steps)
{
	if (a1 < a0) {
		f64 t = a0;
		a0 = a1;
		a1 = t;
	}
	if (steps < 2)
		steps = 2;
	for (int i = 0; i < steps; i++) {
		f64 t0 = a0 + (a1 - a0) * (f64)i / steps;
		f64 t1 = a0 + (a1 - a0) * (f64)(i + 1) / steps;
		Vec2 p0 = { center.x + cos(t0) * r, center.y + sin(t0) * r };
		Vec2 p1 = { center.x + cos(t1) * r, center.y + sin(t1) * r };
		emit_tri(pb, center, p0, p1);
	}
}

static f64 wrap_pi(f64 a)
{
	while (a > M_PI) a -= 2.0 * M_PI;
	while (a < -M_PI) a += 2.0 * M_PI;
	return a;
}

/* dash splitting: emits open polyline pieces into `out` */

static void dash_polyline(const Vec2 *pts, size_t n, bool closed,
                          const RStroke *st, PolyB *out)
{
	if (n < 2)
		return;

	if (st->ndash <= 0) {
		VBuf *cur = polyb_add(out);
		for (size_t i = 0; i < n; i++)
			vb_push(cur, pts[i]);
		if (closed && n > 1)
			vb_push(cur, pts[0]);
		return;
	}

	f64 pattern[32];
	int np = st->ndash;
	if (np & 1) {
		for (int i = 0; i < np; i++)
			pattern[i] = st->dash[i];
		for (int i = 0; i < np && np + i < 32; i++)
			pattern[np + i] = st->dash[i];
		np *= 2;
	} else {
		for (int i = 0; i < np; i++)
			pattern[i] = st->dash[i];
	}
	f64 total = 0;
	for (int i = 0; i < np; i++) {
		if (pattern[i] < 0) pattern[i] = 0;
		total += pattern[i];
	}
	if (total <= 1e-12)
		return;

	/* advance state past the dash offset */
	f64 off = st->dashoff < 0 ? -st->dashoff : st->dashoff;
	int seg = 0;
	f64 phase = 0;
	while (off > 0) {
		f64 left = pattern[seg % np] - phase;
		if (off >= left) {
			off -= left;
			seg++;
			phase = 0;
		} else {
			phase += off;
			off = 0;
		}
	}

	size_t count = closed ? n + 1 : n;
	VBuf *cur = NULL;
	Vec2 prev = pts[0];
	for (size_t i = 1; i < count; i++) {
		Vec2 p = (i < n) ? pts[i] : pts[0];
		f64 dx = p.x - prev.x, dy = p.y - prev.y;
		f64 len = hypot(dx, dy);
		if (len < 1e-12)
			continue;
		f64 done = 0;
		while (done < len - 1e-12) {
			f64 remain = len - done;
			f64 seg_left = pattern[seg % np] - phase;
			f64 take = remain < seg_left ? remain : seg_left;
			Vec2 a = { prev.x + dx * (done / len), prev.y + dy * (done / len) };
			Vec2 b = { prev.x + dx * ((done + take) / len), prev.y + dy * ((done + take) / len) };
			bool on = (seg % 2) == 0;
			if (on) {
				if (!cur) {
					cur = polyb_add(out);
					vb_push(cur, a);
				}
				vb_push(cur, b);
			} else {
				cur = NULL;
			}
			phase += take;
			done += take;
			if (phase >= pattern[seg % np] - 1e-12) {
				phase = 0;
				seg++;
			}
		}
		prev = p;
	}
}

/* stroke one polyline */

static void stroke_polyline(PolyB *pb, const Vec2 *pts, size_t n, bool closed,
                            const RStroke *st)
{
	if (n < 2)
		return;
	f64 hw = st->width / 2.0;
	if (hw <= 0)
		return;

	/* trim degenerate points */
	size_t end = n;
	while (end > 1 && rev_vec_dist(pts[end - 2], pts[end - 1]) < 1e-12)
		end--;
	size_t start = 0;
	while (start + 1 < end && rev_vec_dist(pts[start], pts[start + 1]) < 1e-12)
		start++;
	size_t m = end - start;
	if (m < 2)
		return;
	const Vec2 *p = pts + start;

	/* segment quads (+ square-cap extension) */
	for (size_t i = 0; i + 1 < m; i++) {
		Vec2 a = p[i], b = p[i + 1];
		f64 dx = b.x - a.x, dy = b.y - a.y;
		f64 len = hypot(dx, dy);
		if (len < 1e-12)
			continue;
		f64 ux = dx / len, uy = dy / len;
		Vec2 a2 = a, b2 = b;
		if (st->cap == RCAP_SQUARE) {
			if (i == 0 && !closed)      { a2.x -= ux * hw; a2.y -= uy * hw; }
			if (i + 2 == m && !closed)  { b2.x += ux * hw; b2.y += uy * hw; }
		}
		Vec2 na = { -uy * hw, ux * hw };
		emit_quad(pb,
		          (Vec2){ a2.x + na.x, a2.y + na.y },
		          (Vec2){ b2.x + na.x, b2.y + na.y },
		          (Vec2){ b2.x - na.x, b2.y - na.y },
		          (Vec2){ a2.x - na.x, a2.y - na.y });
	}

	/* joins */
	size_t jn = closed ? m : m - 1;
	for (size_t i = 0; i < jn; i++) {
		Vec2 a = p[i], b = p[(i + 1) % m], c = p[(i + 2) % m];
		f64 d1x = b.x - a.x, d1y = b.y - a.y;
		f64 d2x = c.x - b.x, d2y = c.y - b.y;
		f64 l1 = hypot(d1x, d1y), l2 = hypot(d2x, d2y);
		if (l1 < 1e-12 || l2 < 1e-12)
			continue;
		f64 u1x = d1x / l1, u1y = d1y / l1;
		f64 u2x = d2x / l2, u2y = d2y / l2;
		f64 cross = u1x * u2y - u1y * u2x;
		f64 dot = u1x * u2x + u1y * u2y;
		f64 ang = atan2(cross, dot);
		if (fabs(ang) < 1e-6)
			continue;

		/* outer side normals: outer is opposite the turn direction */
		f64 os = cross > 0 ? -1.0 : 1.0;
		Vec2 o1 = { b.x + (-u1y * os) * hw, b.y + (u1x * os) * hw };
		Vec2 o2 = { b.x + (-u2y * os) * hw, b.y + (u2x * os) * hw };

		if (st->join == RJOIN_ROUND) {
			f64 th1 = atan2(o1.y - b.y, o1.x - b.x);
			f64 th2 = atan2(o2.y - b.y, o2.x - b.x);
			/* the arc must pass through the outer bisector */
			Vec2 bis = { (o1.x - b.x) + (o2.x - b.x), (o1.y - b.y) + (o2.y - b.y) };
			f64 thm = atan2(bis.y, bis.x);
			f64 d = wrap_pi(th2 - th1);
			f64 mrel = wrap_pi(thm - th1);
			bool contains = (d >= 0) ? (mrel >= 0 && mrel <= d) : (mrel <= 0 && mrel >= d);
			if (!contains)
				d = d > 0 ? d - 2.0 * M_PI : d + 2.0 * M_PI;
			int steps = (int)ceil(fabs(d) / 0.35);
			if (steps < 2) steps = 2;
			if (steps > 32) steps = 32;
			emit_arc(pb, b, th1, th1 + d, hw, steps);
			/* wedge between quads under the arc */
			emit_tri(pb, b, o1, o2);
		} else if (st->join == RJOIN_BEVEL) {
			emit_tri(pb, b, o1, o2);
		} else {
			/* miter */
			f64 denom = u1x * u2y - u1y * u2x;
			if (fabs(denom) < 1e-9) {
				emit_tri(pb, b, o1, o2);
			} else {
				f64 ex = o2.x - o1.x, ey = o2.y - o1.y;
				f64 t = (ex * u2y - ey * u2x) / denom;
				Vec2 apex = { o1.x + u1x * t, o1.y + u1y * t };
				f64 miter = hypot(apex.x - b.x, apex.y - b.y);
				if (miter > st->miterlimit * hw) {
					emit_tri(pb, b, o1, o2);
				} else {
					emit_tri(pb, b, o1, o2);
					emit_tri(pb, o1, apex, o2);
				}
			}
		}
	}

	/* round caps at open ends */
	if (!closed && st->cap == RCAP_ROUND) {
		Vec2 a = p[0], b = p[m - 1];
		f64 dx = b.x - a.x, dy = b.y - a.y;
		f64 len = hypot(dx, dy);
		if (len > 1e-12) {
			f64 ux = dx / len, uy = dy / len;
			f64 thn = atan2(ux, -uy);            /* angle of +normal */
			emit_arc(pb, a, thn, thn + M_PI, hw, 12);        /* through -u */
			emit_arc(pb, b, thn + M_PI, thn + 2 * M_PI, hw, 12);  /* through +u */
		}
	}
}

void rev_raster_stroke(u32 *px, u32 w, u32 h, const RPoly *poly,
                       const RStroke *st, RArena *scratch)
{
	if (!st || st->width <= 0)
		return;
	if (st->paint.kind == RPAINT_SOLID ? st->paint.solid.a == 0 : st->paint.nstops == 0)
		return;

	PolyB pb;
	memset(&pb, 0, sizeof pb);
	pb.ar = scratch;

	for (size_t c = 0; c < poly->n; c++) {
		const Vec2 *pts = poly->cs[c].pts;
		size_t n = poly->cs[c].n;
		bool closed = poly->cs[c].closed;

		if (!st->dashed) {
			stroke_polyline(&pb, pts, n, closed, st);
		} else {
			/* split into dash pieces, then stroke each */
			PolyB pieces;
			memset(&pieces, 0, sizeof pieces);
			pieces.ar = scratch;
			dash_polyline(pts, n, closed, st, &pieces);
			for (size_t k = 0; k < pieces.n; k++) {
				VBuf *b = &pieces.bufs[k];
				/* dash pieces are always open */
				stroke_polyline(&pb, b->pts, b->n, false, st);
			}
		}
	}

	/* build an RPoly view over the stroke outline and fill (nonzero) */
	RPoly out;
	out.n = pb.n;
	out.cs = (RContour *)arena_alloc(scratch, (pb.n ? pb.n : 1) * sizeof(RContour));
	for (size_t i = 0; i < pb.n; i++) {
		out.cs[i].pts = pb.bufs[i].pts;
		out.cs[i].n = pb.bufs[i].n;
		out.cs[i].closed = true;
	}
	rev_raster_fill(px, w, h, &out, &st->paint, false, scratch);
}
