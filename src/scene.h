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
#include "render.h"
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

/* Latched click edge (device pixels, monitor-local): queues a one-shot
 * splash on ripple layers and a spark burst on particle layers, consumed
 * by the next render. Safe before the first render. */
void rtscene_click(RTScene *sc, int mon, f64 x, f64 y);

#define RT_FRAME_MAX_LAYERS 64

/* One emitted layer (GPU offload mode) — identical to the renderer-side
 * RLayerCmd (render.h): pixels are straight-alpha RGBA8 packed u32, or a
 * GPU-owned simulation descriptor when .sim is set. */
typedef RLayerCmd RTLayerCmd;

/* A frame's layer list, filled by rtscene_render_layers(). Pixel buffers
 * remain owned by the scene's per-monitor layer instances and stay valid
 * until the next render call.
 *
 * INPUT fields the caller sets BEFORE the call:
 *   w, h          target size for this frame
 *   sim_offload   true: fluid layers are emitted as GPU simulation
 *                 descriptors (no CPU sim runs, no pixels) — only set
 *                 this when the renderer reported wants_sims. */
typedef struct {
        RTLayerCmd cmds[RT_FRAME_MAX_LAYERS];
        int n;
        u32 w, h;          /* target size for this frame */
        bool sim_offload;  /* renderer owns the sims (input flag) */
        bool point_offload; /* renderer rasterizes particle layers as GPU
                               point sprites (input flag; llvmpipe keeps the
                               CPU sprite raster — wide points crawl there) */
        bool grad_offload;  /* renderer evaluates gradient layers in shaders
                               (input flag; only when wants_grads) */
} RTFrameList;

/* True when the scene contains at least one particle layer. */
bool rtscene_has_particles(const RTScene *sc);

/* True when the scene contains at least one gradient layer. */
bool rtscene_has_gradients(const RTScene *sc);

/* True when the frame can be built as an independent layer list for GPU
 * compositing. Ripple layers need the renderer to own simulations (their
 * refraction composites against the below-frame ON the GPU); without sim
 * offload such scenes must composite on the CPU. */
bool rtscene_gpu_capable(const RTScene *sc, bool sim_offload);

/* Renders the scene for monitor `mon` into fb at time t (seconds since
 * scene start), advancing simulations by dt. */
void rtscene_render(RTScene *sc, RFb *fb, int mon, f64 t, f64 dt);

/* GPU offload render: fills `fl` instead of compositing on the CPU. */
void rtscene_render_layers(RTScene *sc, RTFrameList *fl, int mon, f64 t, f64 dt);

/* Stage timings accumulated by the LAST render call (either mode), in
 * milliseconds — sim step+render, static/animated raster builds. Read by
 * main's REV_PROFILE perf line for field triage; reading resets them. */
void rtscene_profile(RTScene *sc, f64 *sim_ms, f64 *raster_ms);

/* Drops caches (e.g. after hot-reload of assets). */
void rtscene_flush_caches(RTScene *sc);

/* Optional in-memory asset source (used for .revpack loading). `fn` returns
 * a pointer to `*len` bytes for `path`, or NULL to fall back to disk. */
typedef const u8 *(*RTAssetFn)(void *ctx, const char *path, size_t *len);
void rtscene_set_asset_provider(RTScene *sc, void *ctx, RTAssetFn fn);

/* Same as rtscene_compile, but asset existence may ALSO be satisfied
 * through a provider (theme folder cache or a packed container) — this is
 * how packed themes load WITHOUT their assets existing on disk next to
 * the pack. The provider is stored on the scene for decode-time use. */
RTScene *rtscene_compile_assets(const RSceneIR *ir, RArena *ir_arena,
                                const char *base_dir, void *asset_ctx, RTAssetFn asset_fn,
                                char *err, size_t errcap);

#endif /* REV_SCENE_H */
