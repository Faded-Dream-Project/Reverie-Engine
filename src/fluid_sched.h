/* ============================================================================
 * Reverie Engine — fluid_sched.h
 * Organic scheduling for the fluid's ambient life: WISP firing cadence,
 * wisp trajectories, and the ambient "breathing" gain.
 *
 * Header-only f64 math, shared by all three solver tiers (the CPU fluid in
 * fluid.c, the GL GPU offload in render_gl.c, the Vulkan offload in
 * render_vk.c) — every tier schedules CPU-side through THESE functions, so
 * what they feed their solvers is bit-identical and cross-tier parity is
 * preserved by construction.
 *
 * What replaced what (the "mechanical" feel, measured):
 *   - wisp cadence was a fixed 0.9 s (a 1.1 Hz comb in any temporal
 *     analysis); it is now hash-jittered per firing, [0.45, 1.75] s,
 *     mean ~1.1 s, no repeat cadence.
 *   - wisp positions were fixed sin/cos functions of the ambient clock
 *     (|sin(t*1.7)| etc. — visibly oscillatory, bounded to the same
 *     Lissajous curve forever); they now walk an open, non-closing
 *     multi-harmonic trajectory with a golden-angle phase per firing.
 *   - the ambient force amplitude was CONSTANT; it now breathes with three
 *     incommensurate slow LFOs (periods ~153 s / 59 s / 23 s, gain clamped
 *     to [0.25, 1.0]) so the drift never settles into a steady state.
 *
 * Determinism: integer hashing is exact; sin/cos come from the host libm
 * in the same process for every tier — identical results.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_FLUID_SCHED_H
#define REV_FLUID_SCHED_H

#include "rev_types.h"

/* splitmix64 finalizer — a tiny exact integer hash (no state, no seed). */
static inline u64 fluid_fire_hash(u64 i)
{
        u64 z = i + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
}

/* Seconds until wisp #i fires: [0.45, 1.75], irregular, never repeating. */
static inline f64 fluid_wisp_interval(u64 i)
{
        return 0.45 + 1.3 * ((f64)(fluid_fire_hash(i) >> 40) / 16777216.0);
}

/* Wisp #i's state at ambient-clock time t. Positions stay in
 * [0.05, 0.95] of the viewport; the trajectory is a sum of harmonics on
 * two incommensurate time bases plus a golden-angle phase per firing, so
 * the curve never closes and consecutive wisps never share a region.
 * Impulse strength varies per firing (some faint, some bold). */
static inline void fluid_wisp_at(f64 t, u64 i, f64 *x, f64 *y,
                                 f64 *vx, f64 *vy, bool *alt)
{
        f64 ph = (f64)(i & 4095u) * 0.6283185307179586; /* ~2pi/10 turns */
        f64 ax = t * 0.163 + ph;
        f64 ay = t * 0.117 + ph * 1.9 + 0.7;
        f64 u = 0.34 * sin(ax) + 0.11 * sin(2.9 * ax + 1.3);
        f64 v = 0.30 * cos(ay) + 0.09 * sin(3.7 * ay + 0.6);
        /* curve tangent, scaled to a visible impulse, strength 0.35..1.0 */
        f64 str = 0.35 + 0.65 * ((f64)(fluid_fire_hash(i ^ 0xA5A5u) >> 44)
                                 / 1048576.0);
        f64 dx = 0.34 * 0.163 * cos(ax) + 0.11 * 0.163 * 2.9 * cos(2.9 * ax + 1.3);
        f64 dy = -0.30 * 0.117 * sin(ay) + 0.09 * 0.117 * 3.7 * cos(3.7 * ay + 0.6);
        *x = 0.5 + u;
        *y = 0.5 + v;
        *vx = dx * 10.0 * str;
        *vy = dy * 10.0 * str;
        *alt = (fluid_fire_hash(i ^ 0xDEADu) >> 20) & 1u;
}

/* Ambient breathing gain at ambient-clock time t: three slow incommensurate
 * LFOs, clamped to [0.25, 1.0] — the drift never fully stops (no dead
 * minutes) and never exceeds the configured strength. */
static inline f64 fluid_ambient_gain(f64 t)
{
        f64 g = 0.60 + 0.24 * sin(t * 0.041 + 0.8)
                     + 0.12 * sin(t * 0.107 + 2.3)
                     + 0.06 * sin(t * 0.271 + 4.1);
        if (g < 0.25)
                g = 0.25;
        if (g > 1.0)
                g = 1.0;
        return g;
}

#endif /* REV_FLUID_SCHED_H */
