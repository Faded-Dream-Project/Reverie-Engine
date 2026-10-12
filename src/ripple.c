/* ============================================================================
 * Reverie Engine — ripple.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "ripple.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct RevRipple {
	u32 w, h;
	int gw, gh;         /* height field grid (half device res, capped) */
	f32 *cur, *prev;
	f64 strength, damping, radius;
	f64 px, py;
	bool pressed;
};

#define Ripple RevRipple

RevRipple *ripple_new(u32 w, u32 h)
{
	if (w < 8 || h < 8)
		return NULL;
	Ripple *r = (Ripple *)calloc(1, sizeof(Ripple));
	r->w = w;
	r->h = h;
	r->gw = (int)w / 2;
	r->gh = (int)h / 2;
	if (r->gw < 32) r->gw = 32;
	if (r->gh < 32) r->gh = 32;
	if (r->gw > 512) r->gw = 512;
	if (r->gh > 512) r->gh = 512;
	r->cur = (f32 *)calloc((size_t)r->gw * r->gh, sizeof(f32));
	r->prev = (f32 *)calloc((size_t)r->gw * r->gh, sizeof(f32));
	r->strength = 1.0;
	r->damping = 0.12;
	r->radius = 0.01;
	return r;
}

void ripple_free(RevRipple *r)
{
	if (!r)
		return;
	free(r->cur);
	free(r->prev);
	free(r);
}

void ripple_configure(RevRipple *r, f64 strength, f64 damping, f64 radius)
{
	if (!r)
		return;
	r->strength = rev_clampf(strength, 0.1, 8.0);
	r->damping = rev_clampf(damping, 0.001, 0.9);
	r->radius = rev_clampf(radius, 0.002, 0.1);
}

void ripple_pointer(RevRipple *r, f64 x, f64 y, bool pressed)
{
	if (!r)
		return;
	r->px = rev_clampf(x, 0, 1);
	r->py = rev_clampf(y, 0, 1);
	r->pressed = pressed;
}

void ripple_step(RevRipple *r, f64 dt)
{
	if (!r)
		return;
	int gw = r->gw, gh = r->gh;

	if (r->pressed) {
		f64 gx = r->px * (gw - 1);
		f64 gy = r->py * (gh - 1);
		f64 rad = r->radius * gw;
		int x0 = (int)(gx - rad), x1 = (int)(gx + rad);
		int y0 = (int)(gy - rad), y1 = (int)(gy + rad);
		if (x0 < 1) x0 = 1;
		if (y0 < 1) y0 = 1;
		if (x1 > gw - 2) x1 = gw - 2;
		if (y1 > gh - 2) y1 = gh - 2;
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				f64 dx = (x - gx) / (rad + 1e-9);
				f64 dy = (y - gy) / (rad + 1e-9);
				f64 d2 = dx * dx + dy * dy;
				if (d2 <= 1.0)
					r->cur[(size_t)y * gw + x] += (f32)(-r->strength * (1.0 - d2));
			}
		}
	}

	/* u_tt = c^2 * laplacian(u) — classic two-buffer update */
	f64 c2 = 0.24;                    /* wave speed (grid units) */
	f64 steps = rev_clampf(dt * 60.0, 0.5, 2.0);
	for (int s = 0; s < (int)steps; s++) {
		for (int y = 1; y < gh - 1; y++) {
			for (int x = 1; x < gw - 1; x++) {
				size_t i = (size_t)y * gw + x;
				f32 lap = r->cur[i - 1] + r->cur[i + 1] +
				          r->cur[i - gw] + r->cur[i + gw] - 4 * r->cur[i];
				r->prev[i] = (f32)((2.0 * r->cur[i] + c2 * lap - r->prev[i] * (1.0 - r->damping * 0.05)));
			}
		}
		f32 *t = r->cur;
		r->cur = r->prev;
		r->prev = t;
	}
}

void ripple_apply(const RevRipple *r, const u32 *below, u32 *out)
{
	if (!r || !below || !out)
		return;
	u32 w = r->w, h = r->h;
	int gw = r->gw, gh = r->gh;
	f64 sx = (f64)(gw - 1) / (w - 1);
	f64 sy = (f64)(gh - 1) / (h - 1);

	for (u32 y = 0; y < h; y++) {
		int gy = (int)(y * sy);
		if (gy < 1) gy = 1;
		if (gy > gh - 2) gy = gh - 2;
		for (u32 x = 0; x < w; x++) {
			int gx = (int)(x * sx);
			if (gx < 1) gx = 1;
			if (gx > gw - 2) gx = gw - 2;
			size_t i = (size_t)gy * gw + gx;

			/* surface normal from height gradient */
			f64 dxn = (r->cur[i + 1] - r->cur[i - 1]) * 0.5;
			f64 dyn = (r->cur[i + gw] - r->cur[i - gw]) * 0.5;

			/* skip flat water fast */
			if (fabs(dxn) < 1e-5 && fabs(dyn) < 1e-5) {
				out[(size_t)y * w + x] = below[(size_t)y * w + x];
				continue;
			}

			/* refraction: offset sample */
			f64 amp = r->strength * 26.0;
			int ox = (int)round(dxn * amp);
			int oy = (int)round(dyn * amp);
			int sxp = rev_clamp_i32((int)x + ox, 0, (int)w - 1);
			int syp = rev_clamp_i32((int)y + oy, 0, (int)h - 1);
			u32 p = below[(size_t)syp * w + sxp];

			/* specular highlight from normal . light */
			f64 len = sqrt(dxn * dxn + dyn * dyn + 0.25);
			f64 dot = (dyn * -0.7 + 0.5 * 0.7) / len;   /* light from upper-left */
			f64 spec = dot > 0 ? pow(dot, 24.0) : 0;

			u32 rr = p & 0xff, gg = (p >> 8) & 0xff, bb = (p >> 16) & 0xff;
			u32 sr = rr + (u32)(spec * 110);
			u32 sg = gg + (u32)(spec * 112);
			u32 sb = bb + (u32)(spec * 120);
			out[(size_t)y * w + x] =
				(p >> 24) << 24 |
				(sb > 255 ? 255 : sb) << 16 |
				(sg > 255 ? 255 : sg) << 8 |
				(sr > 255 ? 255 : sr);
		}
	}
}
