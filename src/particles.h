/* ============================================================================
 * Reverie Engine — particles.h
 * Data-driven particle system: a PartCfg fully describes a layer's look
 * and motion — spawn regions, velocity cones, force fields (wind, drag,
 * turbulence, sway, pointer attraction), lifetime/size envelopes, fade and
 * flicker, N-stop color ramps, and sprite shapes with identical math on
 * the CPU raster, the GL point-sprite path, and the Vulkan compute path.
 * The named presets (rain/snow/embers/petals/fireflies) are nothing more
 * than known-good PartCfg bundles — every field stays overridable, so a
 * theme authors new looks without touching engine code.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_PARTICLES_H
#define REV_PARTICLES_H

#include "rev_types.h"

enum {
        RPSPAWN_AMBIENT = 0,
        RPSPAWN_POINTER,
        RPSPAWN_BOTH
};

enum {
        RPPRESET_CUSTOM = 0,
        RPPRESET_RAIN,
        RPPRESET_SNOW,
        RPPRESET_EMBERS,
        RPPRESET_PETALS,
        RPPRESET_FIREflies /* 'fireflies' */
};

/* Where ambient particles are born. */
enum {
        RPEMIT_TOP = 0,     /* across the top edge (rain, snow)          */
        RPEMIT_BOTTOM,      /* across the bottom edge (embers)           */
        RPEMIT_LEFT,
        RPEMIT_RIGHT,
        RPEMIT_EDGES,       /* any of the four edges, chosen at random   */
        RPEMIT_AREA,        /* inside emit_area (relative x,y,w,h box)   */
        RPEMIT_EVERYWHERE   /* the whole frame (fireflies)               */
};

/* Sprite shapes. The coverage math is mirrored bit-for-bit-ish across the
 * three rasterizers (CPU sprite loop, FS_PARTICLES, particles.comp) — the
 * e2e parity legs hold them to mean<=2/p95<=8 of each other. */
enum {
        RSHAPE_SOFT = 0,    /* (1-d^2)^2 falloff — the classic sprite    */
        RSHAPE_DISC,        /* hard disc with a ~1px antialiased edge    */
        RSHAPE_GLOW,        /* hot core + wide skirt, zero at the rim    */
        RSHAPE_SPARK,       /* thin four-way cross                       */
        RSHAPE_STAR,        /* sharp four-point star                     */
        RSHAPE_RING         /* rim band                                  */
};

#define RP_MAX_RAMP 8

/* The full authoring surface of a particles layer (one per layer). */
typedef struct {
        /* ---- emission ---- */
        int      spawn;          /* RPSPAWN_*: ambient | pointer | both  */
        int      emit_from;      /* RPEMIT_*                             */
        f64      area[4];        /* x, y, w, h relative (RPEMIT_AREA)    */
        f64      rate;           /* particles per second                 */

        /* ---- initial motion ---- */
        f64      dir_deg;        /* 0 = right, 90 = down (screen space)  */
        f64      spread_deg;     /* random cone around dir, +- half      */
        f64      speed0, speed1; /* initial speed range, px/s            */

        /* ---- continuous fields ---- */
        f64      gravity;        /* px/s^2, + pulls down                 */
        f64      wind[2];        /* constant accel px/s^2 (x, y)         */
        f64      turbulence;     /* 0..1 random wander                   */
        f64      sway;           /* 0..1 sinusoidal sway (leaves, snow)  */
        f64      drag;           /* velocity damping, 1/s                */
        f64      attract;        /* px/s^2 toward the pointer; <0 repels */

        /* ---- lifetime & size ---- */
        f64      life0, life1;   /* seconds, random in range             */
        f64      size0, size1;   /* sprite radius at birth, px range     */
        f64      size_out;       /* radius scale at end of life          */

        /* ---- appearance ---- */
        f64      fade_in;        /* seconds ramping opacity in           */
        f64      fade_out;       /* seconds ramping out; < 0 = the
                                  * classic fade over the last 20%       */
        f64      flicker;        /* 0..1 sin brightness wobble           */
        int      shape;          /* RSHAPE_*                             */
        RColor   ramp[RP_MAX_RAMP];  /* per-particle color ramp          */
        f64      ramp_t[RP_MAX_RAMP]; /* 0..1 sampling offsets          */
        int      nramp;

        /* ---- click bursts ---- */
        int      burst;          /* sparks per click                     */
        f64      burst_speed0, burst_speed1;
        f64      burst_size;     /* nominal spark radius, px             */

        /* ---- internal (preset-supplied, not theme-facing) ---- */
        bool     streaks;        /* rain's velocity streaks; rendered on
                                  * the CPU sprite path (not a point
                                  * sprite), so GPU offload declines    */
} PartCfg;

/* Fills *cfg with a preset's known-good bundle (CUSTOM = plain defaults).
 * Callers then layer explicit theme properties on top: a preset is a
 * starting point, not a cage. */
void particles_preset(int preset, PartCfg *cfg);

typedef struct RevParticles RevParticles;

RevParticles *particles_new(u32 w, u32 h);
void particles_free(RevParticles *p);

/* Applies a full config. Call once after particles_new (and again on
 * hot-reload, should a layer's numbers change without a rebuild). */
void particles_setup(RevParticles *p, const PartCfg *cfg);

/* Live pointer position in [0..1] (window-relative) + button state. */
void particles_pointer(RevParticles *p, f64 x, f64 y, bool pressed);

/* One-shot burst of n sparks at (x, y) — a full click, possibly entirely
 * between two polls (the platform layer latches such edges). x/y in [0..1]. */
void particles_burst(RevParticles *p, f64 x, f64 y, int n);

void particles_step(RevParticles *p, f64 dt);
void particles_render(const RevParticles *p, u32 *px, u32 w, u32 h);

/* GPU point-sprite offload: packs live particles into two float bands
 * (band0: x, y, size, alpha; band1: r, g, b, SHAPE — same size/alpha
 * math as particles_render, including envelopes and flicker). Each band
 * holds `slots` entries of 4 floats in 1024-wide row layout. Returns
 * the number of particles packed. */
u32 particles_pack(const RevParticles *p, f32 *band0, f32 *band1, u32 slots);

#endif /* REV_PARTICLES_H */
