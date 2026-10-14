/* ============================================================================
 * Reverie Engine — ripple.c
 *
 * Interactive water: a wave-equation height field driven by the pointer,
 * rendered as refraction of the composite below plus specular highlights.
 *
 * Update (2026-10, the "expanding glitch" bug): the old update was
 *     nh = 2h + c2*lap - prev*(1 - damping*0.05)
 * which is an ANTI-damped oscillator for smooth modes: the recurrence
 * z^2 - 2z + (1-d) has a pole at z = 1 + sqrt(d) > 1, so every click's
 * broad displacement component grew exponentially (measured: x38/s with
 * the ripple-lake theme) until the field hit inf/NaN — seconds later on
 * the GL path (RGBA16F overflows at 65504), longer on f32. That was the
 * field report: the splash expands, glitches once it reaches the screen
 * edges, and further clicks do nothing (adding -2.4 to NaN stays NaN).
 * It also explains the CPU/Vulkan tier's runaway sim time: once every
 * cell waved, ripple_apply's flat-water early-out never fired.
 *
 * The update is now the stable velocity-damped form
 *     nh = 2h - prev + c2*lap - dv*(h - prev),   dv = damping * 0.05
 * (all poles on/inside the unit circle), plus two calm-water measures:
 * an ABSORBING border band (waves die at the screen edges instead of
 * sloshing forever) and a snap floor (sub-visible heights become exact
 * zeros). A fully snapped field is an exact pass-through — tracked in
 * `peak` — so ripple_step short-circuits and the caller can skip the
 * full-screen refraction entirely (see ripple_active()).
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
        f32 peak;           /* exact max |height| over cur+prev (0 = calm)  */
        bool dirty;         /* injection since the last peak recompute      */
        f32 *attx, *atty;   /* absorbing border tapers, one axis each       */
};

#define Ripple RevRipple

/* wave speed^2 in grid cells/step — unchanged from the original solver
 * (CFL-stable for c2 < 1); keeps the visible ring-expansion rate the
 * same as every prior build. */
#define RIPPLE_C2 0.24
/* absorbing border band: within BAND cells of an edge the update is
 * scaled by a taper reaching 1 - EDGE_ABS (0.70) at the outermost row,
 * so wave energy dies at the screen edges instead of reflecting and
 * sloshing forever (~99% absorbed per pass: the 0.86 taper left a 10%
 * reflection bouncing for ~a minute). */
#define RIPPLE_BAND 24
#define RIPPLE_EDGE 0.30
/* heights below this are flat to the renderer — snapped to exact zero
 * so the field reaches a true quiescent state instead of decaying for
 * minutes in f32 denormals. The snap is MOMENTUM-GATED (fires only when
 * cur and prev are also at the floor): an unconditional deadzone zeroed
 * mid-cycle troughs while `prev` still carried the oscillator's momentum,
 * and the recurrence then catapulted (0, -5.6e-4) -> +5.2e-4 -> +7.5e-4 —
 * a self-recharging deadzone oscillator that equilibrated at ~7e-4 (and
 * at ~0.04 with a 1e-2 floor) forever. */
#define RIPPLE_SNAP 2.0e-4
/* sub-visible fast fade: heights under RIPPLE_VIS (refraction < ~1 px)
 * drain 6%/step, so the tail dies seconds after the last visible ripple
 * instead of riding the mild interior damping for a minute. Strictly
 * dissipative (unlike the snap deadzone), so it cannot sustain a cycle. */
#define RIPPLE_VIS 0.08
#define RIPPLE_FADE 0.94

static void ripple_build_taper(f32 *t, int n)
{
        for (int i = 0; i < n; i++) {
                int d = i < n - 1 - i ? i : n - 1 - i;
                f64 a = 1.0;
                if (d < RIPPLE_BAND)
                        a = 1.0 - RIPPLE_EDGE * (1.0 - (f64)d / (f64)RIPPLE_BAND);
                t[i] = (f32)a;
        }
}

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
        r->attx = (f32 *)malloc((size_t)r->gw * sizeof(f32));
        r->atty = (f32 *)malloc((size_t)r->gh * sizeof(f32));
        if (!r->cur || !r->prev || !r->attx || !r->atty) {
                ripple_free(r);
                return NULL;
        }
        ripple_build_taper(r->attx, r->gw);
        ripple_build_taper(r->atty, r->gh);
        r->strength = 1.0;
        r->damping = 0.12;
        r->radius = 0.01;
        r->peak = 0;
        r->dirty = false;
        return r;
}

void ripple_free(RevRipple *r)
{
        if (!r)
                return;
        free(r->cur);
        free(r->prev);
        free(r->attx);
        free(r->atty);
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

void ripple_click(RevRipple *r, f64 x, f64 y)
{
        if (!r)
                return;
        /* one-shot impact: stronger and wider than the held-pointer drip —
         * a dropped stone rather than a finger dragging through */
        f64 gx = rev_clampf(x, 0, 1) * (r->gw - 1);
        f64 gy = rev_clampf(y, 0, 1) * (r->gh - 1);
        f64 rad = rev_clampf(r->radius * 2.5, 0.004, 0.12) * r->gw;
        int x0 = (int)(gx - rad), x1 = (int)(gx + rad);
        int y0 = (int)(gy - rad), y1 = (int)(gy + rad);
        if (x0 < 1) x0 = 1;
        if (y0 < 1) y0 = 1;
        if (x1 > r->gw - 2) x1 = r->gw - 2;
        if (y1 > r->gh - 2) y1 = r->gh - 2;
        f64 amp = -rev_clampf(r->strength, 0.1, 8.0) * 1.5;
        for (int yy = y0; yy <= y1; yy++) {
                for (int xx = x0; xx <= x1; xx++) {
                        f64 ddx = (xx - gx) / (rad + 1e-9);
                        f64 ddy = (yy - gy) / (rad + 1e-9);
                        f64 d2 = ddx * ddx + ddy * ddy;
                        if (d2 <= 1.0)
                                r->cur[(size_t)yy * r->gw + xx] += (f32)(amp * (1.0 - d2) * (1.0 - d2));
                }
        }
        r->dirty = true;    /* peak is stale until the next step scans it */
}

bool ripple_active(const RevRipple *r)
{
        return r && (r->dirty || r->pressed || r->peak > 0.0f);
}

void ripple_step(RevRipple *r, f64 dt)
{
        if (!r)
                return;
        int gw = r->gw, gh = r->gh;

        /* calm water: zero field is a fixed point of the update — nothing
         * to advance until the next injection (clicks set `dirty`, the
         * drip keeps `pressed`). Keeps the CPU/Vulkan tiers' idle cost at
         * ~zero instead of stepping a dead grid every frame. */
        if (!r->dirty && !r->pressed && r->peak == 0.0f)
                return;

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

        /* u_tt = c^2 * laplacian(u) - velocity damping - level relaxation
         * — the STABLE form: every pole sits on/inside the unit circle, so
         * energy only ever leaves (the old -(1-d)*prev pole at 1+sqrt(d)
         * GREW it). Velocity damping alone leaves the DC pole at exactly
         * z=1: a slight "water level" offset would persist forever,
         * invisible but keeping the field (and the CPU apply path) alive
         * — the tiny ga term drains it. */
        f64 dv = r->damping * 0.05;
        const f64 ga = 0.0025;    /* level relaxation, per substep */
        f64 steps = rev_clampf(dt * 60.0, 0.5, 2.0);
        int nsteps = (int)steps;
        f32 peak = 0;

        for (int s = 0; s < nsteps; s++) {
                f32 *cur = r->cur, *prev = r->prev;
                for (int y = 1; y < gh - 1; y++) {
                        f32 ay = r->atty[y];
                        size_t row = (size_t)y * (size_t)gw;
                        for (int x = 1; x < gw - 1; x++) {
                                size_t i = row + (size_t)x;
                                f32 c = cur[i];
                                f32 p = prev[i];
                                f64 lap = cur[i - 1] + cur[i + 1] +
                                          cur[i - gw] + cur[i + gw] - 4.0 * c;
                                f64 nh = 2.0 * c - p + RIPPLE_C2 * lap
                                         - dv * (c - p) - ga * c;
                                /* absorbing border band (tapers, see above) */
                                nh *= (f64)(r->attx[x] * ay);
                                /* sub-visible fade + momentum-gated snap
                                 * (see the RIPPLE_SNAP comment above) */
                                if (nh < RIPPLE_VIS && nh > -RIPPLE_VIS)
                                        nh *= RIPPLE_FADE;
                                if (nh < RIPPLE_SNAP && nh > -RIPPLE_SNAP &&
                                    c < RIPPLE_SNAP && c > -RIPPLE_SNAP &&
                                    p < RIPPLE_SNAP && p > -RIPPLE_SNAP)
                                        nh = 0.0;
                                prev[i] = (f32)nh;
                                /* exact peak over BOTH buffers: prev[i] is
                                 * the new cur after the swap, c the new prev */
                                f32 a = fabsf(prev[i]);
                                if (a > peak) peak = a;
                                f32 b = fabsf(c);
                                if (b > peak) peak = b;
                        }
                }
                f32 *t = r->cur;
                r->cur = r->prev;
                r->prev = t;
        }

        if (nsteps <= 0) {
                /* no substep this frame (short dt): still recompute the
                 * exact peak so ripple_active() reflects any injection */
                size_t n = (size_t)gw * (size_t)gh;
                for (size_t i = 0; i < n; i++) {
                        f32 a = fabsf(r->cur[i]);
                        if (a > peak) peak = a;
                        f32 b = fabsf(r->prev[i]);
                        if (b > peak) peak = b;
                }
        }
        r->peak = peak;
        r->dirty = false;
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

                        /* skip flat water fast — 2.5e-4 is above anything a
                         * snap-floored (quiescent) field can produce, so a
                         * calm lake takes the pure copy path everywhere */
                        if (fabs(dxn) < 2.5e-4 && fabs(dyn) < 2.5e-4) {
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
                        /* dot^24 by exponentiation by squaring (t = dot^8,
                         * t*t*t = dot^24) — same curve as pow(dot, 24.0),
                         * a fraction of its cost on the hot path */
                        f64 t8 = dot * dot;
                        t8 *= t8;
                        t8 *= t8;
                        f64 spec = dot > 0 ? t8 * t8 * t8 : 0;

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
