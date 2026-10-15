/* ============================================================================
 * Reverie Engine — render.c
 * Renderer selection under the documented deterministic policy (render.h):
 *
 *   explicit request -> that renderer only; failure is FATAL (no silent
 *                        substitution — the log shows which tier failed)
 *   auto             -> vulkan-hardware, else opengl-hardware, else the
 *                        native CPU renderer (software GL / software
 *                        Vulkan initialize an entire software stack to
 *                        deliver what the CPU renderer already does
 *                        natively — see docs/BACKENDS.md for the measured
 *                        rationale; REV_AUTO_VK / REV_AUTO_GL_SW override
 *                        for compatibility testing)
 *
 * Every selection and every skip logs its reason, so the daemon log is a
 * complete audit trail of the decision.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "render.h"
#include "render_cpu.h"
#include "render_gl.h"
#include "render_vk.h"

typedef RRenderer *(*RevRendererCtor)(void);

static bool try_init(RRenderer *r, RPlatform *p, char *why, size_t cap)
{
        char err[256] = { 0 };
        if (r->init(r, p, err, sizeof err))
                return true;
        snprintf(why, cap, "%s: %s", r->name, err[0] ? err : "init failed");
        return false;
}

static void log_selected(RRenderer *r, const char *how)
{
        LOGI("renderer: %s (tier %d, %s)%s%s [%s]",
             r->name, r->tier,
             r->wants_layers
                 ? (r->software ? "GPU-API compositing on a software device"
                                : "GPU compositing")
                 : "CPU compositing",
             r->device_desc[0] ? " — " : "",
             r->device_desc[0] ? r->device_desc : "",
             r->software ? "SOFTWARE" : "hardware");
        (void)how;
}

RRenderer *renderer_create(const char *pref, RPlatform *p, char *err, size_t errcap)
{
        const bool want_vk = pref && !strcmp(pref, "vulkan");
        const bool want_gl = pref && (!strcmp(pref, "opengl") || !strcmp(pref, "gl"));
        const bool want_cpu = pref && !strcmp(pref, "cpu");

        char why[256] = { 0 };
        RRenderer *r = NULL;

        /* ---- explicit requests: honored exactly, no silent fallback ---- */
        if (want_vk) {
                r = render_vk_create();
                if (r && try_init(r, p, why, sizeof why)) {
                        log_selected(r, "explicit");
                        return r;
                }
                renderer_destroy(r);
                if (err && errcap)
                        snprintf(err, errcap,
                                 "renderer 'vulkan' requested explicitly and "
                                 "failed: %s (no silent fallback — use "
                                 "'-R auto' to let the engine choose)",
                                 why);
                return NULL;
        }
        if (want_gl) {
                r = render_gl_create();
                if (r && try_init(r, p, why, sizeof why)) {
                        log_selected(r, "explicit");
                        return r;
                }
                renderer_destroy(r);
                if (err && errcap)
                        snprintf(err, errcap,
                                 "renderer 'opengl' requested explicitly and "
                                 "failed: %s (no silent fallback — use "
                                 "'-R auto' to let the engine choose)",
                                 why);
                return NULL;
        }
        if (want_cpu) {
                r = render_cpu_create();
                if (r && try_init(r, p, why, sizeof why)) {
                        log_selected(r, "explicit");
                        return r;
                }
                renderer_destroy(r);
                if (err && errcap)
                        snprintf(err, errcap,
                                 "renderer 'cpu' requested explicitly and "
                                 "failed: %s", why);
                return NULL;
        }

        /* ---- automatic selection (deterministic) ------------------------
         * Order (2026-10 second revision): vulkan -> opengl -> cpu.
         * The Vulkan tier is the most capable — full GPU compute
         * compositing AND GPU sims — so a hardware device takes it first;
         * hardware OpenGL follows as the long-proven tier; the native CPU
         * renderer is the universal floor. Software stacks (llvmpipe,
         * lavapipe) boot an entire software pipeline to deliver what the
         * CPU renderer does natively, so they stay opt-in via the env
         * overrides — order between them only matters when both are
         * admitted, and then Vulkan wins (it is the higher tier). */
        const char *sw_gl_env = getenv("REV_AUTO_GL_SW");
        const char *auto_vk_env = getenv("REV_AUTO_VK");

        /* 1. hardware Vulkan — the GPU compute compositing tier (every
         *    layer op and the fluid/ripple solvers run as compute kernels) */
        {
                RRenderer *vk = render_vk_create();
                if (vk) {
                        if (try_init(vk, p, why, sizeof why)) {
                                if (!vk->software) {
                                        log_selected(vk, "auto: hardware Vulkan");
                                        return vk;
                                }
                                if (auto_vk_env && *auto_vk_env) {
                                        LOGI("renderer: auto — software Vulkan "
                                             "kept (REV_AUTO_VK=1)");
                                        log_selected(vk, "auto: software Vulkan (override)");
                                        return vk;
                                }
                                LOGI("renderer: auto — Vulkan initialized as SOFTWARE "
                                     "(%s); skipping it for the next tier "
                                     "(set REV_AUTO_VK=1 to keep software Vulkan)",
                                     vk->device_desc[0] ? vk->device_desc : "lavapipe");
                        } else {
                                LOGI("renderer: auto — Vulkan unavailable (%s)", why);
                        }
                        renderer_destroy(vk);
                }
        }

        /* 2. hardware OpenGL — renders and simulates on the GPU; the
         *    long-proven tier. */
        r = render_gl_create();
        if (r) {
                if (try_init(r, p, why, sizeof why)) {
                        if (!r->software) {
                                log_selected(r, "auto: hardware GL");
                                return r;
                        }
                        /* software GL (llvmpipe) — keep only when the
                         * compatibility override is set; otherwise drop it
                         * in favor of the native CPU renderer */
                        if (sw_gl_env && *sw_gl_env) {
                                LOGI("renderer: auto — software GL kept "
                                     "(REV_AUTO_GL_SW=1)");
                                log_selected(r, "auto: software GL (override)");
                                return r;
                        }
                        LOGI("renderer: auto — OpenGL initialized as SOFTWARE "
                             "(%s); skipping it for the native CPU renderer "
                             "(set REV_AUTO_GL_SW=1 to keep software GL)",
                             r->device_desc[0] ? r->device_desc : "llvmpipe");
                        renderer_destroy(r);
                        r = NULL;
                } else {
                        LOGI("renderer: auto — OpenGL unavailable (%s)", why);
                        renderer_destroy(r);
                        r = NULL;
                }
        }

        /* 3. the native CPU renderer — always works */
        r = render_cpu_create();
        if (r && try_init(r, p, why, sizeof why)) {
                log_selected(r, "auto");
                return r;
        }
        renderer_destroy(r);

        if (err && errcap)
                snprintf(err, errcap, "no renderer could be initialized (%s)", why);
        return NULL;
}

void renderer_destroy(RRenderer *r)
{
        if (!r)
                return;
        if (r->fini)
                r->fini(r);    /* frees ctx */
        free(r);
}
