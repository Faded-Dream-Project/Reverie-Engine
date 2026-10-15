/* ============================================================================
 * Reverie Engine — render_cpu.c
 * The built-in CPU renderer: the engine's own compositor + the platform's
 * SHM (X11) / wl_shm (Wayland) presenter. Always available; it is the final
 * tier of the chain and works even on systems with no GPU stack at all.
 *
 * Supports both frame protocols:
 *   - layer mode (wants_layers = true): blurs and blends RLayerCmd streams
 *     with composite.c — the exact same math the GPU shaders implement,
 *     so CPU and GPU output are directly comparable.
 *   - cpu_frame mode: presents an already-composited RFb.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>

#include "render_cpu.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

typedef struct {
        RPlatform *p;
        RFb fb[REV_PLATFORM_MAX_OUTPUTS];
        int cur_win;
        u32 cur_w, cur_h;
        f64 perf_present_ms;    /* wall time inside present_cpu (read-reset) */
} RenderCpu;

static bool cpu_init(RRenderer *r, RPlatform *p, char *err, size_t errcap)
{
        RenderCpu *c = (RenderCpu *)r->ctx;
        c->p = p;
        (void)err;
        (void)errcap;
        if (!p->present_cpu)
                return false;
        r->software = true;
        snprintf(r->device_desc, sizeof r->device_desc,
                 "native CPU compositor (%s)", p->name);
        return true;
}

static void cpu_fini(RRenderer *r)
{
        RenderCpu *c = (RenderCpu *)r->ctx;
        for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++)
                free(c->fb[i].px);
        free(c);
}

static bool cpu_begin(RRenderer *r, int win, u32 w, u32 h)
{
        RenderCpu *c = (RenderCpu *)r->ctx;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return false;
        c->cur_win = win;
        c->cur_w = w;
        c->cur_h = h;
        if (c->fb[win].w != w || c->fb[win].h != h) {
                free(c->fb[win].px);
                c->fb[win].px = (u32 *)malloc((size_t)w * h * 4);
                c->fb[win].w = w;
                c->fb[win].h = h;
        }
        if (!c->fb[win].px)
                return false;
        memset(c->fb[win].px, 0, (size_t)w * h * 4);
        return true;
}

static bool cpu_layer(RRenderer *r, const RLayerCmd *cmd)
{
        RenderCpu *c = (RenderCpu *)r->ctx;
        RFb *dst = &c->fb[c->cur_win];
        if (!dst->px || !cmd->px)
                return false;

        u32 pw = cmd->place_w ? cmd->place_w : cmd->w;
        u32 ph = cmd->place_h ? cmd->place_h : cmd->h;
        if (pw != cmd->w || ph != cmd->h) {
                /* sub-frame-resolution sim layer stretched over the frame:
                 * bilinear upscale, then the normal placed blend */
                u32 *tmp = (u32 *)malloc((size_t)pw * ph * 4);
                if (!tmp)
                        return false;
                rev_resample_bilinear((const u32 *)cmd->px, cmd->w, cmd->h,
                                      tmp, pw, ph);
                RFb src = { tmp, pw, ph };
                rev_comp_blend_at(dst, &src, cmd->dx, cmd->dy, cmd->blend, cmd->opacity);
                free(tmp);
                return true;
        }

        if (cmd->blur >= 0.5) {
                /* blur into a scratch copy, then blend the copy */
                RFb tmp = { NULL, cmd->w, cmd->h };
                tmp.px = (u32 *)malloc((size_t)cmd->w * cmd->h * 4);
                if (!tmp.px)
                        return false;
                memcpy(tmp.px, cmd->px, (size_t)cmd->w * cmd->h * 4);
                rev_blur(&tmp, cmd->blur);
                rev_comp_blend_at(dst, &tmp, cmd->dx, cmd->dy, cmd->blend, cmd->opacity);
                free(tmp.px);
        } else {
                RFb src = { (u32 *)cmd->px, cmd->w, cmd->h };
                rev_comp_blend_at(dst, &src, cmd->dx, cmd->dy, cmd->blend, cmd->opacity);
        }
        return true;
}

static bool cpu_end(RRenderer *r, const RFb *cpu_frame)
{
        RenderCpu *c = (RenderCpu *)r->ctx;
        const RFb *fb = cpu_frame ? cpu_frame : &c->fb[c->cur_win];
        if (!fb || !fb->px)
                return false;
        f64 t0 = rev_monotonic();
        bool ok = c->p->present_cpu(c->p, c->cur_win, fb->px, fb->w, fb->h, false);
        c->perf_present_ms += (rev_monotonic() - t0) * 1000.0;
        return ok;
}

static bool cpu_perf(RRenderer *r, f64 *cpu_ms, f64 *gpu_ms)
{
        RenderCpu *c = (RenderCpu *)r->ctx;
        if (cpu_ms)
                *cpu_ms = c->perf_present_ms;
        if (gpu_ms)
                *gpu_ms = 0.0;  /* no GPU on this tier */
        c->perf_present_ms = 0;
        return true;
}

static void cpu_resize(RRenderer *r, int win, u32 w, u32 h)
{
        RenderCpu *c = (RenderCpu *)r->ctx;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return;
        if (c->fb[win].w != w || c->fb[win].h != h) {
                free(c->fb[win].px);
                c->fb[win].px = (u32 *)malloc((size_t)w * h * 4);
                c->fb[win].w = w;
                c->fb[win].h = h;
        }
}

RRenderer *render_cpu_create(void)
{
        RRenderer *r = (RRenderer *)calloc(1, sizeof *r);
        RenderCpu *c = (RenderCpu *)calloc(1, sizeof *c);
        if (!r || !c) {
                free(r);
                free(c);
                return NULL;
        }
        r->name = "cpu";
        r->tier = 1;
        r->wants_layers = true;
        r->ctx = c;
        r->init = cpu_init;
        r->fini = cpu_fini;
        r->begin = cpu_begin;
        r->layer = cpu_layer;
        r->end = cpu_end;
        r->resize = cpu_resize;
        r->perf = cpu_perf;
        return r;
}
