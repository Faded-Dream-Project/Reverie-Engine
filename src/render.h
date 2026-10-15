/* ============================================================================
 * Reverie Engine — render.h
 * Renderer abstraction with a DETERMINISTIC, documented selection policy.
 *
 * Explicit requests never fall back silently:
 *     --renderer vulkan   Vulkan only (best hardware device; a software
 *                         Vulkan device is USED and REPORTED as software —
 *                         explicit requests are honored, never substituted)
 *     --renderer opengl   OpenGL only (llvmpipe likewise used + reported)
 *     --renderer cpu      the native CPU compositor
 *     failure             the engine EXITS with the init error — the log
 *                         shows exactly which tier failed and why
 *
 * Automatic selection (documented, measured, no software layers without
 * benefit — see docs/BACKENDS.md for the rationale and benchmarks):
 *     1. the Vulkan tier on a HARDWARE device — the GPU compute
 *        compositing tier (every layer op and the fluid/ripple solvers
 *        run as compute kernels)
 *     2. OpenGL with a HARDWARE context — renders and simulates on the
 *        GPU; the long-proven tier
 *     3. the native CPU renderer otherwise (a software GL or lavapipe
 *        stack would initialize an entire software pipeline to deliver
 *        what the CPU renderer does natively; REV_AUTO_GL_SW /
 *        REV_AUTO_VK opt in anyway)
 *
 * All GPU backends are loaded with dlopen() at runtime and never linked.
 *
 * Frame protocol (per window):
 *   begin(win, w, h)                       start a frame
 *   layer(cmd) xN                          GPU-composite mode only
 *   end(cpu_frame_or_NULL)                 present; cpu_frame != NULL means
 *                                          the engine already composited
 *
 * Renderers with wants_layers=true receive per-layer commands (raster,
 * blend mode, opacity, blur sigma, placement) and do blur + blending
 * themselves on the GPU. Otherwise the engine composites on the CPU and
 * hands the finished frame to end().
 *
 * Renderers with wants_sims=true ALSO own layer simulations: fluid layers
 * arrive as RSimFluid descriptors (cmd.sim != NULL, cmd.px == NULL) and are
 * stepped entirely on the GPU — the CPU never touches a per-pixel loop for
 * them. Fixed at init() time (float render targets are probed there), so
 * the scene knows before rendering whether to offload or run the CPU sim.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_RENDER_H
#define REV_RENDER_H

#include "composite.h"
#include "platform.h"
#include "rev_types.h"

/* Per-frame fluid simulation descriptor (GPU offload). The renderer keys
 * its GPU state on the struct's ADDRESS (stable per layer instance /
 * monitor, owned by the scene) and mirrors the CPU solver's grid sizing
 * from dye_w/dye_h. Pointer coordinates are layer-normalized [0..1],
 * deltas consume-on-read. */
typedef struct {
        f64 dt;                /* step seconds (already clamped)        */
        f64 px, py;            /* pointer position, [0..1]              */
        f64 pdx, pdy;          /* per-frame pointer delta               */
        bool pressed;          /* button held                           */
        f64 force, radius;     /* animated splat strength / size        */
        f64 dissip, ambient;   /* dye fade / autonomous drift           */
        RColor dye_a, dye_b;   /* splat palette                         */
        u32 frame_w, frame_h;  /* device-pixel size of the layer        */
        u32 dye_w, dye_h;      /* dye grid (matches the CPU solver)     */
} RSimFluid;

/* Per-frame ripple simulation descriptor (GPU offload). Same identity
 * contract as RSimFluid: address-stable, owned by the scene's layer
 * instance; the renderer runs the height-field wave equation and the
 * refraction composite entirely on the GPU — the CPU never touches a
 * per-pixel loop. Grid sizing mirrors ripple_new() exactly. */
typedef struct {
        f64 dt;                /* step seconds (already clamped)        */
        f64 px, py;            /* pointer position, [0..1]              */
        bool pressed;          /* button held (continuous drip)         */
        f64 strength, damping, radius;
        f64 click_x[4], click_y[4];  /* queued one-shot impacts, [0..1] */
        u32 nclicks;           /* 0..4, consume-on-read                 */
        u32 frame_w, frame_h;  /* device-pixel size of the layer        */
        u32 gw, gh;            /* height field grid (matches ripple_new)*/
} RSimRipple;

/* Per-frame particle layer descriptor (GPU raster offload). The CPU keeps
 * the lightweight per-particle state update (position/velocity/life —
 * microseconds for sane counts) and packs the LIVE particles into two
 * float bands for one tiny RGBA32F texture upload; the renderer draws them
 * as point sprites and blends them into a layer texture on the GPU. This
 * replaces the old full-resolution CPU sprite raster + full-frame RGBA
 * upload every frame. Band 0: (x, y, size, alpha) per particle; band 1:
 * (r, g, b, 0). Bands are `slots` entries of 4 floats each, laid out as
 * 1024-wide rows (particle i at column i%1024, row i/1024; band 1 starts
 * at row slots/1024). */
typedef struct {
        u32 n;                 /* live particles packed below            */
        u32 slots;             /* band capacity in 4-float entries       */
        const f32 *band0;      /* position/size/alpha                    */
        const f32 *band1;      /* color                                 */
        u32 frame_w, frame_h;  /* device-pixel size of the layer         */
} RSimParticles;

/* Video frame descriptor (GPU plane upload + shader color conversion).
 * Planes are YUV420P (BT.601 limited range — the overwhelming default for
 * wallpaper loops) borrowed from the decoder's current frame; they stay
 * valid until the layer's next render call. */
typedef struct {
        const u8 *y, *u, *v;
        int ls_y, ls_u, ls_v;    /* plane line sizes, bytes            */
        u32 w, h;                /* coded size                         */
} RSimVideo;

/* Gradient layer descriptor (GPU evaluation). The CPU rasterizes gradient
 * layers at LUT speed but ANIMATED gradients rebuild a full-resolution
 * raster every frame and upload it (8MB at 1080p); the renderer can instead
 * evaluate the gradient directly in a shader into a cached texture — zero
 * CPU raster, zero per-frame upload for static gradients, one fullscreen
 * pass when the angle animates. Geometry is pre-resolved to PIXEL space
 * exactly like build_gradient_raster(); stops are sorted+capped the way
 * build_lut() does. */
typedef struct {
        int shape;             /* 0 linear, 1 radial                    */
        f64 p0x, p0y, p1x, p1y; /* linear axis, pixels                 */
        f64 cx, cy, radius;    /* radial center + radius, pixels        */
        RColor stops[16];      /* sorted, offsets in offs[]             */
        f64 offs[16];
        int nstops;
        u32 frame_w, frame_h;
} RGradDesc;

/* One layer emission (GPU compositing). Pixels are straight-alpha RGBA8
 * packed u32 — identical to RFb contents — UNLESS sim != NULL, in which case
 * px is NULL and the renderer owns that layer's simulation. */
typedef struct {
        const u32 *px;
        u32 w, h;              /* texture size                          */
        u32 place_w, place_h;  /* placement size; 0 = same as w,h —
                                  lets sub-frame-resolution sims (fluid
                                  dye fields) stretch over the frame with
                                  linear sampling instead of paying a
                                  full-res CPU upscale every frame      */
        int blend;             /* REV_BLEND_*                           */
        f64 opacity;           /* 0..1                                  */
        f64 blur;              /* gaussian sigma, px; 0 = none          */
        i32 dx, dy;            /* top-left placement into the target    */
        bool linear;           /* linear texture filtering              */
        u64 content_id;        /* non-zero: raster serial — renderers
                                  skip re-upload while it is unchanged;
                                  0: pixels change every frame        */
        const RSimFluid *sim;  /* fluid layer handed to the GPU; NULL
                                  for plain pixel layers              */
        const RSimRipple *rsim;/* ripple layer handed to the GPU; NULL
                                  unless this is a GPU ripple layer   */
        const RSimParticles *psim; /* particle layer rasterized on the
                                  GPU as point sprites; NULL otherwise */
        const RGradDesc *grad;    /* gradient evaluated on the GPU; NULL
                                  for plain pixel layers             */
        const RSimVideo *vsim;    /* video planes uploaded + converted on
                                  the GPU; NULL otherwise            */
} RLayerCmd;

typedef struct RRenderer RRenderer;

struct RRenderer {
        const char *name;   /* "vulkan" | "opengl" | "cpu" */
        int tier;           /* 3 = vulkan, 2 = opengl, 1 = cpu */
        bool wants_layers;  /* GPU compositing of RLayerCmd streams */

        /* honest hardware/software classification, set during init():
         * software=true means the API is running on a CPU implementation
         * (llvmpipe GL, lavapipe Vulkan, or the native CPU renderer).
         * device_desc names the actual device/driver in use. Selection and
         * the daemon log use these so "initialized" is never confused with
         * "hardware accelerated". */
        bool software;
        char device_desc[96];
        bool wants_sims;    /* GPU-owned layer simulations (fluid, ripple) —
                               settled during init(), never after */
        bool wants_points;  /* GPU point-sprite rasterization of particle
                               layers — false on software rasterizers where
                               wide-point rendering is pathologically slow
                               (llvmpipe); the CPU sprite raster is faster
                               there. Settled during init(), never after. */
        bool wants_grads;   /* GPU shader evaluation of gradient layers
                               (replaces the per-frame CPU raster + full-
                               frame upload of animated gradients). False
                               on software rasterizers. Settled at init. */
        bool wants_video;   /* GPU path for video layers: Y/U/V plane
                               upload + shader YUV->RGB (vs sws_scale +
                               full-frame RGBA upload). Settled at init. */

        bool (*init)(RRenderer *r, RPlatform *p, char *err, size_t errcap);
        void (*fini)(RRenderer *r);

        bool (*begin)(RRenderer *r, int win, u32 w, u32 h);
        bool (*layer)(RRenderer *r, const RLayerCmd *cmd);
        bool (*end)(RRenderer *r, const RFb *cpu_frame);
        void (*resize)(RRenderer *r, int win, u32 w, u32 h);

        /* Optional perf counters (read-and-reset, once per frame):
         *   cpu_ms — renderer CPU wall time that is a SUBSET of the caller's
         *            render measurement (GL: texture uploads; CPU tier: the
         *            present call). Lets REV_PROFILE break render= into its
         *            expensive parts.
         *   gpu_ms — ASYNCHRONOUS GPU execution time for the PREVIOUS frame,
         *            measured with driver timer queries where available.
         *            GPU work is queued, not synchronous: this number lags
         *            one frame behind and is 0.0 (not a lie, just unknown)
         *            on renderers without timer support. NULL = unimplemented. */
        bool (*perf)(RRenderer *r, f64 *cpu_ms, f64 *gpu_ms);

        void *ctx;                 /* implementation-private state       */
};

/* Includes composite.h's RFb. */

/* Creates a renderer under the documented selection policy (see above).
 * pref: "auto" | "vulkan" | "opengl" | "gl" | "cpu".
 * Explicit requests never fall back silently — init failure returns NULL
 * with the reason in err. "auto" is deterministic and never retries a
 * backend that already failed under the same conditions.
 * Returns NULL only for explicit-request failures or if even the CPU
 * renderer cannot initialize. */
RRenderer *renderer_create(const char *pref, RPlatform *p, char *err, size_t errcap);
void renderer_destroy(RRenderer *r);

#endif /* REV_RENDER_H */
