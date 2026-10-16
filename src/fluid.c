/* ============================================================================
 * Reverie Engine — fluid.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "fluid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fluid_sched.h"
#include "log.h"

#define NITER  16        /* pressure Gauss-Seidel iterations */
#define VORT_E 2.0       /* vorticity confinement strength   */

/* ---- tonemap LUT -----------------------------------------------------------
 * fluid_render's soft glow: a = 1 - exp(-lum*2.2), k = a/lum (k -> 2.2 as
 * lum -> 0). Both are smooth functions of lum, so a 2048-entry table over
 * lum in [0, 6] replaces the per-pixel exp() + division — the per-pixel
 * cost drops from ~200 to ~40 cycles and 8-bit output differs by at most
 * one step. */
#define FLUT_N    2048
#define FLUT_LMAX 6.0

struct RevFluid {
        int gw, gh;          /* velocity grid size                */
        int dw, dh;          /* dye grid size (2x velocity)      */
        f64 scale;           /* grid units per device pixel      */
        f32 *u, *v, *u0, *v0;
        f32 *r, *g, *b;      /* dye channels at dye res           */
        f32 *r0, *g0, *b0;
        /* scratch for projection */
        f32 *div, *p;
        /* vorticity scratch (per-instance; was a process-global) */
        f32 *curl;
        /* config */
        RColor dye_a, dye_b;
        f64 force, radius, dissip, ambient;
        /* ambient drift clock + wisp accumulator (per-instance) */
        f64 amb_t, wisp_acc;
        u64 wisp_fires;         /* organic cadence: hash index of the NEXT firing */
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

/* Shared grid sizing (fluid_new + the GPU solver in render_gl.c must agree
 * on the velocity/dye resolution a layer gets for identical visuals). */
void fluid_grid_size(u32 w, u32 h, f64 res_scale, u32 *out_gw, u32 *out_gh)
{
        if (res_scale <= 0)
                res_scale = 0.25;
        if (res_scale > 1.0)
                res_scale = 1.0;

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
        if (out_gw)
                *out_gw = (u32)gw;
        if (out_gh)
                *out_gh = (u32)gh;
}

RevFluid *fluid_new(u32 w, u32 h, f64 res_scale)
{
        if (w < 8 || h < 8)
                return NULL;

        u32 gwx, ghy;
        fluid_grid_size(w, h, res_scale, &gwx, &ghy);
        int gw = (int)gwx, gh = (int)ghy;

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
        f->curl = fld_new(gw * gh);
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
        free(f->div); free(f->p); free(f->curl);
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

/* Dye grid size — the resolution the dye field actually exists at. GPU
 * compositing renders the fluid AT this size and lets linear texture
 * sampling do the upscale (identical mapping: fluid_render's device-pixel
 * loop is itself a bilinear upscale of the same field). */
void fluid_dye_size(const RevFluid *f, u32 *w, u32 *h)
{
        if (w) *w = (u32)f->dw;
        if (h) *h = (u32)f->dh;
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

/* f32 bilinear sample — every hot-path caller. Same expression tree the
 * original f64 version used, in f32 arithmetic: the per-sample cost halves
 * and the dye's trajectory differs only in f32 rounding (invisible in the
 * rendered field). */
static f32 sample_f32(const f32 *fld, int fw, int fh, f32 x, f32 y)
{
        if (x < 0.0f) x = 0.0f;
        if (x > (f32)(fw - 1)) x = (f32)(fw - 1);
        if (y < 0.0f) y = 0.0f;
        if (y > (f32)(fh - 1)) y = (f32)(fh - 1);
        int x0 = (int)x, y0 = (int)y;
        int x1 = x0 + 1 < fw ? x0 + 1 : x0;
        int y1 = y0 + 1 < fh ? y0 + 1 : y0;
        f32 fx = x - (f32)x0, fy = y - (f32)y0;
        f32 a = fld[(size_t)y0 * fw + x0];
        f32 b = fld[(size_t)y0 * fw + x1];
        f32 c = fld[(size_t)y1 * fw + x0];
        f32 d = fld[(size_t)y1 * fw + x1];
        return a * (1.0f - fx) * (1.0f - fy) + b * fx * (1.0f - fy) +
               c * (1.0f - fx) * fy + d * fx * fy;
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

/* divergence + pressure solve (Gauss-Seidel) + gradient subtraction.
 * Hot loop notes: multiplication by 0.25f/0.5f is bit-identical to division
 * by 4.0/2.0 (powers of two scale exactly in binary FP) but stays in f32 —
 * the old / 4.0 promoted every cell to f64 and back, roughly doubling the
 * cost of the 2 x 16 x NITER passes that dominate fluid_step. */
static void project(Fluid *f)
{
        int gw = f->gw, gh = f->gh;
        f32 *u = f->u, *v = f->v, *div = f->div, *p = f->p;

        for (int y = 1; y < gh - 1; y++) {
                for (int x = 1; x < gw - 1; x++) {
                        size_t i = (size_t)y * gw + x;
                        div[i] = -0.5f * (u[i + 1] - u[i - 1] + v[i + gw] - v[i - gw]);
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
                                p[i] = (div[i] + p[i - 1] + p[i + 1] + p[i - gw] + p[i + gw]) * 0.25f;
                        }
                }
        }

        for (int y = 1; y < gh - 1; y++) {
                for (int x = 1; x < gw - 1; x++) {
                        size_t i = (size_t)y * gw + x;
                        u[i] -= 0.5f * (p[i + 1] - p[i - 1]);
                        v[i] -= 0.5f * (p[i + gw] - p[i - gw]);
                }
        }
}

/* semi-Lagrangian advection. The velocity sample and the backtrack are
 * IDENTICAL for every field advected through the same grid — so velocity
 * u+v advect in one pass (4 samples/cell instead of 6) and the three dye
 * channels in one pass (5 samples/cell instead of 9). Same expressions,
 * same values, less arithmetic. */

static void advect_vel(f32 *du, f32 *dv, const f32 *u0, const f32 *v0,
                       int gw, int gh, f64 dt)
{
        f32 fdt = (f32)dt;
        for (int y = 1; y < gh - 1; y++) {
                f32 fy = (f32)y;
                for (int x = 1; x < gw - 1; x++) {
                        f32 fx = (f32)x;
                        f32 vx = sample_f32(u0, gw, gh, fx, fy);
                        f32 vy = sample_f32(v0, gw, gh, fx, fy);
                        f32 px = fx - vx * fdt;
                        f32 py = fy - vy * fdt;
                        size_t i = (size_t)y * gw + x;
                        du[i] = sample_f32(u0, gw, gh, px, py);
                        dv[i] = sample_f32(v0, gw, gh, px, py);
                }
        }
}

static void advect_dye(f32 *dr, f32 *dg, f32 *db,
                       const f32 *r0, const f32 *g0, const f32 *b0,
                       const f32 *u, const f32 *v, int gw, int gh,
                       int dw, int dh, f64 dt)
{
        f32 sx = (f32)(dw - 1) / (f32)(gw - 1);
        f32 sy = (f32)(dh - 1) / (f32)(gh - 1);
        f32 fdt = (f32)dt;
        /* the dye->velocity coordinate map depends only on the column/row —
         * hoist the divisions out of the per-cell loop entirely */
        f32 *gx = (f32 *)malloc((size_t)dw * sizeof(f32));
        f32 *gy = (f32 *)malloc((size_t)dh * sizeof(f32));
        if (!gx || !gy) {
                free(gx);
                free(gy);
                return;
        }
        for (int x = 0; x < dw; x++)
                gx[x] = (f32)x / sx;
        for (int y = 0; y < dh; y++)
                gy[y] = (f32)y / sy;
        for (int y = 1; y < dh - 1; y++) {
                f32 fgy = gy[y];
                for (int x = 1; x < dw - 1; x++) {
                        f32 fgx = gx[x];
                        f32 vx = sample_f32(u, gw, gh, fgx, fgy);
                        f32 vy = sample_f32(v, gw, gh, fgx, fgy);
                        f32 px = (f32)x - vx * fdt * sx;
                        f32 py = (f32)y - vy * fdt * sy;
                        size_t i = (size_t)y * dw + x;
                        dr[i] = sample_f32(r0, dw, dh, px, py);
                        dg[i] = sample_f32(g0, dw, dh, px, py);
                        db[i] = sample_f32(b0, dw, dh, px, py);
                }
        }
        free(gx);
        free(gy);
}

static void vorticity(Fluid *f, f64 dt)
{
        int gw = f->gw, gh = f->gh;
        f32 *u = f->u, *v = f->v, *curl = f->curl;

        for (int y = 1; y < gh - 1; y++) {
                for (int x = 1; x < gw - 1; x++) {
                        size_t i = (size_t)y * gw + x;
                        curl[i] = ((v[i + 1] - v[i - 1]) - (u[i + gw] - u[i - gw])) * 0.5f;
                }
        }
        /* VORT_E * dt * 60.0 * 0.05 == 6.0 * dt, folded */
        f64 k = VORT_E * 60.0 * 0.05 * dt;
        for (int y = 2; y < gh - 2; y++) {
                for (int x = 2; x < gw - 2; x++) {
                        size_t i = (size_t)y * gw + x;
                        f64 dx = (fabs((f64)curl[i + 1]) - fabs((f64)curl[i - 1])) * 0.5;
                        f64 dy = (fabs((f64)curl[i + gw]) - fabs((f64)curl[i - gw])) * 0.5;
                        f64 len = sqrt(dx * dx + dy * dy) + 1e-9;
                        dx /= len;
                        dy /= len;
                        f64 c = curl[i];
                        u[i] += (f32)(k * dy * c);
                        v[i] -= (f32)(k * dx * c);
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

        /* ambient drift: slowly rotating force field, strength breathing
         * through fluid_ambient_gain() so the motion never settles into a
         * steady state (the pre-scheduler constant amplitude read as a
         * mechanical loop after a few minutes) */
        if (f->ambient > 0) {
                f->amb_t += dt;
                f64 t = f->amb_t;
                f64 amb = f->ambient * fluid_ambient_gain(t);
                for (int y = 1; y < gh - 1; y += 3) {
                        for (int x = 1; x < gw - 1; x += 3) {
                                f64 nx = (f64)x / gw, ny = (f64)y / gh;
                                f64 fx = sin(ny * 6.0 + t * 0.35) * cos(nx * 4.0 - t * 0.22);
                                f64 fy = cos(nx * 5.0 + t * 0.3) * sin(ny * 5.0 + t * 0.18);
                                f->u[(size_t)y * gw + x] += (f32)(fx * amb * dt * 8.0);
                                f->v[(size_t)y * gw + x] += (f32)(fy * amb * dt * 8.0);
                        }
                }
                /* occasional dye wisps — irregular cadence, open trajectory
                 * (see fluid_sched.h: what the fixed 0.9 s comb used to be) */
                f->wisp_acc += dt;
                if (f->wisp_acc > fluid_wisp_interval(f->wisp_fires)) {
                        f->wisp_acc = 0;
                        f->wisp_fires++;
                        f64 wx, wy, wvx, wvy;
                        bool alt;
                        fluid_wisp_at(t, f->wisp_fires, &wx, &wy, &wvx, &wvy, &alt);
                        splat(f, wx, wy, wvx, wvy,
                              alt ? f->dye_b : f->dye_a);
                }
        }

        vorticity(f, dt);
        project(f);

        /* advect velocity */
        memcpy(f->u0, f->u, (size_t)gw * gh * sizeof(f32));
        memcpy(f->v0, f->v, (size_t)gw * gh * sizeof(f32));
        advect_vel(f->u, f->v, f->u0, f->v0, gw, gh, dt);
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
        advect_dye(f->r, f->g, f->b, f->r0, f->g0, f->b0, f->u, f->v,
                   gw, gh, dw, dh, dt);

        /* dye dissipation */
        f64 fade = pow(1.0 - f->dissip * 0.35, dt * 60.0);
        for (int i = 0; i < dw * dh; i++) {
                f->r[i] *= (f32)fade;
                f->g[i] *= (f32)fade;
                f->b[i] *= (f32)fade;
        }
}

static inline u32 q8(f32 v)
{
        return v <= 0.0f ? 0 : (v >= 255.0f ? 255 : (u32)v);
}

void fluid_render(const RevFluid *f, u32 *px, u32 w, u32 h)
{
        if (!f || !px || !w || !h)
                return;
        const int dw = f->dw, dh = f->dh;
        const f32 *R = f->r, *G = f->g, *B = f->b;

        /* tonemap LUT (built once) */
        static f32 alut[FLUT_N], klut[FLUT_N];
        static bool lut_ok;
        if (!lut_ok) {
                for (int i = 0; i < FLUT_N; i++) {
                        f64 lum = (f64)i * (FLUT_LMAX / FLUT_N);
                        f64 a = (i == 0) ? 0.0 : 1.0 - exp(-lum * 2.2);
                        alut[i] = (f32)a;
                        klut[i] = (f32)(i == 0 ? 2.2 : a / lum);
                }
                lut_ok = true;
        }
        const f32 ls = (f32)(FLUT_N / FLUT_LMAX);

        /* per-column dye coordinates + x weights (independent of y) */
        int *x0 = (int *)malloc((size_t)w * sizeof(int));
        int *x1 = (int *)malloc((size_t)w * sizeof(int));
        f32 *wx0 = (f32 *)malloc((size_t)w * sizeof(f32));
        f32 *wx1 = (f32 *)malloc((size_t)w * sizeof(f32));
        if (!x0 || !x1 || !wx0 || !wx1) {
                free(x0); free(x1); free(wx0); free(wx1);
                return;
        }
        for (u32 i = 0; i < w; i++) {
                f64 dx = (w > 1) ? (f64)i / (w - 1) * (dw - 1) : 0.0;
                if (dx < 0.0) dx = 0.0;
                if (dx > dw - 1) dx = dw - 1;
                int a = (int)dx;
                int b = a + 1 < dw ? a + 1 : a;
                f64 fx = dx - a;
                x0[i] = a;
                x1[i] = b;
                wx0[i] = (f32)(1.0 - fx);
                wx1[i] = (f32)fx;
        }

        for (u32 j = 0; j < h; j++) {
                f64 dy = (h > 1) ? (f64)j / (h - 1) * (dh - 1) : 0.0;
                if (dy < 0.0) dy = 0.0;
                if (dy > dh - 1) dy = dh - 1;
                int y0 = (int)dy;
                int y1 = y0 + 1 < dh ? y0 + 1 : y0;
                f64 fy = dy - y0;
                f32 wy0 = (f32)(1.0 - fy), wy1 = (f32)fy;

                const f32 *r0p = R + (size_t)y0 * dw, *r1p = R + (size_t)y1 * dw;
                const f32 *g0p = G + (size_t)y0 * dw, *g1p = G + (size_t)y1 * dw;
                const f32 *b0p = B + (size_t)y0 * dw, *b1p = B + (size_t)y1 * dw;
                u32 *row = px + (size_t)j * w;

                for (u32 i = 0; i < w; i++) {
                        int a = x0[i], b = x1[i];
                        f32 ax = wx0[i], bx = wx1[i];
                        f32 r = (r0p[a] * ax + r0p[b] * bx) * wy0 +
                                (r1p[a] * ax + r1p[b] * bx) * wy1;
                        f32 g = (g0p[a] * ax + g0p[b] * bx) * wy0 +
                                (g1p[a] * ax + g1p[b] * bx) * wy1;
                        f32 b2 = (b0p[a] * ax + b0p[b] * bx) * wy0 +
                                 (b1p[a] * ax + b1p[b] * bx) * wy1;
                        f32 lum = r > g ? (r > b2 ? r : b2) : (g > b2 ? g : b2);
                        int idx = (int)(lum * ls);
                        if (idx < 0) idx = 0;
                        if (idx >= FLUT_N) idx = FLUT_N - 1;
                        f32 alpha = alut[idx];
                        if (alpha <= 0.003f) {
                                row[i] = 0;
                                continue;
                        }
                        f32 k = klut[idx];
                        row[i] = q8(alpha * 255.0f) << 24 | q8(b2 * k * 255.0f) << 16 |
                                 q8(g * k * 255.0f) << 8 | q8(r * k * 255.0f);
                }
        }

        free(x0); free(x1); free(wx0); free(wx1);
}
