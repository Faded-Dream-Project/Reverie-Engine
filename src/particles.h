/* ============================================================================
 * Reverie Engine — particles.h
 * CPU particle system with ambient presets and pointer-triggered emitters.
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

typedef struct RevParticles RevParticles;

RevParticles *particles_new(u32 w, u32 h);
void particles_free(RevParticles *p);

void particles_configure(RevParticles *p, int preset,
                         int count, f64 rate, f64 size, f64 gravity, f64 drift,
                         int spawn, RColor c0, RColor c1);

void particles_pointer(RevParticles *p, f64 x, f64 y, bool pressed);

/* One-shot burst of `n` sparks at (x, y) — a full click, possibly entirely
 * between two polls (the platform layer latches such edges). x/y in [0..1]. */
void particles_burst(RevParticles *p, f64 x, f64 y, int n);

void particles_step(RevParticles *p, f64 dt);
void particles_render(const RevParticles *p, u32 *px, u32 w, u32 h);

/* GPU point-sprite offload: packs live particles into two float bands
 * (band0: x, y, size, alpha; band1: r, g, b, 0 — same alpha math as
 * particles_render, including the fireflies/embers flicker). Each band
 * holds `slots` entries of 4 floats in 1024-wide row layout. Returns
 * the number of particles packed. */
u32 particles_pack(const RevParticles *p, f32 *band0, f32 *band1, u32 slots);

#endif /* REV_PARTICLES_H */
