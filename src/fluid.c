/* ============================================================================
 * Reverie Engine — fluid.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "fluid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define NITER  16        /* pressure Gauss-Seidel iterations */
#define VORT_E 2.0       /* vorticity confinement strength   */

struct RevFluid {
	int gw, gh;          /* velocity grid size                */
	int dw, dh;          /* dye grid size (2x velocity)      */
	f64 scale;           /* grid units per device pixel      */
	f32 *u, *v, *u0, *v0;
	f32 *r, *g, *b;      /* dye channels at dye res           */
	f32 *r0, *g0, *b0;
	/* scratch for projection */
	f32 *div, *p;
	/* config */
	RColor dye_a, dye_b;
	f64 force, radius, dissip, ambient;
	/* pointer splats queued between steps */
	struct {
		f64 x, y, dx, dy;
		bool pressed;
	} ptr;
	u32 w, h;
};

#define Fluid RevFluid

static f32 *fld_new(int n)
{
	f32 *p = (f32 *)calloc((size_t)n, sizeof(f32));
	return p;
}

RevFluid *fluid_new(u32 w, u32 h, f64 res_scale)
{
	if (w < 8 || h < 8)
		return NULL;
	if (res_scale <= 0) res_scale = 0.25;
	if (res_scale > 1.0) res_scale = 1.0;

	int gw = (int)(w * res_scale);
	int gh = (int)(h * res_scale);
	if (gw < 48) gw = 48;
	if (gh < 48) gh = 48;
	if (gw > 384) gw = 384;
	if (gh > 384) gh = 384;
	/* keep aspect roughly */
	if (w > h && gw < (int)((f64)gh * w / h)) {
		gw = (int)((f64)gh * w / h);
		if (gw > 384) gw = 384;
	} else if (h > w && gh < (int)((f64)gw * h / w)) {
		gh = (int)((f64)gw * h / w);
		if (gh > 384) gh = 384;
	}

	Fluid *f = (Fluid *)calloc(1, sizeof(Fluid));
	f->gw = gw;
	f->gh = gh;
	f->dw = gw * 2;
	f->dh = gh * 2;
	f->w = w;
	f->h = h;
	f->scale = (f64)gw / w;

	f->u  = fld_new(gw * gh); f->v  = fld_new(gw * gh);
	f->u0 = fld_new(gw * gh); f->v0 = fld_new(gw * gh);
	f->div = fld_new(gw * gh); f->p = fld_new(gw * gh);
	f->r = fld_new(f->dw * f->dh); f->g = fld_new(f->dw * f->dh);
	f->b = fld_new(f->dw * f->dh);
	f->r0 = fld_new(f->dw * f->dh); f->g0 = fld_new(f->dw * f->dh);
	f->b0 = fld_new(f->dw * f->dh);

	f->dye_a = rev_rgb(255, 90, 190);
	f->dye_b = rev_rgb(80, 180, 255);
	f->force = 8.0;
	f->radius = 0.015;
	f->dissip = 0.12;
	f->ambient = 0.0;
	return f;
}

void fluid_free(RevFluid *f)
{
	if (!f)
		return;
	free(f->u); free(f->v); free(f->u0); free(f->v0);
	free(f->div); free(f->p);
	free(f->r); free(f->g); free(f->b);
	free(f->r0); free(f->g0); free(f->b0);
	free(f);
}

void fluid_configure(RevFluid *f, RColor dye_a, RColor dye_b,
                     f64 force, f64 radius, f64 dissipation, f64 ambient)
{
	if (!f)
		return;
	f->dye_a = dye_a;
	f->dye_b = dye_b;
	f->force = force;
	f->radius = rev_clampf(radius, 0.002, 0.2);
	f->dissip = rev_clampf(dissipation, 0.0, 1.0);
	f->ambient = rev_clampf(ambient, 0.0, 1.0);
}

void fluid_pointer(RevFluid *f, f64 x, f64 y, f64 dx, f64 dy, bool pressed)
{
	if (!f)
		return;
	f->ptr.x = rev_clampf(x, 0, 1);
	f->ptr.y = rev_clampf(y, 0, 1);
	f->ptr.dx = dx;
	f->ptr.dy = dy;
	f->ptr.pressed = pressed;
}

/* bilinear sample on an arbitrary-resolution field */
static f32 sample_f(const f32 *fld, int fw, int fh, f64 x, f64 y)
{
	x = rev_clampf(x, 0.0, fw - 1.0);
	y = rev_clampf(y, 0.0, fh - 1.0);
	int x0 = (int)x, y0 = (int)y;
	int x1 = x0 + 1 < fw ? x0 + 1 : x0;
	int y1 = y0 + 1 < fh ? y0 + 1 : y0;
	f64 fx = x - x0, fy = y - y0;
	f64 a = fld[(size_t)y0 * fw + x0];
	f64 b = fld[(size_t)y0 * fw + x1];
	f64 c = fld[(size_t)y1 * fw + x0];
	f64 d = fld[(size_t)y1 * fw + x1];
	return (f32)(a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) +
	             c * (1 - fx) * fy + d * fx * fy);
}

/* add a gaussian splat of dye and velocity */
static void splat(Fluid *f, f64 px, f64 py, f64 vx, f64 vy, RColor col)
{
	f64 gx = px * (f->gw - 1);
	f64 gy = py * (f->gh - 1);
	f64 rad = f->radius * f->gw * 0.5 + 1.5;

	int x0 = (int)(gx - rad * 2), x1 = (int)(gx + rad * 2);
	int y0 = (int)(gy - rad * 2), y1 = (int)(gy + rad * 2);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > f->gw - 1) x1 = f->gw - 1;
	if (y1 > f->gh - 1) y1 = f->gh - 1;

	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			f64 dx = x - gx, dy = y - gy;
			f64 d2 = (dx * dx + dy * dy) / (rad * rad);
			f64 fall = exp(-d2 * 3.0);
			f->v[(size_t)y * f->gw + x] += (f32)(vy * fall);
			f->u[(size_t)y * f->gw + x] += (f32)(vx * fall);
		}
	}

	/* dye at dye-res */
	f64 dgx = px * (f->dw - 1);
	f64 dgy = py * (f->dh - 1);
	f64 drad = rad * 2;
	x0 = (int)(dgx - drad * 2); x1 = (int)(dgx + drad * 2);
	y0 = (int)(dgy - drad * 2); y1 = (int)(dgy + drad * 2);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > f->dw - 1) x1 = f->dw - 1;
	if (y1 > f->dh - 1) y1 = f->dh - 1;
	f64 cr = col.r / 255.0, cg = col.g / 255.0, cb = col.b / 255.0;
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			f64 dx = x - dgx, dy = y - dgy;
			f64 d2 = (dx * dx + dy * dy) / (drad * drad);
			f64 fall = exp(-d2 * 3.0) * 0.9;
			f->r[(size_t)y * f->dw + x] += (f32)(cr * fall);
			f->g[(size_t)y * f->dw + x] += (f32)(cg * fall);
			f->b[(size_t)y * f->dw + x] += (f32)(cb * fall);
		}
	}
}

/* divergence + pressure solve (Gauss-Seidel) + gradient subtraction */
static void project(Fluid *f)
{
	int gw = f->gw, gh = f->gh;
	f32 *u = f->u, *v = f->v, *div = f->div, *p = f->p;

	for (int y = 1; y < gh - 1; y++) {
		for (int x = 1; x < gw - 1; x++) {
			size_t i = (size_t)y * gw + x;
			div[i] = (f32)(-0.5 * (u[i + 1] - u[i - 1] + v[i + gw] - v[i - gw]));
			p[i] = 0;
		}
	}
	for (int x = 0; x < gw; x++) {
		div[(size_t)x] = div[(size_t)gw + x];
		div[(size_t)(gh - 1) * gw + x] = div[(size_t)(gh - 2) * gw + x];
	}
	for (int y = 0; y < gh; y++) {
		div[(size_t)y * gw] = div[(size_t)y * gw + 1];
		div[(size_t)y * gw + gw - 1] = div[(size_t)y * gw + gw - 2];
	}

	for (int k = 0; k < NITER; k++) {
		for (int y = 1; y < gh - 1; y++) {
			for (int x = 1; x < gw - 1; x++) {
				size_t i = (size_t)y * gw + x;
				p[i] = (f32)((div[i] + p[i - 1] + p[i + 1] + p[i - gw] + p[i + gw]) / 4.0);
			}
		}
	}

	for (int y = 1; y < gh - 1; y++) {
		for (int x = 1; x < gw - 1; x++) {
			size_t i = (size_t)y * gw + x;
			u[i] -= (f32)(0.5 * (p[i + 1] - p[i - 1]));
			v[i] -= (f32)(0.5 * (p[i + gw] - p[i - gw]));
		}
	}
}

/* semi-Lagrangian advection of field d0 (res fw x fh) through velocity */
static void advect(f32 *d, const f32 *d0, int fw, int fh,
                   const f32 *u, const f32 *v, int gw, int gh, f64 dt)
{
	f64 sx = (f64)(fw - 1) / (gw - 1);
	f64 sy = (f64)(fh - 1) / (gh - 1);
	for (int y = 1; y < fh - 1; y++) {
		for (int x = 1; x < fw - 1; x++) {
			/* velocity at this dye cell */
			f64 vx = sample_f(u, gw, gh, x / sx, y / sy);
			f64 vy = sample_f(v, gw, gh, x / sx, y / sy);
			f64 px = x - vx * dt * sx;
			f64 py = y - vy * dt * sy;
			d[(size_t)y * fw + x] = sample_f(d0, fw, fh, px, py);
		}
	}
}

static void vorticity(Fluid *f, f64 dt)
{
	int gw = f->gw, gh = f->gh;
	f32 *u = f->u, *v = f->v;

	for (int y = 2; y < gh - 2; y++) {
		for (int x = 2; x < gw - 2; x++) {
			size_t i = (size_t)y * gw + x;
			f64 curl = (v[i + 1] - v[i - 1]) - (u[i + gw] - u[i - gw]);
			(void)curl;
		}
	}
	/* full version with gradient of |curl| */
	static f32 *curlbuf = NULL;
	static int cap = 0;
	if (cap < gw * gh) {
		free(curlbuf);
		curlbuf = (f32 *)calloc((size_t)gw * gh, sizeof(f32));
		cap = gw * gh;
	}
	for (int y = 1; y < gh - 1; y++) {
		for (int x = 1; x < gw - 1; x++) {
			size_t i = (size_t)y * gw + x;
			curlbuf[i] = (f32)(((v[i + 1] - v[i - 1]) - (u[i + gw] - u[i - gw])) * 0.5);
		}
	}
	for (int y = 2; y < gh - 2; y++) {
		for (int x = 2; x < gw - 2; x++) {
			size_t i = (size_t)y * gw + x;
			f64 dx = (fabs(curlbuf[i + 1]) - fabs(curlbuf[i - 1])) * 0.5;
			f64 dy = (fabs(curlbuf[i + gw]) - fabs(curlbuf[i - gw])) * 0.5;
			f64 len = sqrt(dx * dx + dy * dy) + 1e-9;
			dx /= len;
			dy /= len;
			f64 c = curlbuf[i];
			u[i] += (f32)(VORT_E * dy * c * dt * 60.0 * 0.05);
			v[i] -= (f32)(VORT_E * dx * c * dt * 60.0 * 0.05);
		}
	}
}

void fluid_step(RevFluid *f, f64 dt)
{
	if (!f)
		return;
	if (dt <= 0) dt = 1.0 / 60.0;
	if (dt > 0.1) dt = 0.1;
	int gw = f->gw, gh = f->gh;

	/* pointer splats */
	if (f->ptr.pressed) {
		f64 speed = hypot(f->ptr.dx, f->ptr.dy);
		RColor c = (f->ptr.dx >= 0) ? f->dye_a : f->dye_b;
		if (speed < 1e-4) {
			/* button held without movement: gentle bloom */
			c = ((f->ptr.x * 7.0 + f->ptr.y * 13.0) > 10.0) ? f->dye_a : f->dye_b;
		}
		splat(f, f->ptr.x, f->ptr.y,
		      f->ptr.dx * f->force, f->ptr.dy * f->force, c);
	}
	f->ptr.dx = 0;
	f->ptr.dy = 0;

	/* ambient drift: slowly rotating force field */
	if (f->ambient > 0) {
		static f64 t = 0;
		t += dt;
		for (int y = 1; y < gh - 1; y += 3) {
			for (int x = 1; x < gw - 1; x += 3) {
				f64 nx = (f64)x / gw, ny = (f64)y / gh;
				f64 fx = sin(ny * 6.0 + t * 0.35) * cos(nx * 4.0 - t * 0.22);
				f64 fy = cos(nx * 5.0 + t * 0.3) * sin(ny * 5.0 + t * 0.18);
				f->u[(size_t)y * gw + x] += (f32)(fx * f->ambient * dt * 8.0);
				f->v[(size_t)y * gw + x] += (f32)(fy * f->ambient * dt * 8.0);
			}
		}
		/* occasional dye wisps */
		static f64 acc = 0;
		acc += dt;
		if (acc > 0.9) {
			acc = 0;
			f64 px = 0.15 + 0.7 * fabs(sin(t * 1.7));
			f64 py = 0.15 + 0.7 * fabs(cos(t * 1.1));
			splat(f, px, py, sin(t * 2.0) * 0.6, cos(t * 1.6) * 0.6,
			      ((int)(t * 3) & 1) ? f->dye_a : f->dye_b);
		}
	}

	vorticity(f, dt);
	project(f);

	/* advect velocity */
	memcpy(f->u0, f->u, (size_t)gw * gh * sizeof(f32));
	memcpy(f->v0, f->v, (size_t)gw * gh * sizeof(f32));
	advect(f->u, f->u0, gw, gh, f->u0, f->v0, gw, gh, dt);
	advect(f->v, f->v0, gw, gh, f->u0, f->v0, gw, gh, dt);
	project(f);

	/* velocity damping */
	f64 damp = pow(0.999, dt * 60.0);
	for (int i = 0; i < gw * gh; i++) {
		f->u[i] *= (f32)damp;
		f->v[i] *= (f32)damp;
	}

	/* advect dye */
	int dw = f->dw, dh = f->dh;
	memcpy(f->r0, f->r, (size_t)dw * dh * sizeof(f32));
	memcpy(f->g0, f->g, (size_t)dw * dh * sizeof(f32));
	memcpy(f->b0, f->b, (size_t)dw * dh * sizeof(f32));
	advect(f->r, f->r0, dw, dh, f->u, f->v, gw, gh, dt);
	advect(f->g, f->g0, dw, dh, f->u, f->v, gw, gh, dt);
	advect(f->b, f->b0, dw, dh, f->u, f->v, gw, gh, dt);

	/* dye dissipation */
	f64 fade = pow(1.0 - f->dissip * 0.35, dt * 60.0);
	for (int i = 0; i < dw * dh; i++) {
		f->r[i] *= (f32)fade;
		f->g[i] *= (f32)fade;
		f->b[i] *= (f32)fade;
	}
}

void fluid_render(const RevFluid *f, u32 *px, u32 w, u32 h)
{
	if (!f || !px)
		return;
	for (u32 y = 0; y < h; y++) {
		for (u32 x = 0; x < w; x++) {
			f64 dx = (f64)x / (w - 1) * (f->dw - 1);
			f64 dy = (f64)y / (h - 1) * (f->dh - 1);
			f64 r = sample_f(f->r, f->dw, f->dh, dx, dy);
			f64 g = sample_f(f->g, f->dw, f->dh, dx, dy);
			f64 b = sample_f(f->b, f->dw, f->dh, dx, dy);
			/* soft tonemap: color * (1 - exp(-l)) style glow */
			f64 lum = r > g ? (r > b ? r : b) : (g > b ? g : b);
			f64 a = 1.0 - exp(-lum * 2.2);
			if (a <= 0.003) {
				px[(size_t)y * w + x] = 0;
				continue;
			}
			f64 k = lum > 1e-6 ? a / lum : 0;
			u32 rr = (u32)rev_clampf(r * k * 255.0, 0, 255);
			u32 gg = (u32)rev_clampf(g * k * 255.0, 0, 255);
			u32 bb = (u32)rev_clampf(b * k * 255.0, 0, 255);
			u32 aa = (u32)rev_clampf(a * 255.0, 0, 255);
			px[(size_t)y * w + x] = aa << 24 | bb << 16 | gg << 8 | rr;
		}
	}
}
