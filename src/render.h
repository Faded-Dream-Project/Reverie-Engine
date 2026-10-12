/* ============================================================================
 * Reverie Engine — render.h
 * Renderer abstraction with an automatic capability chain:
 *
 *     vulkan -> opengl -> opengl-on-llvmpipe (software GL) -> cpu
 *
 * All GPU backends are loaded with dlopen() at runtime and never linked.
 * Every renderer degrades cleanly: init() failure falls through to the next
 * tier; the CPU renderer always works (it is the engine's own compositor).
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
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_RENDER_H
#define REV_RENDER_H

#include "composite.h"
#include "platform.h"
#include "rev_types.h"

/* One layer emission (GPU compositing). Pixels are straight-alpha RGBA8
 * packed u32 — identical to RFb contents. */
typedef struct {
        const u32 *px;
        u32 w, h;
        int blend;         /* REV_BLEND_* */
        f64 opacity;       /* 0..1 */
        f64 blur;          /* gaussian sigma, px; 0 = none */
        i32 dx, dy;        /* top-left placement into the target */
} RLayerCmd;

typedef struct RRenderer RRenderer;

struct RRenderer {
        const char *name;   /* "vulkan" | "opengl" | "cpu" */
        int tier;           /* 3 = vulkan, 2 = opengl, 1 = cpu */
        bool wants_layers;  /* GPU compositing of RLayerCmd streams */

        bool (*init)(RRenderer *r, RPlatform *p, char *err, size_t errcap);
        void (*fini)(RRenderer *r);

        bool (*begin)(RRenderer *r, int win, u32 w, u32 h);
        bool (*layer)(RRenderer *r, const RLayerCmd *cmd);
        bool (*end)(RRenderer *r, const RFb *cpu_frame);
        void (*resize)(RRenderer *r, int win, u32 w, u32 h);

        void *ctx;                 /* implementation-private state       */
};

/* Includes composite.h's RFb. */

/* Probes the chain. pref: "auto" | "vulkan" | "opengl" | "cpu".
 * With "auto": vulkan -> opengl -> cpu (Mesa's llvmpipe is simply an
 * OpenGL renderer whose GL_RENDERER string contains "llvmpipe").
 * Explicit preferences still fall through to CPU as a last resort,
 * logging why. Returns NULL only if even the CPU renderer fails. */
RRenderer *renderer_create(const char *pref, RPlatform *p, char *err, size_t errcap);
void renderer_destroy(RRenderer *r);

#endif /* REV_RENDER_H */
