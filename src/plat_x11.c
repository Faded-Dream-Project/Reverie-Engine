/* ============================================================================
 * Reverie Engine — plat_x11.c
 * RPlatform implementation backed by the raw X11 wire client (x11.c).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "plat_x11.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"
#include "x11.h"

typedef struct {
        X11 *x;
        i32 last_px, last_py;
        bool have_last;
        RRect mons[REV_PLATFORM_MAX_OUTPUTS];
        int nmons;
        int winidx[REV_PLATFORM_MAX_OUTPUTS];
        /* idle power-save: cached display-power answer — the DPMS query is
         * a round trip, and power transitions are slow; re-ask at most every
         * 0.5 s while the frame loop polls every 16 ms */
        bool vis_last;
        bool vis_ok;            /* a successful answer is cached          */
        f64 vis_check;          /* last time we asked the server          */
        int vis_errors;         /* consecutive cannot-tell answers        */
        bool vis_broken;        /* gave up after repeated query errors    */
} PlatX11;

static bool px_init(RPlatform *p, char *err, size_t errcap)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        c->x = x11_connect(err, errcap);
        if (!c->x)
                return false;
        /* surface_visible only exists when the server can actually answer
         * a display-state question; otherwise NULL keeps the documented
         * "cannot tell — always render" contract */
        if (!x11_display_state_available(c->x))
                p->surface_visible = NULL;
        return true;
}

static void px_fini(RPlatform *p)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        if (c->x) {
                x11_disconnect(c->x);
                c->x = NULL;
        }
        free(c);
}

static int px_monitors(RPlatform *p, RRect *out, int max)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        int n = x11_monitors(c->x, out, max);
        if (n > REV_PLATFORM_MAX_OUTPUTS)
                n = REV_PLATFORM_MAX_OUTPUTS;
        c->nmons = n;
        memcpy(c->mons, out, (size_t)n * sizeof *out);
        return n;
}

static f64 px_monitor_scale(RPlatform *p, int mon)
{
        (void)p;
        (void)mon;
        return 1.0;    /* X11 core has no output scale concept */
}

static int px_monitor_refresh(RPlatform *p, int mon)
{
        (void)p;
        (void)mon;
        return 0;    /* X11 core protocol has no refresh rate; pacing uses scene fps */
}

static u64 px_monitor_id(RPlatform *p, int mon)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        return x11_monitor_id(c->x, mon);
}

static int px_add_window(RPlatform *p, RRect geom)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        return x11_add_window(c->x, geom);
}

static bool px_present_cpu(RPlatform *p, int win, const u32 *rgba, u32 w, u32 h,
                           bool premultiplied)
{
        /* X11 TrueColor presents have no alpha compositing convention: the
         * byte order is all that matters, straight or premultiplied alike */
        (void)premultiplied;
        PlatX11 *c = (PlatX11 *)p->ctx;
        return x11_present(c->x, win, rgba, w, h);
}

static u64 px_native_handle(RPlatform *p, int win)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        u32 xid = x11_window_xid(c->x, win);
        return xid ? (u64)xid : 0;
}

static void px_pump(RPlatform *p)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        x11_pump_events(c->x);
}

static bool px_pointer(RPlatform *p, RPointerState *out)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        if (!x11_pointer(c->x, out))
                return false;
        if (c->have_last) {
                out->dx = (f64)((i32)out->x - c->last_px);
                out->dy = (f64)((i32)out->y - c->last_py);
        }
        c->last_px = (i32)out->x;
        c->last_py = (i32)out->y;
        c->have_last = true;
        return true;
}

static bool px_wait_frame(RPlatform *p, f64 seconds)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        x11_wait_frame(c->x, seconds);
        return false;    /* no compositor frame signal on X11 core */
}

static u64 px_dropped_frames(RPlatform *p)
{
        (void)p;
        return 0;    /* MIT-SHM double-buffer presents are never dropped */
}

/* Idle power-save: the X11 signal is the DISPLAY POWER STATE, not frame
 * latching (there is no compositor feedback on core X11). DPMS level
 * on = visible; standby/suspend/off or an active screensaver = hidden —
 * rendering pauses exactly like the Wayland paths, and the first "on"
 * answer resumes it. The answer is cached 0.5 s; a server that errors on
 * the query three times in a row disables the check (always visible). */
static bool px_surface_visible(RPlatform *p)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        f64 now = rev_monotonic();
        if (c->vis_broken)
                return true;
        if (c->vis_ok && now - c->vis_check < 0.5)
                return c->vis_last;
        c->vis_check = now;
        int active = x11_display_active(c->x);
        if (active < 0) {
                if (++c->vis_errors >= 3 && !c->vis_broken) {
                        c->vis_broken = true;
                        LOGW("x11: display-state queries keep failing — "
                             "idle power-save disabled (always rendering)");
                        return true;
                }
                return c->vis_ok ? c->vis_last : true;   /* cannot tell */
        }
        c->vis_errors = 0;
        c->vis_ok = true;
        c->vis_last = active != 0;
        return c->vis_last;
}

static bool px_win_geometry(RPlatform *p, int win, RRect *geom)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        return x11_win_geometry(c->x, win, geom);
}

static void px_sync(RPlatform *p)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        x11_sync(c->x);
}

static void *px_native_display(RPlatform *p)
{
        (void)p;
        return NULL;    /* EGL default display opens its own X connection */
}

/* Destroy + re-create the window in the same slot (new XID). */
static bool px_resize_window(RPlatform *p, int win, RRect geom)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        return x11_resize_window(c->x, win, geom);
}

static bool px_remove_window(RPlatform *p, int win)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        return x11_remove_window(c->x, win);
}

static bool px_should_close(RPlatform *p)
{
        (void)p;
        return false;    /* X11 desktop windows are not "closed" by the server */
}

static u32 *px_screenshot(RPlatform *p, int win, u32 *w, u32 *h)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        return x11_screenshot(c->x, win, w, h);
}

RPlatform *plat_x11_create(void)
{
        RPlatform *p = (RPlatform *)calloc(1, sizeof *p);
        PlatX11 *c = (PlatX11 *)calloc(1, sizeof *c);
        if (!p || !c) {
                free(p);
                free(c);
                return NULL;
        }
        p->name = "x11";
        p->ctx = c;
        p->init = px_init;
        p->fini = px_fini;
        p->monitors = px_monitors;
        p->monitor_scale = px_monitor_scale;
        p->monitor_refresh = px_monitor_refresh;
        p->monitor_id = px_monitor_id;
        p->add_window = px_add_window;
        p->present_cpu = px_present_cpu;
        p->present_premul = false;    /* no premul convention on X11 presents */
        p->present_bgra = false;      /* x11.c converts into the visual's masks
                                       * itself; the engine's R,G,B,A order in */
        p->has_gpu = true;
        p->native_handle = px_native_handle;
        p->native_display = px_native_display;
        p->resize_window = px_resize_window;
        p->remove_window = px_remove_window;
        p->pump = px_pump;
        p->pointer = px_pointer;
        p->wait_frame = px_wait_frame;
        p->dropped_frames = px_dropped_frames;
        p->surface_visible = px_surface_visible;    /* NULLed by init when the
                                                     * server cannot answer */
        p->win_geometry = px_win_geometry;
        p->sync = px_sync;
        p->should_close = px_should_close;
        p->screenshot = px_screenshot;
        return p;
}

/* The window visual, for EGL config matching (0 on failure). */
u32 plat_x11_window_visual(RPlatform *p, int win)
{
        PlatX11 *c = (PlatX11 *)p->ctx;
        return c->x ? x11_win_visual(c->x, win) : 0;
}
