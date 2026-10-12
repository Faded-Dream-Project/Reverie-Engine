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

        /* optional accelerated path: REV_WAYLAND_GPU=1 activates the shim,
         * which then owns display/surfaces and provides native handles. */
        if (wlshim_active()) {
                p->has_gpu = true;
                c->wl = NULL;
                return wlshim_platform_init(p, err, errcap);
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

static bool pw_present_cpu(RPlatform *p, int win, const u32 *rgba, u32 w, u32 h)
{
        PlatWayland *c = (PlatWayland *)p->ctx;
        if (wlshim_active())
                return wlshim_present_cpu(win, rgba, w, h);    /* shim owns surfaces; CPU present not used */
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
        memset(out, 0, sizeof *out);
        f64 x, y, dx, dy;
        bool pressed;
        if (!wl_pointer(c->wl, &x, &y, &dx, &dy, &pressed))
                return false;
        out->valid = true;
        out->x = x;
        out->y = y;
        out->dx = dx;
        out->dy = dy;
        out->pressed = pressed;
        return true;
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
        p->add_window = pw_add_window;
        p->present_cpu = pw_present_cpu;
        p->has_gpu = false;    /* set dynamically in init() */
        p->native_handle = pw_native_handle;
        p->native_display = pw_native_display;
        p->resize_window = pw_resize_window;
        p->pump = pw_pump;
        p->pointer = pw_pointer;
        p->win_geometry = pw_win_geometry;
        p->sync = pw_sync;
        p->should_close = pw_should_close;
        p->screenshot = pw_screenshot;
        return p;
}
