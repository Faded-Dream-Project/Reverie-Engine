/* ============================================================================
 * Reverie Engine — wlshim.c
 * Optional Wayland WSI shim (EXPERIMENTAL, opt-in via REV_WAYLAND_GPU=1).
 *
 * EGL and Vulkan need *libwayland-owned* wl_display/wl_surface handles to
 * create accelerated surfaces on Wayland — hand-rolled wire structs cannot
 * be handed to them. This shim drives a genuine libwayland-client
 * connection (dlopen("libwayland-client.so.0"), never linked) and exposes
 * the resulting handles to the renderers. The wire opcodes and the zwlr
 * layer-shell interface metadata below are hand-written; core-protocol
 * calls use the library's own exported symbols.
 *
 * When the shim is off (default), Wayland runs the fully hand-written raw
 * client (wayland.c) with the CPU compositor — zero libwayland involved.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "wlshim.h"

#include <dlfcn.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* ---- libwayland ABI types ------------------------------------------------ */
typedef struct wl_proxy wl_proxy_;
typedef struct wl_display wl_display_;
typedef struct wl_registry wl_registry_;
typedef struct wl_compositor wl_compositor_;
typedef struct wl_seat wl_seat_;
typedef struct wl_pointer wl_pointer_;
typedef struct wl_output wl_output_;
typedef struct wl_surface wl_surface_;
typedef int wl_fixed_t_;

struct wl_message_ {
        const char *signature;
        const void *const *types;
};

struct wl_interface_ {
        const char *name;
        int version;
        int method_count;
        const struct wl_message_ *methods;
        int event_count;
        const struct wl_message_ *events;
};

/* ---- zwlr layer-shell interface metadata (hand-written) ----------------- */
extern const struct wl_interface_ *zwlr_layer_shell_v1_iface;
extern const struct wl_interface_ *zwlr_layer_surface_v1_iface;

static const struct wl_message_ layer_shell_methods[] = {
        { "", NULL },                                    /* destroy                 */
        { "n?oos", NULL },                               /* get_layer_surface       */
};
static const struct wl_message_ layer_surface_methods[] = {
        { "uu", NULL },                                  /* set_size                */
        { "u", NULL },                                   /* set_anchor              */
        { "i", NULL },                                   /* set_exclusive_zone      */
        { "iiii", NULL },                                /* set_margin              */
        { "u", NULL },                                   /* set_keyboard_interactivity */
        { "n?o", NULL },                                 /* get_popup               */
        { "u", NULL },                                   /* ack_configure           */
};
static const struct wl_message_ layer_surface_events[] = {
        { "uuu", NULL },                                 /* configure(serial,w,h)  */
        { "", NULL },                                    /* closed                  */
};

static const struct wl_interface_ zwlr_layer_shell_v1_i = {
        "zwlr_layer_shell_v1", 1, 2, layer_shell_methods, 0, NULL
};
static const struct wl_interface_ zwlr_layer_surface_v1_i = {
        "zwlr_layer_surface_v1", 1, 7, layer_surface_methods, 2, layer_surface_events
};
const struct wl_interface_ *zwlr_layer_shell_v1_iface = &zwlr_layer_shell_v1_i;
const struct wl_interface_ *zwlr_layer_surface_v1_iface = &zwlr_layer_surface_v1_i;

/* types arrays referenced by "n?oos" signatures — filled at runtime with
 * dlsym'd libwayland interface symbols */
static const void *get_layer_surface_types[4];

/* ---- shim state ----------------------------------------------------------- */
typedef struct {
        int out;            /* monitor index */
        wl_proxy_ *layer_surface;
        wl_proxy_ *surface;
        bool configured;
        bool closed;
        u32 cfg_serial;
        u32 lw, lh;         /* logical size */
} ShimWin;

typedef struct {
        u32 name;
        char iface[64];
        u32 version;
} ShimGlobal;

typedef struct {
        wl_proxy_ *output;
        i32 lx, ly, pw, ph, scale;
        bool have_mode;
        int win;
} ShimOutput;

static struct {
        bool tried, active;
        void *lib;
        wl_display_ *display;
        wl_registry_ *registry;
        wl_compositor_ *compositor;
        wl_proxy_ *seat;
        wl_proxy_ *pointer;
        wl_proxy_ *layer_shell;
        const struct wl_interface_ *wl_surface_iface;
        const struct wl_interface_ *wl_output_iface;

        ShimGlobal *globals;   /* grown on demand — compositors advertise 50+ */
        int nglobals, globcap;
        ShimOutput outputs[8];
        int noutputs;
        ShimWin wins[8];
        int nwins;

        f64 px, py, pdx, pdy;
        bool pvalid, ppressed, plasted;
        int ptr_win;
        u32 seat_caps;
        bool close_requested;

        /* libwayland symbols */
        wl_display_ *(*connect)(const char *);
        void (*disconnect)(wl_display_ *);
        int (*dispatch)(wl_display_ *);
        int (*dispatch_pending)(wl_display_ *);
        int (*roundtrip)(wl_display_ *);
        int (*flush)(wl_display_ *);
        int (*get_fd)(wl_display_ *);
        wl_registry_ *(*get_registry)(wl_display_ *);
        int (*registry_add_listener)(wl_registry_ *, void (**)(void), void *);
        void *(*registry_bind)(wl_registry_ *, u32, const void *, u32);
        int (*proxy_add_listener)(wl_proxy_ *, void (**)(void), void *);
        void (*proxy_marshal)(wl_proxy_ *, u32, ...);
        wl_proxy_ *(*proxy_marshal_flags)(wl_proxy_ *, u32, const struct wl_interface_ *,
                                          u32, u32, ...);
        void (*proxy_destroy)(wl_proxy_ *);
        wl_surface_ *(*compositor_create_surface)(wl_compositor_ *);
        void (*surface_commit)(wl_surface_ *);
} S;

/* ---- listeners ------------------------------------------------------------ */

static void on_global(void *data, wl_registry_ *reg, u32 name, const char *iface, u32 version)
{
        (void)data;
        (void)reg;
        if (S.nglobals == S.globcap) {
                int ncap = S.globcap ? S.globcap * 2 : 64;
                ShimGlobal *ng = (ShimGlobal *)realloc(S.globals,
                                                       (size_t)ncap * sizeof *ng);
                if (!ng)
                        return;    /* drop on OOM; required globals bind below */
                S.globals = ng;
                S.globcap = ncap;
        }
        S.globals[S.nglobals].name = name;
        snprintf(S.globals[S.nglobals].iface, sizeof S.globals->iface, "%s", iface ? iface : "?");
        S.globals[S.nglobals].version = version;
        S.nglobals++;
}

static void on_global_remove(void *data, wl_registry_ *reg, u32 name)
{
        (void)data;
        (void)reg;
        (void)name;
}

static void on_seat_caps(void *data, wl_proxy_ *seat, u32 caps)
{
        (void)data;
        (void)seat;
        S.seat_caps = caps;
}

/* seat capabilities + pointer events + output events + layer surface events */

static void on_ptr_enter(void *data, wl_proxy_ *ptr, u32 serial, wl_proxy_ *surface,
                         wl_fixed_t_ sx, wl_fixed_t_ sy)
{
        (void)data; (void)ptr; (void)serial;
        for (int i = 0; i < S.nwins; i++) {
                if (S.wins[i].surface == surface) {
                        ShimOutput *out = &S.outputs[S.wins[i].out >= 0 ? S.wins[i].out : 0];
                        S.px = out->lx + (f64)sx / 256.0;
                        S.py = out->ly + (f64)sy / 256.0;
                        S.pvalid = true;
                        S.ptr_win = i;
                        return;
                }
        }
        S.ptr_win = -1;
}

static void on_ptr_motion(void *data, wl_proxy_ *ptr, u32 time, wl_fixed_t_ sx, wl_fixed_t_ sy)
{
        (void)data; (void)ptr; (void)time;
        if (!S.pvalid || S.ptr_win < 0 || S.ptr_win >= S.nwins)
                return;
        ShimOutput *out = &S.outputs[S.wins[S.ptr_win].out >= 0 ? S.wins[S.ptr_win].out : 0];
        f64 nx = out->lx + (f64)sx / 256.0;
        f64 ny = out->ly + (f64)sy / 256.0;
        if (S.plasted) {
                S.pdx = nx - S.px;
                S.pdy = ny - S.py;
        }
        S.px = nx;
        S.py = ny;
        S.plasted = true;
}

static void on_ptr_button(void *data, wl_proxy_ *ptr, u32 serial, u32 time, u32 button, u32 state)
{
        (void)data; (void)ptr; (void)serial; (void)time; (void)button;
        S.ppressed = state == 1;
}

static void on_output_geometry(void *data, wl_proxy_ *out, i32 x, i32 y, i32 pw, i32 ph,
                               i32 sub, const char *make, const char *model, i32 transform)
{
        (void)data; (void)sub; (void)make; (void)model; (void)transform;
        for (int i = 0; i < S.noutputs; i++)
                if (S.outputs[i].output == out) {
                        S.outputs[i].lx = x;
                        S.outputs[i].ly = y;
                        S.outputs[i].pw = pw;
                        S.outputs[i].ph = ph;
                        return;
                }
}

static void on_output_mode(void *data, wl_proxy_ *out, u32 flags, i32 w, i32 h, i32 refresh)
{
        (void)data; (void)refresh;
        if (!(flags & 1))
                return;
        for (int i = 0; i < S.noutputs; i++)
                if (S.outputs[i].output == out) {
                        S.outputs[i].pw = w;
                        S.outputs[i].ph = h;
                        S.outputs[i].have_mode = true;
                        return;
                }
}

static void on_output_scale(void *data, wl_proxy_ *out, i32 factor)
{
        (void)data;
        for (int i = 0; i < S.noutputs; i++)
                if (S.outputs[i].output == out) {
                        S.outputs[i].scale = factor < 1 ? 1 : factor;
                        return;
                }
}

static void on_layer_configure(void *data, wl_proxy_ *ls, u32 serial, u32 w, u32 h)
{
        (void)data;
        for (int i = 0; i < S.nwins; i++)
                if (S.wins[i].layer_surface == ls) {
                        S.wins[i].cfg_serial = serial;
                        if (w && h) {
                                S.wins[i].lw = w;
                                S.wins[i].lh = h;
                        }
                        S.wins[i].configured = true;
                        return;
                }
}

static void on_layer_closed(void *data, wl_proxy_ *ls)
{
        (void)data;
        for (int i = 0; i < S.nwins; i++)
                if (S.wins[i].layer_surface == ls) {
                        S.wins[i].closed = true;
                        S.close_requested = true;
                        return;
                }
}

/* ---- activation ------------------------------------------------------------- */

bool wlshim_active(void)
{
        if (!S.tried) {
                S.tried = true;
                const char *env = getenv("REV_WAYLAND_GPU");
                if (!env || strcmp(env, "1") != 0)
                        return false;
                S.lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
                if (!S.lib) {
                        LOGI("wlshim: libwayland-client.so.0 unavailable (%s) — staying on the raw client",
                             dlerror());
                        return false;
                }
                S.active = true;
        }
        return S.active;
}

/* ---- platform-level implementation ------------------------------------------- */

bool wlshim_platform_init(RPlatform *p, char *err, size_t errcap)
{
        (void)p;
        if (!S.active) {
                snprintf(err, errcap, "wlshim not active");
                return false;
        }

#define SYM(var, name) *(void **)(&S.var) = dlsym(S.lib, name)
        SYM(connect, "wl_display_connect");
        SYM(disconnect, "wl_display_disconnect");
        SYM(dispatch, "wl_display_dispatch");
        SYM(dispatch_pending, "wl_display_dispatch_pending");
        SYM(roundtrip, "wl_display_roundtrip");
        SYM(flush, "wl_display_flush");
        SYM(get_fd, "wl_display_get_fd");
        SYM(get_registry, "wl_display_get_registry");
        SYM(registry_add_listener, "wl_registry_add_listener");
        SYM(registry_bind, "wl_registry_bind");
        SYM(proxy_add_listener, "wl_proxy_add_listener");
        SYM(proxy_marshal, "wl_proxy_marshal");
        SYM(proxy_marshal_flags, "wl_proxy_marshal_flags");
        SYM(proxy_destroy, "wl_proxy_destroy");
        SYM(compositor_create_surface, "wl_compositor_create_surface");
        SYM(surface_commit, "wl_surface_commit");
#undef SYM
        if (!S.connect || !S.get_registry || !S.registry_add_listener || !S.registry_bind ||
            !S.proxy_add_listener || !S.proxy_marshal_flags || !S.compositor_create_surface) {
                snprintf(err, errcap, "wlshim: missing libwayland symbols");
                return false;
        }

        S.display = S.connect(NULL);
        if (!S.display) {
                snprintf(err, errcap, "wlshim: wl_display_connect failed");
                return false;
        }
        S.registry = S.get_registry(S.display);
        if (!S.registry) {
                snprintf(err, errcap, "wlshim: get_registry failed");
                return false;
        }

        static void (*const registry_listener[])(void) = {
                (void (*)(void))on_global,
                (void (*)(void))on_global_remove,
        };
        S.registry_add_listener(S.registry, (void (**)(void))registry_listener, NULL);

        /* surface/output interfaces (data symbols) for the metadata tables */
        *(void **)&S.wl_surface_iface = dlsym(S.lib, "wl_surface_interface");
        *(void **)&S.wl_output_iface = dlsym(S.lib, "wl_output_interface");
        if (!S.wl_surface_iface || !S.wl_output_iface) {
                snprintf(err, errcap, "wlshim: interface data symbols missing");
                return false;
        }
        get_layer_surface_types[0] = zwlr_layer_surface_v1_iface;
        get_layer_surface_types[1] = S.wl_surface_iface;
        get_layer_surface_types[2] = S.wl_output_iface;
        get_layer_surface_types[3] = NULL;
        ((struct wl_message_ *)&layer_shell_methods[1])->types = get_layer_surface_types;

        S.roundtrip(S.display);

        /* bind the globals we need */
        int have_shell = 0, have_comp = 0;
        for (int i = 0; i < S.nglobals; i++) {
                ShimGlobal *g = &S.globals[i];
                if (strcmp(g->iface, "wl_compositor") == 0) {
                        S.compositor = S.registry_bind(S.registry, g->name,
                                                       dlsym(S.lib, "wl_compositor_interface"),
                                                       g->version < 4 ? g->version : 4);
                        have_comp = S.compositor != NULL;
                } else if (strcmp(g->iface, "zwlr_layer_shell_v1") == 0) {
                        S.layer_shell = S.registry_bind(S.registry, g->name, zwlr_layer_shell_v1_iface,
                                                        g->version > 4 ? 4 : g->version);
                        have_shell = S.layer_shell != NULL;
                } else if (strcmp(g->iface, "wl_seat") == 0) {
                        S.seat = S.registry_bind(S.registry, g->name, dlsym(S.lib, "wl_seat_interface"),
                                                 g->version < 5 ? g->version : 5);
                } else if (strcmp(g->iface, "wl_output") == 0 && S.noutputs < 8) {
                        wl_output_ *out = S.registry_bind(S.registry, g->name,
                                                          dlsym(S.lib, "wl_output_interface"),
                                                          g->version < 4 ? g->version : 4);
                        if (out) {
                                static void (*const output_listener[])(void) = {
                                        (void (*)(void))on_output_geometry,
                                        (void (*)(void))on_output_mode,
                                        NULL,    /* done  */
                                        (void (*)(void))on_output_scale,
                                };
                                S.proxy_add_listener((wl_proxy_ *)out, (void (**)(void))output_listener, NULL);
                                S.outputs[S.noutputs].output = (wl_proxy_ *)out;
                                S.outputs[S.noutputs].scale = 1;
                                S.outputs[S.noutputs].win = -1;
                                S.noutputs++;
                        }
                }
        }
        if (!have_comp || !have_shell) {
                snprintf(err, errcap, "wlshim: compositor lacks wl_compositor or zwlr_layer_shell_v1");
                return false;
        }

        /* seat capabilities listener */
        if (S.seat) {
                static void (*const seat_listener[])(void) = {
                        (void (*)(void))on_seat_caps,
                        NULL,
                };
                S.proxy_add_listener(S.seat, (void (**)(void))seat_listener, NULL);
        }

        S.roundtrip(S.display);

        /* pointer: wl_seat_get_pointer (exported symbol) */
        if (S.seat) {
                wl_pointer_ *(*get_pointer)(wl_proxy_ *) = NULL;
                *(void **)(&get_pointer) = dlsym(S.lib, "wl_seat_get_pointer");
                if (get_pointer) {
                        S.pointer = (wl_proxy_ *)get_pointer((wl_proxy_ *)S.seat);
                        if (S.pointer) {
                                static void (*const pointer_listener[])(void) = {
                                        (void (*)(void))on_ptr_enter,
                                        NULL,             /* leave    */
                                        (void (*)(void))on_ptr_motion,
                                        (void (*)(void))on_ptr_button,
                                        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                };
                                S.proxy_add_listener(S.pointer, (void (**)(void))pointer_listener, NULL);
                        }
                }
        }

        S.roundtrip(S.display);
        return true;
}

void wlshim_platform_fini(RPlatform *p)
{
        (void)p;
        if (S.display) {
                S.disconnect(S.display);
                S.display = NULL;
        }
        free(S.globals);
        S.globals = NULL;
        S.globcap = 0;
        memset(&S, 0, sizeof S);
        S.tried = true;
}

int wlshim_monitors(RRect *out, int max)
{
        int n = S.noutputs < max ? S.noutputs : max;
        for (int i = 0; i < n; i++) {
                i32 s = S.outputs[i].scale < 1 ? 1 : S.outputs[i].scale;
                out[i].x = S.outputs[i].lx;
                out[i].y = S.outputs[i].ly;
                out[i].w = S.outputs[i].pw / s;
                out[i].h = S.outputs[i].ph / s;
        }
        return n;
}

f64 wlshim_monitor_scale(int mon)
{
        if (mon < 0 || mon >= S.noutputs)
                return 1.0;
        return (f64)(S.outputs[mon].scale < 1 ? 1 : S.outputs[mon].scale);
}

int wlshim_add_window(RRect geom)
{
        (void)geom;
        if (!S.active || S.nwins >= 8)
                return -1;

        int mon = -1;
        for (int i = 0; i < S.noutputs; i++)
                if (S.outputs[i].lx == geom.x && S.outputs[i].ly == geom.y) {
                        mon = i;
                        break;
                }
        if (mon < 0)
                mon = 0;

        int wi = S.nwins;
        ShimWin *win = &S.wins[wi];
        win->out = mon;
        S.outputs[mon].win = wi;

        win->surface = (wl_proxy_ *)S.compositor_create_surface(S.compositor);
        if (!win->surface)
                return -1;

        /* zwlr_layer_shell_v1.get_layer_surface(new_id, surface, output,
         * layer=BACKGROUND(1), namespace) — opcode 1, signature "n?oos" */
        wl_proxy_ *out_proxy = S.outputs[mon].output;
        win->layer_surface = S.proxy_marshal_flags(S.layer_shell, 1, zwlr_layer_surface_v1_iface,
                                                   0, 1, win->surface, out_proxy, 1, "reverie");
        if (!win->layer_surface)
                return -1;

        static void (*const layer_listener[])(void) = {
                (void (*)(void))on_layer_configure,
                (void (*)(void))on_layer_closed,
        };
        S.proxy_add_listener(win->layer_surface, (void (**)(void))layer_listener, NULL);

        /* set_anchor(all) opcode 1; set_exclusive_zone(-1) opcode 2;
         * set_keyboard_interactivity(none) opcode 4; set_size(w,h) opcode 0 */
        S.proxy_marshal(win->layer_surface, 1, 15u);
        S.proxy_marshal(win->layer_surface, 2, (i32)-1);
        S.proxy_marshal(win->layer_surface, 4, 0u);
        i32 s = S.outputs[mon].scale < 1 ? 1 : S.outputs[mon].scale;
        S.proxy_marshal(win->layer_surface, 0, (u32)(S.outputs[mon].pw / s), (u32)(S.outputs[mon].ph / s));
        S.surface_commit((wl_surface_ *)win->surface);
        S.flush(S.display);

        /* wait for configure */
        for (int spin = 0; spin < 100 && !win->configured; spin++) {
                S.dispatch(S.display);
        }
        if (!win->configured) {
                LOGW("wlshim: layer surface did not configure");
                return -1;
        }
        S.proxy_marshal(win->layer_surface, 6, win->cfg_serial);    /* ack_configure */
        S.flush(S.display);

        win->lw = (u32)(S.outputs[mon].pw / s);
        win->lh = (u32)(S.outputs[mon].ph / s);
        S.nwins++;
        return wi;
}

bool wlshim_present_cpu(int win, const u32 *rgba, u32 w, u32 h)
{
        /* In GPU mode the renderers present via EGL/Vulkan; the CPU presenter is
         * unavailable (log once). */
        static bool warned;
        (void)win; (void)rgba; (void)w; (void)h;
        if (!warned) {
                LOGW("wlshim: CPU present requested in GPU mode — skipped");
                warned = true;
        }
        return false;
}

void wlshim_pump(RPointerState *ptr)
{
        if (!S.active)
                return;
        struct pollfd p = { S.get_fd(S.display), POLLIN, 0 };
        if (poll(&p, 1, 0) > 0)
                S.dispatch(S.display);
        else
                S.dispatch_pending(S.display);
        if (ptr) {
                ptr->valid = S.pvalid;
                ptr->pressed = S.ppressed;
                ptr->x = S.px;
                ptr->y = S.py;
                ptr->dx = S.pdx;
                ptr->dy = S.pdy;
        }
        S.pdx = 0;
        S.pdy = 0;
}

bool wlshim_pointer(RPointerState *out)
{
        if (!out)
                return false;
        memset(out, 0, sizeof *out);
        if (!S.pvalid)
                return false;
        out->valid = true;
        out->x = S.px;
        out->y = S.py;
        out->dx = S.pdx;
        out->dy = S.pdy;
        out->pressed = S.ppressed;
        return true;
}

bool wlshim_win_geometry(int win, RRect *geom)
{
        if (win < 0 || win >= S.nwins)
                return false;
        int mon = S.wins[win].out;
        geom->x = S.outputs[mon].lx;
        geom->y = S.outputs[mon].ly;
        geom->w = (i32)S.wins[win].lw;
        geom->h = (i32)S.wins[win].lh;
        return true;
}

void wlshim_sync(void)
{
        if (S.active && S.display)
                S.roundtrip(S.display);
}

bool wlshim_should_close(void)
{
        return S.close_requested;
}

u64 wlshim_surface_handle(int win)
{
        if (win < 0 || win >= S.nwins)
                return 0;
        return (u64)(uintptr_t)S.wins[win].surface;
}

void *wlshim_display_handle(void)
{
        return S.active ? (void *)S.display : NULL;
}
