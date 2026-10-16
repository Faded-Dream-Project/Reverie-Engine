/* ============================================================================
 * Reverie Engine — particles.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "particles.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* deterministic PRNG so scenes look the same across runs */
static u32 rnd_state = 0x9E3779B9u;
static f64 rnd(void)
{
        rnd_state ^= rnd_state << 13;
        rnd_state ^= rnd_state >> 17;
        rnd_state ^= rnd_state << 5;
        return (f64)(rnd_state % 1000000u) / 1000000.0;
}

typedef struct Part_ {
        f64 x, y;        /* device px                          */
        f64 vx, vy;      /* px/s                               */
        f64 life, maxlife;
        f64 size0;       /* sprite radius at birth, px         */
        f64 size1;       /* radius at end of life (size_out)   */
        RColor c;
        bool spark;      /* pointer-spawned burst particle    */
} Part;

struct RevParticles {
        u32 w, h;
        Part *parts;
        int cap, n;
        PartCfg cfg;
        f64 emit_acc;
        f64 px, py;      /* pointer, [0..1]                   */
        bool pressed;
};

#define PS RevParticles

/* ---------------------------------------------------------------- presets */

static void ramp2(PartCfg *c, RColor a, RColor b)
{
        c->ramp[0] = a;
        c->ramp[1] = b;
        c->ramp_t[0] = 0;
        c->ramp_t[1] = 1;
        c->nramp = 2;
}

void particles_preset(int preset, PartCfg *c)
{
        memset(c, 0, sizeof *c);
        c->spawn = RPSPAWN_AMBIENT;
        c->emit_from = RPEMIT_EVERYWHERE;
        c->area[0] = 0; c->area[1] = 0; c->area[2] = 1; c->area[3] = 1;
        c->rate = 60;
        c->dir_deg = 90;
        c->spread_deg = 180;
        c->speed0 = 0;
        c->speed1 = 30;
        c->gravity = 0;
        c->life0 = 3;
        c->life1 = 7;
        c->size0 = 1.8;
        c->size1 = 4.2;
        c->size_out = 1.0;
        c->fade_in = 0;
        c->fade_out = -1;          /* the classic last-20% tail */
        c->flicker = 0;
        c->shape = RSHAPE_SOFT;
        ramp2(c, rev_rgb(255, 200, 120), rev_rgb(255, 90, 60));
        c->burst = 24;
        c->burst_speed0 = 90;
        c->burst_speed1 = 350;
        c->burst_size = 3;
        c->streaks = false;

        switch (preset) {
        case RPPRESET_RAIN:
                /* baked-in fall speed, constant (no per-frame gravity —
                 * that is what the original emitter did) */
                c->rate = 340;
                c->emit_from = RPEMIT_TOP;
                c->dir_deg = 90;
                c->spread_deg = 2;
                c->speed0 = 1105;
                c->speed1 = 1495;
                c->life0 = c->life1 = 4;
                c->size0 = c->size1 = 1.6;
                c->burst_size = 1.6;
                ramp2(c, rev_rgba(170, 200, 255, 190), rev_rgba(120, 160, 230, 150));
                c->streaks = true;
                break;
        case RPPRESET_SNOW:
                c->rate = 90;
                c->emit_from = RPEMIT_TOP;
                c->dir_deg = 90;
                c->spread_deg = 40;
                c->speed0 = 33;
                c->speed1 = 77;
                c->life0 = c->life1 = 20;
                c->size0 = 1.56;
                c->size1 = 3.9;
                c->burst_size = 2.6;
                c->sway = 1.0;
                ramp2(c, rev_rgba(255, 255, 255, 230), rev_rgba(220, 235, 255, 180));
                break;
        case RPPRESET_EMBERS:
                c->rate = 55;
                c->emit_from = RPEMIT_BOTTOM;
                c->dir_deg = -90;
                c->spread_deg = 20;
                c->speed0 = 35;
                c->speed1 = 105;
                c->life0 = c->life1 = 6;
                c->size0 = 1.1;
                c->size1 = 2.2;
                c->burst_size = 2.2;
                c->flicker = 0.9;
                ramp2(c, rev_rgba(255, 170, 60, 230), rev_rgba(255, 60, 30, 200));
                break;
        case RPPRESET_PETALS:
                c->rate = 24;
                c->emit_from = RPEMIT_TOP;
                c->dir_deg = 90;
                c->spread_deg = 50;
                c->speed0 = 25;
                c->speed1 = 66;
                c->life0 = c->life1 = 20;
                c->size0 = 2.7;
                c->size1 = 6.75;
                c->burst_size = 4.5;
                c->sway = 1.0;
                ramp2(c, rev_rgba(255, 170, 200, 220), rev_rgba(255, 120, 170, 200));
                break;
        case RPPRESET_FIREflies:
                c->rate = 14;
                c->emit_from = RPEMIT_EVERYWHERE;
                c->spread_deg = 180;
                c->speed0 = 0;
                c->speed1 = 25;
                c->life0 = 3;
                c->life1 = 8;
                c->size0 = c->size1 = 2.4;
                c->burst_size = 2.4;
                c->flicker = 0.9;
                ramp2(c, rev_rgba(220, 255, 140, 210), rev_rgba(160, 255, 120, 160));
                break;
        default:
                break;      /* CUSTOM keeps the plain defaults above */
        }
}

/* ------------------------------------------------------------ color ramp */

static RColor ramp_sample(const PartCfg *c, f64 t)
{
        if (c->nramp <= 0)
                return rev_rgb(255, 255, 255);
        if (c->nramp == 1 || t <= c->ramp_t[0])
                return c->ramp[0];
        if (t >= c->ramp_t[c->nramp - 1])
                return c->ramp[c->nramp - 1];
        for (int i = 0; i < c->nramp - 1; i++) {
                f64 a = c->ramp_t[i], b = c->ramp_t[i + 1];
                if (t >= a && t <= b) {
                        f64 span = b - a;
                        f64 k = span > 1e-9 ? (t - a) / span : 0;
                        return rev_lerp_color(c->ramp[i], c->ramp[i + 1], k);
                }
        }
        return c->ramp[c->nramp - 1];
}

/* ------------------------------------------------------------- lifecycle */

RevParticles *particles_new(u32 w, u32 h)
{
        if (w < 8 || h < 8)
                return NULL;
        struct RevParticles *p = calloc(1, sizeof *p);
        p->w = w;
        p->h = h;
        p->cap = 4096;
        p->parts = (Part *)calloc((size_t)p->cap, sizeof(Part));
        particles_preset(RPPRESET_CUSTOM, &p->cfg);
        return p;
}

void particles_free(RevParticles *p)
{
        if (!p)
                return;
        free(p->parts);
        free(p);
}

void particles_setup(RevParticles *pp, const PartCfg *cfg)
{
        struct RevParticles *p = pp;
        if (!p || !cfg)
                return;
        p->cfg = *cfg;
        p->n = 0;
        p->emit_acc = 0;
}

static void emit(PS *p, bool spark);

void particles_pointer(RevParticles *pp, f64 x, f64 y, bool pressed)
{
        struct RevParticles *p = pp;
        if (!p)
                return;
        p->px = rev_clampf(x, 0, 1);
        p->py = rev_clampf(y, 0, 1);
        p->pressed = pressed;
}

void particles_burst(RevParticles *pp, f64 x, f64 y, int n)
{
        struct RevParticles *p = pp;
        if (!p || n <= 0)
                return;
        /* emit() places sparks at the current pointer position — borrow it for
         * the burst, then restore whatever the real pointer was tracking */
        f64 sx = p->px, sy = p->py;
        p->px = rev_clampf(x, 0, 1);
        p->py = rev_clampf(y, 0, 1);
        for (int i = 0; i < n; i++)
                emit(p, true);
        p->px = sx;
        p->py = sy;
}

static void emit(PS *p, bool spark)
{
        if (p->n >= p->cap)
                return;
        const PartCfg *c = &p->cfg;
        Part *t = &p->parts[p->n++];
        memset(t, 0, sizeof *t);

        t->c = ramp_sample(c, rnd());

        if (spark) {
                t->x = p->px * p->w;
                t->y = p->py * p->h;
                f64 ang = rnd() * 2.0 * M_PI;
                f64 spd = c->burst_speed0 + rnd() * (c->burst_speed1 - c->burst_speed0);
                t->vx = cos(ang) * spd;
                t->vy = sin(ang) * spd - 60;
                t->maxlife = 0.7 + rnd() * 0.8;
                t->size0 = c->burst_size * (0.7 + rnd() * 0.8);
                t->size1 = t->size0 * c->size_out;
                t->spark = true;
                return;
        }

        /* spawn position by region */
        switch (c->emit_from) {
        case RPEMIT_TOP:
                t->x = rnd() * p->w * 1.1 - p->w * 0.05;
                t->y = -8;
                break;
        case RPEMIT_BOTTOM:
                t->x = rnd() * p->w;
                t->y = p->h + 6;
                break;
        case RPEMIT_LEFT:
                t->x = -8;
                t->y = rnd() * p->h;
                break;
        case RPEMIT_RIGHT:
                t->x = p->w + 8;
                t->y = rnd() * p->h;
                break;
        case RPEMIT_EDGES:
                switch ((int)(rnd() * 4)) {
                case 0:  t->x = rnd() * p->w * 1.1 - p->w * 0.05; t->y = -8;     break;
                case 1:  t->x = rnd() * p->w;                      t->y = p->h + 6; break;
                case 2:  t->x = -8;                                t->y = rnd() * p->h; break;
                default: t->x = p->w + 8;                          t->y = rnd() * p->h; break;
                }
                break;
        case RPEMIT_AREA: {
                f64 ax = c->area[0] + rnd() * c->area[2];
                f64 ay = c->area[1] + rnd() * c->area[3];
                t->x = ax * p->w;
                t->y = ay * p->h;
                break;
        }
        default:
                t->x = rnd() * p->w;
                t->y = rnd() * p->h;
                break;
        }

        /* initial velocity: the direction cone */
        f64 deg = c->dir_deg + (rnd() - 0.5) * c->spread_deg;
        f64 rad = deg * (M_PI / 180.0);
        f64 spd = c->speed0 + rnd() * (c->speed1 - c->speed0);
        t->vx = cos(rad) * spd;
        t->vy = sin(rad) * spd;

        t->maxlife = c->life0 + rnd() * (c->life1 - c->life0);
        t->size0 = c->size0 + rnd() * (c->size1 - c->size0);
        t->size1 = t->size0 * c->size_out;
}

void particles_step(RevParticles *pp, f64 dt)
{
        struct RevParticles *p = pp;
        if (!p)
                return;
        if (dt <= 0) dt = 1.0 / 60.0;
        if (dt > 0.1) dt = 0.1;
        const PartCfg *c = &p->cfg;

        /* ambient emission */
        if (c->spawn == RPSPAWN_AMBIENT || c->spawn == RPSPAWN_BOTH) {
                p->emit_acc += c->rate * dt;
                while (p->emit_acc >= 1.0) {
                        emit(p, false);
                        p->emit_acc -= 1.0;
                }
        }
        /* pointer sparks while the button is held */
        if (p->pressed && (c->spawn == RPSPAWN_POINTER || c->spawn == RPSPAWN_BOTH)) {
                p->emit_acc += 160 * dt;
                while (p->emit_acc >= 1.0) {
                        emit(p, true);
                        p->emit_acc -= 1.0;
                }
        }

        f64 drag_k = c->drag > 0 ? c->drag * dt : 0;
        if (drag_k > 1) drag_k = 1;
        f64 tx = p->px * p->w, ty = p->py * p->h;

        int n = 0;
        for (int i = 0; i < p->n; i++) {
                Part *t = &p->parts[i];
                t->life += dt;
                if (t->life >= t->maxlife)
                        continue;

                /* continuous fields */
                if (c->gravity != 0)
                        t->vy += c->gravity * dt;
                if (c->wind[0] != 0 || c->wind[1] != 0) {
                        t->vx += c->wind[0] * dt;
                        t->vy += c->wind[1] * dt;
                }
                if (c->turbulence > 0) {
                        t->vx += (rnd() - 0.5) * c->turbulence * 400.0 * dt;
                        t->vy += (rnd() - 0.5) * c->turbulence * 400.0 * dt;
                }
                if (c->sway > 0)
                        t->vx += sin(t->life * 2.2 + t->x * 0.02) * 26.0 * c->sway * dt;
                if (drag_k > 0) {
                        t->vx *= 1.0 - drag_k;
                        t->vy *= 1.0 - drag_k;
                }
                if (c->attract != 0) {
                        f64 dx = tx - t->x, dy = ty - t->y;
                        f64 len = sqrt(dx * dx + dy * dy);
                        if (len > 1.0) {
                                t->vx += dx / len * c->attract * dt;
                                t->vy += dy / len * c->attract * dt;
                        }
                }
                if (t->spark)
                        t->vy += -220.0 * dt;    /* bursts arc upward (legacy) */

                t->x += t->vx * dt;
                t->y += t->vy * dt;
                if (t->x < -40 || t->x > p->w + 40 || t->y > p->h + 60 || t->y < -60)
                        continue;
                if (n != i)
                        p->parts[n] = p->parts[i];
                n++;
        }
        p->n = n;
}

/* ------------------------------------------------------ shared appearance */

/* The per-particle alpha envelope — one formula, used by BOTH the CPU
 * raster and particles_pack (the GPU tiers read the packed value, so the
 * three renderers agree by construction). */
static f64 part_alpha(const PS *p, const Part *t, int i)
{
        const PartCfg *c = &p->cfg;
        f64 a = 1.0;
        if (c->fade_in > 0) {
                f64 f = t->life / c->fade_in;
                a *= f < 1.0 ? f : 1.0;
        }
        if (c->fade_out >= 0) {
                f64 f = (t->maxlife - t->life) / c->fade_out;
                a *= f < 0 ? 0 : (f < 1 ? f : 1);
        } else {
                f64 fade = t->life / t->maxlife;    /* the classic tail */
                a *= fade < 0.8 ? 1.0 : 1.0 - (fade - 0.8) / 0.2;
        }
        if (c->flicker > 0)
                a *= (1.0 - c->flicker) + c->flicker * (0.5 + 0.5 * sin(t->life * 6.0 + i));
        return a;
}

/* Radius at the current point of life (size_out interpolation). */
static f64 part_size(const Part *t)
{
        f64 k = t->life / t->maxlife;
        return t->size0 + (t->size1 - t->size0) * (k < 0 ? 0 : (k > 1 ? 1 : k));
}

/* Shape coverage at unit-space coordinates (u, v) in [-1,1] — mirrored in
 * FS_PARTICLES (render_gl.c) and particles.comp; the parity legs hold the
 * three to each other. */
static f64 shape_cov(int shape, f64 u, f64 v)
{
        f64 d2 = u * u + v * v;
        if (d2 > 1.0)
                return 0.0;
        f64 t = sqrt(d2);
        f64 c;
        switch (shape) {
        case RSHAPE_DISC:
                c = (1.0 - t) * 6.0;
                return c > 1 ? 1 : c;
        case RSHAPE_GLOW:
                c = (exp(-5.0 * d2) - 0.006737946999) / 0.993262053;
                return c < 0 ? 0 : c;
        case RSHAPE_SPARK: {
                f64 a1 = 1.0 - 3.0 * fabs(u);
                f64 a2 = 1.0 - 3.0 * fabs(v);
                if (a1 < 0) a1 = 0;
                if (a2 < 0) a2 = 0;
                c = a1 + a2;
                return c > 1 ? 1 : c;
        }
        case RSHAPE_STAR:
                c = 1.0 - fabs(u) - fabs(v);
                if (c < 0)
                        return 0;
                return c * c * c;
        case RSHAPE_RING:
                c = 1.0 - fabs(t - 0.75) * 5.0;
                return c < 0 ? 0 : (c > 1 ? 1 : c);
        default:                    /* RSHAPE_SOFT */
                c = 1.0 - d2;
                return c * c;
        }
}

/* ------------------------------------------------------------- rendering */

void particles_render(const RevParticles *pp, u32 *px, u32 w, u32 h)
{
        const struct RevParticles *p = pp;
        if (!p || !px)
                return;
        const PartCfg *cfg = &p->cfg;

        for (int i = 0; i < p->n; i++) {
                const Part *t = &p->parts[i];
                f64 alpha = part_alpha(p, t, i);
                if (alpha <= 0.02)
                        continue;

                f64 sz = part_size(t);
                if (sz < 0.5) sz = 0.5;
                f64 cx = t->x, cy = t->y;

                /* rain's velocity streaks — the CPU-only sprite */
                if (cfg->streaks) {
                        f64 len = sz * 9.0;
                        i32 x0 = (i32)(cx - sz), x1 = (i32)(cx + sz + 1);
                        i32 y0 = (i32)(cy - len), y1 = (i32)(cy + len * 0.25);
                        for (i32 y = y0; y <= y1; y++) {
                                if (y < 0 || y >= (i32)h) continue;
                                for (i32 x = x0; x <= x1; x++) {
                                        if (x < 0 || x >= (i32)w) continue;
                                        f64 fy = (f64)(y - y0) / (y1 - y0 + 1);
                                        f64 cov = (1.0 - fabs((f64)x - cx) / (sz + 1)) * (0.35 + 0.65 * fy) * alpha;
                                        if (cov <= 0.02) continue;
                                        u32 dst = px[(size_t)y * w + x];
                                        u32 sa = (u32)(t->c.a * cov);
                                        u32 da = dst >> 24;
                                        u32 oa = sa + ((da * (255 - sa) + 127) >> 8);
                                        if (oa == 0) continue;
                                        u64 den = (u64)oa * 255;
                                        u64 nr = (u64)t->c.r * sa * 255 + (u64)(dst & 0xff) * da * (255 - sa);
                                        u64 ng = (u64)t->c.g * sa * 255 + (u64)((dst >> 8) & 0xff) * da * (255 - sa);
                                        u64 nb = (u64)t->c.b * sa * 255 + (u64)((dst >> 16) & 0xff) * da * (255 - sa);
                                        u64 vr = nr / den, vg = ng / den, vb = nb / den;
                                        px[(size_t)y * w + x] = oa << 24 |
                                                ((vb > 255 ? 255 : vb)) << 16 |
                                                ((vg > 255 ? 255 : vg)) << 8 |
                                                ((vr > 255 ? 255 : vr));
                                }
                        }
                        continue;
                }

                f64 r = sz * 2.0;
                if (r < 1) r = 1;
                i32 x0 = (i32)(cx - r), x1 = (i32)(cx + r);
                i32 y0 = (i32)(cy - r), y1 = (i32)(cy + r);
                for (i32 y = y0; y <= y1; y++) {
                        if (y < 0 || y >= (i32)h) continue;
                        for (i32 x = x0; x <= x1; x++) {
                                if (x < 0 || x >= (i32)w) continue;
                                f64 u = ((f64)x - cx) / (r + 0.5);
                                f64 v = ((f64)y - cy) / (r + 0.5);
                                f64 cov = shape_cov(cfg->shape, u, v) * alpha;
                                if (cov <= 0.02) continue;
                                u32 dst = px[(size_t)y * w + x];
                                u32 sa = (u32)(t->c.a * cov);
                                u32 da = dst >> 24;
                                u32 oa = sa + ((da * (255 - sa) + 127) >> 8);
                                if (oa == 0) continue;
                                u64 den = (u64)oa * 255;
                                u64 nr = (u64)t->c.r * sa * 255 + (u64)(dst & 0xff) * da * (255 - sa);
                                u64 ng = (u64)t->c.g * sa * 255 + (u64)((dst >> 8) & 0xff) * da * (255 - sa);
                                u64 nb = (u64)t->c.b * sa * 255 + (u64)((dst >> 16) & 0xff) * da * (255 - sa);
                                u64 vr = nr / den, vg = ng / den, vb = nb / den;
                                px[(size_t)y * w + x] = oa << 24 |
                                        ((vb > 255 ? 255 : vb)) << 16 |
                                        ((vg > 255 ? 255 : vg)) << 8 |
                                        ((vr > 255 ? 255 : vr));
                        }
                }
        }
}

u32 particles_pack(const RevParticles *pp, f32 *band0, f32 *band1, u32 slots)
{
        const struct RevParticles *p = pp;
        if (!p || !band0 || !band1 || slots == 0)
                return 0;
        u32 n = 0;
        for (int i = 0; i < p->n && n < slots; i++) {
                const Part *t = &p->parts[i];
                f64 alpha = part_alpha(p, t, i);   /* exactly what render uses */
                if (alpha <= 0.02)
                        continue;
                f64 sz = part_size(t);
                if (sz < 0.5) sz = 0.5;
                f32 *a = band0 + (size_t)n * 4;
                a[0] = (f32)t->x;
                a[1] = (f32)t->y;
                a[2] = (f32)sz;
                a[3] = (f32)alpha;
                f32 *b = band1 + (size_t)n * 4;
                b[0] = (f32)t->c.r / 255.0f;
                b[1] = (f32)t->c.g / 255.0f;
                b[2] = (f32)t->c.b / 255.0f;
                b[3] = (f32)p->cfg.shape;      /* band1.w rides the shape id */
                n++;
        }
        return n;
}
