/* ============================================================================
 * Reverie Engine — fluid.h
 * Real-time 2D fluid simulation ("stable fluids", Jos Stam 1999) with
 * vorticity confinement, semi-Lagrangian advection, pressure projection,
 * and pointer-driven force/dye splats. The dye field is simulated at twice
 * the velocity resolution for crisp visuals.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_FLUID_H
#define REV_FLUID_H

#include "rev_types.h"

typedef struct RevFluid RevFluid;

/* Device-space size of the layer this fluid renders into. */
RevFluid *fluid_new(u32 w, u32 h, f64 res_scale);
void fluid_free(RevFluid *f);

void fluid_configure(RevFluid *f, RColor dye_a, RColor dye_b,
                     f64 force, f64 radius, f64 dissipation, f64 ambient);

/* Pointer in layer-relative [0..1] coordinates; dx/dy per-frame deltas. */
void fluid_pointer(RevFluid *f, f64 x, f64 y, f64 dx, f64 dy, bool pressed);

void fluid_step(RevFluid *f, f64 dt);

/* Renders dye into px (w x h from construction), straight alpha. */
void fluid_render(const RevFluid *f, u32 *px, u32 w, u32 h);

/* Dye grid size — the resolution the dye field actually exists at. */
void fluid_dye_size(const RevFluid *f, u32 *w, u32 *h);

#endif /* REV_FLUID_H */
