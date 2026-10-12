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

#define RT_FRAME_MAX_LAYERS 64

/* One emitted layer (GPU offload mode). Pixels are straight-alpha RGBA8,
 * matching RFb packing. */
typedef struct {
        const u32 *px;
        u32 w, h;
        int blend;         /* REV_BLEND_*              */
        f64 opacity;       /* 0..1                     */
        f64 blur;          /* gaussian sigma px        */
        i32 dx, dy;        /* placement into the frame */
} RTLayerCmd;

/* A frame's layer list, filled by rtscene_render_layers(). Pixel buffers
 * remain owned by the scene's per-monitor layer instances and stay valid
 * until the next render call. */
typedef struct {
        RTLayerCmd cmds[RT_FRAME_MAX_LAYERS];
        int n;
        u32 w, h;          /* target size for this frame */
} RTFrameList;

/* True when no layer needs below-composite feedback (ripple), i.e. the
 * frame can be built as an independent layer list for GPU compositing. */
bool rtscene_gpu_capable(const RTScene *sc);

/* Renders the scene for monitor `mon` into fb at time t (seconds since
 * scene start), advancing simulations by dt. */
void rtscene_render(RTScene *sc, RFb *fb, int mon, f64 t, f64 dt);

/* GPU offload render: fills `fl` instead of compositing on the CPU. */
void rtscene_render_layers(RTScene *sc, RTFrameList *fl, int mon, f64 t, f64 dt);

/* Drops caches (e.g. after hot-reload of assets). */
void rtscene_flush_caches(RTScene *sc);

/* Optional in-memory asset source (used for .revpack loading). `fn` returns
 * a pointer to `*len` bytes for `path`, or NULL to fall back to disk. */
typedef const u8 *(*RTAssetFn)(void *ctx, const char *path, size_t *len);
void rtscene_set_asset_provider(RTScene *sc, void *ctx, RTAssetFn fn);

#endif /* REV_SCENE_H */
