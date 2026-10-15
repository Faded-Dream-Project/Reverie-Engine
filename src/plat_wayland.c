/* ============================================================================
 * Reverie Engine — plat_wayland.c
 * RPlatform implementation backed by the raw Wayland wire client (wayland.c).
 * When the optional WSI shim (wlshim.c) is active, GPU handles come from it.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "plat_wayland.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "wayland.h"
#include "wlshim.h"

typedef struct {
        WLDisplay *wl;
        RPointerState ptr;
        RRect mons[REV_PLATFORM_MAX_OUTPUTS];
        int nmons;
        int winmon[REV_PLATFORM_MAX_OUTPUTS];    /* window i -> monitor index */
} PlatWayland;

static bool pw_init(RPlatform *p, char *err, size_t errcap)
{
        PlatWayland *c = (PlatWayland *)p->ctx;

        /* GPU WSI shim (default ON, REV_WAYLAND_GPU=0 opts out): drives a
         * real libwayland-client connection so EGL/Vulkan get proper
         * wl_display/wl_surface handles — a present GPU should be used. When
         * it cannot come up (no libwayland-client, compositor without
         * layer-shell), fall back to the raw hand-written client; the
         * renderer chain then falls to the CPU tier on its own. */
        if (wlshim_active()) {
                if (wlshim_platform_init(p, err, errcap)) {
                        p->has_gpu = true;
                        c->wl = NULL;
                        return true;
                }
                LOGW("wayland GPU WSI unavailable (%s) — raw client, CPU present",
                     err && *err ? err : "init failed");
        }

        p->has_gpu = false;    /* raw mode: no usable native handles for EGL/Vulkan */
        c->wl = wl_connect(err, errcap);
        return c->wl != NULL;
}

static void pw_fini(RPlatform *p)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active()) {
                wlshim_platform_fini(p);
        } else if (c->wl) {
                wl_disconnect(c->wl);
                c->wl = NULL;
        }
        free(c);
}

static int pw_monitors(RPlatform *p, RRect *out, int max)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_monitors(out, max);
        int n = wl_monitors(c->wl, out, max);
        if (n > REV_PLATFORM_MAX_OUTPUTS)
                n = REV_PLATFORM_MAX_OUTPUTS;
        c->nmons = n;
        memcpy(c->mons, out, (size_t)n * sizeof *out);
        return n;
}

static f64 pw_monitor_scale(RPlatform *p, int mon)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_monitor_scale(mon);
        return wl_monitor_scale(c->wl, mon);
}

static int pw_monitor_refresh(RPlatform *p, int mon)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_monitor_refresh(mon);
        return wl_monitor_refresh(c->wl, mon);
}

static int pw_add_window(RPlatform *p, RRect geom)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_add_window(geom);

        /* find the monitor this geometry belongs to (by origin) */
        int mon = -1;
        for (int i = 0; i < c->nmons; i++) {
                if (c->mons[i].x == geom.x && c->mons[i].y == geom.y) {
                        mon = i;
                        break;
                }
        }
        if (mon < 0)
                mon = 0;
        int win = wl_add_window(c->wl, mon);
        if (win >= 0 && win < REV_PLATFORM_MAX_OUTPUTS)
                c->winmon[win] = mon;
        return win;
}

static bool pw_present_cpu(RPlatform *p, int win, const u32 *rgba, u32 w, u32 h,
                           bool premultiplied)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_present_cpu(win, rgba, w, h, premultiplied);    /* shim owns surfaces; CPU present not used */
        /* raw client: only the CPU renderer reaches this path (no usable
         * native handles for GPU tiers) — its frames are straight alpha */
        (void)premultiplied;
        return wl_present(c->wl, win, rgba, w, h);
}

static u64 pw_native_handle(RPlatform *p, int win)
{
        (void)p;
        if (wlshim_active())
                return wlshim_surface_handle(win);
        return 0;    /* raw client surfaces are not usable by external APIs */
}

static void *pw_native_display(RPlatform *p)
{
        (void)p;
        if (wlshim_active())
                return wlshim_display_handle();
        return NULL;
}

static void pw_pump(RPlatform *p)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active()) {
                wlshim_pump(&c->ptr);
                return;
        }
        wl_pump(c->wl);
}

static bool pw_pointer(RPlatform *p, RPointerState *out)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_pointer(out);
        return wl_pointer(c->wl, out);
}

static bool pw_wait_frame(RPlatform *p, f64 seconds)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_wait_frame(seconds);
        return wl_wait_frame(c->wl, seconds);
}

static u64 pw_dropped_frames(RPlatform *p)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_dropped_frames();
        return wl_dropped_frames(c->wl);
}

static bool pw_surface_visible(RPlatform *p)
{
        (void)p;
        /* shim path only: the raw client has no latched-frame evidence,
        * so it reports visible (never idles). */
        if (wlshim_active())
                return wlshim_surface_visible();
        return true;
}

static bool pw_wsi_frame_request(RPlatform *p, int win)
{
        (void)p;
        /* Wayland + shim only: attaches the latched-callback evidence the
         * Vulkan WSI present path needs (see wlshim.h). */
        if (wlshim_active())
                return wlshim_wsi_frame_request(win);
        return false;
}

static bool pw_hidden_render_probe(RPlatform *p)
{
        (void)p;
        if (wlshim_active())
                return wlshim_hidden_render_probe();
        return false;
}

static bool pw_win_geometry(RPlatform *p, int win, RRect *geom)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_win_geometry(win, geom);
        return wl_win_geometry(c->wl, win, geom);
}

static void pw_sync(RPlatform *p)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active()) {
                wlshim_sync();
                return;
        }
        wl_sync(c->wl);
}

static bool pw_should_close(RPlatform *p)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_should_close();
        return wl_should_close(c->wl);
}

static bool pw_resize_window(RPlatform *p, int win, RRect geom)
{
        (void)p;
        (void)win;
        (void)geom;
        /* layer surfaces track their output automatically; sizes update via
         * configure events handled in pump(). */
        return true;
}

static u32 *pw_screenshot(RPlatform *p, int win, u32 *w, u32 *h)
{
        (void)p;
        (void)win;
        (void)w;
        (void)h;
        return NULL;    /* no compositor-agnostic readback on wayland */
}

RPlatform *plat_wayland_create(void)
{
        RPlatform *p = (RPlatform *)calloc(1, sizeof *p);
        PlatWayland *c = (PlatWayland *)calloc(1, sizeof *c);
        if (!p || !c) {
                free(p);
                free(c);
                return NULL;
        }
        for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++)
                c->winmon[i] = -1;
        p->name = "wayland";
        p->ctx = c;
        p->init = pw_init;
        p->fini = pw_fini;
        p->monitors = pw_monitors;
        p->monitor_scale = pw_monitor_scale;
        p->monitor_refresh = pw_monitor_refresh;
        p->add_window = pw_add_window;
        p->present_cpu = pw_present_cpu;
        /* wl_shm ARGB8888 is a premultiplied format AND packs the u32 as
         * 0xAARRGGBB (memory bytes B,G,R,A) — the engine's native order is
         * 0xAABBGGRR, so presenters on this platform must swizzle R/B */
        p->present_premul = true;
        p->present_bgra = true;
        p->has_gpu = false;    /* set dynamically in init() */
        p->native_handle = pw_native_handle;
        p->native_display = pw_native_display;
        p->resize_window = pw_resize_window;
        p->pump = pw_pump;
        p->pointer = pw_pointer;
        p->wait_frame = pw_wait_frame;
        p->dropped_frames = pw_dropped_frames;
        p->surface_visible = pw_surface_visible;
        p->wsi_frame_request = pw_wsi_frame_request;
        p->hidden_render_probe = pw_hidden_render_probe;
        p->win_geometry = pw_win_geometry;
        p->sync = pw_sync;
        p->should_close = pw_should_close;
        p->screenshot = pw_screenshot;
        return p;
}
