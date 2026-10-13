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
        f64 x, y;        /* device px            */
        f64 vx, vy;      /* px/s                 */
        f64 life, maxlife;
        f64 size;
        RColor c;
        bool spark;      /* pointer-spawned burst particle */
} Part;

struct RevParticles {
        u32 w, h;
        Part *parts;
        int cap, n;
        int preset, spawn;
        f64 rate, size, gravity, drift;
        RColor c0, c1;
        f64 emit_acc;
        f64 px, py;
        bool pressed;
};

#define PS RevParticles

RevParticles *particles_new(u32 w, u32 h)
{
        if (w < 8 || h < 8)
                return NULL;
        struct RevParticles *p = calloc(1, sizeof *p);
        p->w = w;
        p->h = h;
        p->cap = 4096;
        p->parts = (Part *)calloc((size_t)p->cap, sizeof(Part));
        p->spawn = RPSPAWN_AMBIENT;
        p->preset = RPPRESET_CUSTOM;
        p->rate = 60;
        p->size = 3;
        p->gravity = 60;
        p->drift = 20;
        p->c0 = rev_rgb(255, 200, 120);
        p->c1 = rev_rgb(255, 90, 60);
        return p;
}

void particles_free(RevParticles *p)
{
        if (!p)
                return;
        free(p->parts);
        free(p);
}

void particles_configure(RevParticles *pp, int preset,
                         int count, f64 rate, f64 size, f64 gravity, f64 drift,
                         int spawn, RColor c0, RColor c1)
{
        struct RevParticles *p = pp;
        if (!p)
                return;
        p->preset = preset;
        p->spawn = spawn;
        p->c0 = c0;
        p->c1 = c1;

        switch (preset) {
        case RPPRESET_RAIN:
                p->rate = 340; p->size = 1.6; p->gravity = 1300; p->drift = 30;
                p->c0 = rev_rgba(170, 200, 255, 190);
                p->c1 = rev_rgba(120, 160, 230, 150);
                break;
        case RPPRESET_SNOW:
                p->rate = 90; p->size = 2.6; p->gravity = 55; p->drift = 45;
                p->c0 = rev_rgba(255, 255, 255, 230);
                p->c1 = rev_rgba(220, 235, 255, 180);
                break;
        case RPPRESET_EMBERS:
                p->rate = 55; p->size = 2.2; p->gravity = -70; p->drift = 26;
                p->c0 = rev_rgba(255, 170, 60, 230);
                p->c1 = rev_rgba(255, 60, 30, 200);
                break;
        case RPPRESET_PETALS:
                p->rate = 24; p->size = 4.5; p->gravity = 42; p->drift = 60;
                p->c0 = rev_rgba(255, 170, 200, 220);
                p->c1 = rev_rgba(255, 120, 170, 200);
                break;
        case RPPRESET_FIREflies:
                p->rate = 14; p->size = 2.4; p->gravity = 0; p->drift = 18;
                p->c0 = rev_rgba(220, 255, 140, 210);
                p->c1 = rev_rgba(160, 255, 120, 160);
                break;
        default:
                p->rate = rate;
                p->size = size;
                p->gravity = gravity;
                p->drift = drift;
                break;
        }
        if (preset == RPPRESET_CUSTOM) {
                p->rate = rate;
                p->size = size;
                p->gravity = gravity;
                p->drift = drift;
        }
        (void)count;
}

static void emit(struct RevParticles *p, bool spark);

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
        Part *t = &p->parts[p->n++];
        memset(t, 0, sizeof *t);
        f64 mixc = rnd();
        t->c = rev_lerp_color(p->c0, p->c1, mixc);

        if (spark) {
                t->x = p->px * p->w;
                t->y = p->py * p->h;
                f64 ang = rnd() * 2.0 * M_PI;
                f64 spd = 90 + rnd() * 260;
                t->vx = cos(ang) * spd;
                t->vy = sin(ang) * spd - 60;
                t->maxlife = 0.7 + rnd() * 0.8;
                t->size = p->size * (0.7 + rnd() * 0.8);
                t->spark = true;
                return;
        }

        switch (p->preset) {
        case RPPRESET_RAIN:
                t->x = rnd() * p->w * 1.2 - p->w * 0.1;
                t->y = -8;
                t->vx = (rnd() - 0.5) * p->drift;
                t->vy = p->gravity * (0.85 + rnd() * 0.3);
                t->maxlife = 4.0;
                t->size = p->size;
                break;
        case RPPRESET_SNOW:
        case RPPRESET_PETALS:
                t->x = rnd() * p->w * 1.1 - p->w * 0.05;
                t->y = -8;
                t->vx = (rnd() - 0.5) * p->drift;
                t->vy = p->gravity * (0.6 + rnd() * 0.8);
                t->maxlife = 20.0;
                t->size = p->size * (0.6 + rnd() * 0.9);
                break;
        case RPPRESET_EMBERS:
                t->x = rnd() * p->w;
                t->y = p->h + 6;
                t->vx = (rnd() - 0.5) * p->drift;
                t->vy = p->gravity * (0.5 + rnd()) ;
                t->maxlife = 6.0;
                t->size = p->size * (0.5 + rnd());
                break;
        case RPPRESET_FIREflies:
                t->x = rnd() * p->w;
                t->y = rnd() * p->h;
                t->vx = (rnd() - 0.5) * p->drift * 2;
                t->vy = (rnd() - 0.5) * p->drift * 2;
                t->maxlife = 3 + rnd() * 5;
                t->size = p->size;
                break;
        default:
                t->x = rnd() * p->w;
                t->y = rnd() * p->h;
                t->vx = (rnd() - 0.5) * p->drift * 2;
                t->vy = (rnd() - 0.5) * p->drift * 2;
                t->maxlife = 3 + rnd() * 4;
                t->size = p->size * (0.6 + rnd() * 0.8);
                break;
        }
}

void particles_step(RevParticles *pp, f64 dt)
{
        struct RevParticles *p = pp;
        if (!p)
                return;
        if (dt <= 0) dt = 1.0 / 60.0;
        if (dt > 0.1) dt = 0.1;

        /* ambient emission */
        if (p->spawn == RPSPAWN_AMBIENT || p->spawn == RPSPAWN_BOTH) {
                p->emit_acc += p->rate * dt;
                while (p->emit_acc >= 1.0) {
                        emit(p, false);
                        p->emit_acc -= 1.0;
                }
        }
        /* pointer sparks */
        if (p->pressed && (p->spawn == RPSPAWN_POINTER || p->spawn == RPSPAWN_BOTH)) {
                p->emit_acc += 160 * dt;
                while (p->emit_acc >= 1.0) {
                        emit(p, true);
                        p->emit_acc -= 1.0;
                }
        }

        int n = 0;
        for (int i = 0; i < p->n; i++) {
                Part *t = &p->parts[i];
                t->life += dt;
                if (t->life >= t->maxlife)
                        continue;
                t->vy += (t->spark ? -220.0 : 0.0) * dt;   /* sparks arc upward */
                if (p->preset == RPPRESET_SNOW || p->preset == RPPRESET_PETALS) {
                        /* gentle sway */
                        t->vx += sin(t->life * 2.2 + t->x * 0.02) * 26.0 * dt;
                }
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

/* soft radial sprite (16x16 coverage LUT, scaled) */
static const u8 SPRITE[16][16] = {
        {0,0,0,0,0,0,1,2,2,1,0,0,0,0,0,0},
        {0,0,0,1,2,4,6,8,8,6,4,2,1,0,0,0},
        {0,0,2,4,7,10,13,15,15,13,10,7,4,2,0,0},
        {0,1,4,8,12,16,20,23,23,20,16,12,8,4,1,0},
        {0,2,7,12,17,22,26,29,29,26,22,17,12,7,2,0},
        {0,4,10,16,22,28,33,36,36,33,28,22,16,10,4,0},
        {1,6,13,20,26,33,39,42,42,39,33,26,20,13,6,1},
        {2,8,15,23,29,36,42,46,46,42,36,29,23,15,8,2},
        {2,8,15,23,29,36,42,46,46,42,36,29,23,15,8,2},
        {1,6,13,20,26,33,39,42,42,39,33,26,20,13,6,1},
        {0,4,10,16,22,28,33,36,36,33,28,22,16,10,4,0},
        {0,2,7,12,17,22,26,29,29,26,22,17,12,7,2,0},
        {0,1,4,8,12,16,20,23,23,20,16,12,8,4,1,0},
        {0,0,2,4,7,10,13,15,15,13,10,7,4,2,0,0},
        {0,0,0,1,2,4,6,8,8,6,4,2,1,0,0,0},
        {0,0,0,0,0,0,1,2,2,1,0,0,0,0,0,0},
};

void particles_render(const RevParticles *pp, u32 *px, u32 w, u32 h)
{
        const struct RevParticles *p = pp;
        if (!p || !px)
                return;

        for (int i = 0; i < p->n; i++) {
                const Part *t = &p->parts[i];
                f64 fade = t->life / t->maxlife;
                f64 alpha = fade < 0.8 ? 1.0 : (1.0 - (fade - 0.8) / 0.2);
                if (p->preset == RPPRESET_FIREflies || p->preset == RPPRESET_EMBERS)
                        alpha *= 0.55 + 0.45 * sin(t->life * 6.0 + i);
                if (alpha <= 0.02)
                        continue;

                f64 sz = t->size;
                if (sz < 0.5) sz = 0.5;
                f64 cx = t->x, cy = t->y;

                /* streaks for rain */
                if (p->preset == RPPRESET_RAIN) {
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

                /* round soft sprite */
                i32 r = (i32)(sz * 2.0);
                if (r < 1) r = 1;
                i32 x0 = (i32)(cx - r), x1 = (i32)(cx + r);
                i32 y0 = (i32)(cy - r), y1 = (i32)(cy + r);
                for (i32 y = y0; y <= y1; y++) {
                        if (y < 0 || y >= (i32)h) continue;
                        for (i32 x = x0; x <= x1; x++) {
                                if (x < 0 || x >= (i32)w) continue;
                                f64 ddx = (f64)x - cx, ddy = (f64)y - cy;
                                f64 dist = sqrt(ddx * ddx + ddy * ddy) / (r + 0.5);
                                if (dist > 1.0) continue;
                                f64 cov = (1.0 - dist * dist);
                                cov *= cov;
                                cov *= alpha;
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
        (void)SPRITE;
}
