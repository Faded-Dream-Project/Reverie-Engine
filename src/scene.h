/* ============================================================================
 * Reverie Engine — scene.h
 * Runtime scene: validated layer configuration, animation evaluation,
 * per-monitor layer instances (raster caches + simulations), and the
 * frame compositor pipeline.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_SCENE_H
#define REV_SCENE_H

#include "composite.h"
#include "rev.h"
#include "rev_types.h"

enum {
	LT_IMAGE = 0,
	LT_GRADIENT,
	LT_SOLID,
	LT_FLUID,
	LT_RIPPLE,
	LT_PARTICLES
};

enum {
	FIT_FILL = 0,     /* cover  */
	FIT_CONTAIN,      /* fit    */
	FIT_STRETCH,
	FIT_TILE
};

#define RT_MAX_MONITORS 8

typedef struct RTScene RTScene;

/* Compiles IR into a runtime scene. `base_dir` resolves relative asset
 * paths. Returns NULL with a message on validation errors. */
RTScene *rtscene_compile(const RSceneIR *ir, RArena *ir_arena, const char *base_dir,
                         char *err, size_t errcap);
void rtscene_free(RTScene *sc);

const char *rtscene_title(const RTScene *sc);
int  rtscene_fps(const RTScene *sc);
int  rtscene_layer_count(const RTScene *sc);
bool rtscene_is_animated(const RTScene *sc);

/* Pointer state for a monitor (device pixels, monitor-local). */
void rtscene_pointer(RTScene *sc, int mon, f64 x, f64 y, f64 dx, f64 dy,
                     bool pressed);

/* Renders the scene for monitor `mon` into fb at time t (seconds since
 * scene start), advancing simulations by dt. */
void rtscene_render(RTScene *sc, RFb *fb, int mon, f64 t, f64 dt);

/* Drops caches (e.g. after hot-reload of assets). */
void rtscene_flush_caches(RTScene *sc);

#endif /* REV_SCENE_H */
