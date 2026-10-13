#define _GNU_SOURCE
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
 * requests are marshalled through the library's EXPORTED wl_proxy_marshal*
 * entry points with its exported wl_*_interface data symbols — the inline
 * wrappers (wl_compositor_create_surface, wl_surface_commit,
 * wl_seat_get_pointer, ...) exist only in wayland-client-protocol.h and
 * are NOT dlsym-able symbols of the shared object.
 *
 * When the shim is off (default), Wayland runs the fully hand-written raw
 * client (wayland.c) with the CPU compositor — zero libwayland involved.
 *
 * v0.1.5: the listener tables must cover EVERY event the bound version can
 * deliver — libwayland indexes implementation[opcode] with NO bounds check,
 * so a short array turns the next v4-only event (wl_output.name/description)
 * into a call through out-of-bounds memory: a silent segfault on every real
 * compositor (KWin, weston >= 10) the moment layer-shell exists. The shim
 * also presents frames itself now (wl_shm pool + double-buffer cycling
 * through libwayland), so the default REV_VK_PRESENT=platform path works.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "wlshim.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* wl_message ABI (wayland-util.h) — THREE fields, name FIRST.  A two-field
 * declaration shifts every metadata table: the signature string lands in the
 * name slot, the signature slot reads NULL, and types reads out of bounds —
 * marshalling through such tables segfaults or silently sends nothing
 * (verified against libwayland 1.23.1 with a byte-logging probe). */
struct wl_message_ {
        const char *name;
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

/* types for "no?ous": [0]=new_id iface, [1]=surface, [2]=output —
 * interface data symbols dlsym'd from libwayland at init */
static const void *get_layer_surface_types[4];

static const struct wl_message_ layer_shell_methods[] = {
        /* request opcodes are the array indices (wlr-layer-shell-unstable-v1):
         *   0 get_layer_surface(new_id, surface, output, layer, namespace)
         *   1 destroy (since v3) */
        { "get_layer_surface", "no?ous", get_layer_surface_types },
        { "destroy",           "",       NULL                     },
};
static const struct wl_message_ layer_surface_methods[] = {
        /* request opcode order per protocol:
         *   0 set_anchor(u)              1 set_exclusive_zone(i)
         *   2 set_margin(iiii)           3 set_keyboard_interactivity(u)
         *   4 get_popup(n?o)            5 ack_configure(u)
         *   6 destroy                    7 set_layer (since v2)
         * There is NO set_size request — the compositor sizes all-edges
         * anchored surfaces and reports the result via configure(). */
        { "set_anchor",                 "u",    NULL },
        { "set_exclusive_zone",         "i",    NULL },
        { "set_margin",                 "iiii", NULL },
        { "set_keyboard_interactivity", "u",    NULL },
        { "get_popup",                  "n?o",  NULL },
        { "ack_configure",              "u",    NULL },
        { "destroy",                    "",     NULL },
        { "set_layer",                  "u",    NULL },
};
static const struct wl_message_ layer_surface_events[] = {
        { "configure", "uuu", NULL },
        { "closed",    "",    NULL },
};

static const struct wl_interface_ zwlr_layer_shell_v1_i = {
        "zwlr_layer_shell_v1", 4, 2, layer_shell_methods, 0, NULL
};
static const struct wl_interface_ zwlr_layer_surface_v1_i = {
        "zwlr_layer_surface_v1", 4, 8, layer_surface_methods, 2, layer_surface_events
};
const struct wl_interface_ *zwlr_layer_shell_v1_iface = &zwlr_layer_shell_v1_i;
const struct wl_interface_ *zwlr_layer_surface_v1_iface = &zwlr_layer_surface_v1_i;

/* ---- shim state ----------------------------------------------------------- */
typedef struct {
        int out;            /* monitor index */
        wl_proxy_ *layer_surface;
        wl_proxy_ *surface;
        bool configured;
        bool closed;
        u32 cfg_serial;
        u32 lw, lh;         /* logical size */
        /* v0.1.5 wl_shm present path (mirrors the raw client's proven
         * presenter, marshalled through libwayland): one pool per window,
         * two cycling buffers, release-driven reuse */
        wl_proxy_ *pool;
        wl_proxy_ *bufs[2];
        int pool_fd;
        void *pool_mem;
        size_t pool_size;
        u32 pw, ph;         /* buffer (device-pixel) size */
        bool buf_free[2];
        int cur;
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
        u32 comp_version;   /* version wl_compositor was bound at (damage caps) */
        wl_proxy_ *shm;      /* v0.1.5: wl_shm for the platform present path */
        wl_proxy_ *seat;
        wl_proxy_ *pointer;
        wl_proxy_ *layer_shell;
        u32 shell_version;   /* version the shell was bound at (caps layer_surface v) */
        const struct wl_interface_ *wl_surface_iface;
        const struct wl_interface_ *wl_output_iface;
        const struct wl_interface_ *wl_shm_pool_iface;
        const struct wl_interface_ *wl_buffer_iface;
        u64 presents;        /* frames delivered through wl_shm (log milestone) */

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

        /* libwayland symbols (EXPORTED functions only — the generated inline
         * wrappers like wl_display_get_registry / wl_registry_bind /
         * wl_compositor_create_surface exist only in the protocol headers
         * and are NOT symbols of the shared object) */
        wl_display_ *(*connect)(const char *);
        void (*disconnect)(wl_display_ *);
        int (*dispatch)(wl_display_ *);
        int (*dispatch_pending)(wl_display_ *);
        int (*roundtrip)(wl_display_ *);
        int (*flush)(wl_display_ *);
        int (*get_fd)(wl_display_ *);
        int (*proxy_add_listener)(wl_proxy_ *, void (**)(void), void *);
        void (*proxy_marshal)(wl_proxy_ *, u32, ...);
        wl_proxy_ *(*proxy_marshal_flags)(wl_proxy_ *, u32, const struct wl_interface_ *,
                                          u32, u32, ...);
        wl_proxy_ *(*proxy_marshal_constructor)(wl_proxy_ *, u32, const struct wl_interface_ *, ...);
        void (*proxy_destroy)(wl_proxy_ *);
        const struct wl_interface_ *wl_pointer_iface;
        const struct wl_interface_ *wl_registry_iface;
} S;

/* core-protocol request opcodes the inline wrappers would have sent:
 *   wl_display.get_registry        = 1
 *   wl_registry.bind               = 0
 *   wl_compositor.create_surface   = 0
 *   wl_seat.get_pointer            = 0
 *   wl_surface.commit              = 6                                    */
#define WL_DISPLAY_GET_REGISTRY      1
#define WL_REGISTRY_BIND             0
#define WL_COMPOSITOR_CREATE_SURFACE 0
#define WL_SEAT_GET_POINTER          0
#define WL_SURFACE_COMMIT            6

/* wl_registry.bind — opcode 0, marshalled exactly like the generated inline
 * wrapper does: proxy_marshal_flags(registry, 0, iface, version, 0,
 *                                   name, iface->name, version, NULL)
 * (the trailing NULL feeds the new_id vararg slot) */
static wl_proxy_ *bind_global(u32 name, const struct wl_interface_ *iface, u32 version)
{
        return S.proxy_marshal_flags((wl_proxy_ *)S.registry, WL_REGISTRY_BIND,
                                     iface, version, 0,
                                     name, iface->name, version, NULL);
}

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

/* wl_seat.name (v2+) — informational; must exist or libwayland ABORTS the
 * process on a NULL listener slot when the event arrives. */
static void on_seat_name(void *data, wl_proxy_ *seat, const char *name)
{
        (void)data; (void)seat; (void)name;
}

/* seat capabilities + pointer events + output events + layer surface events */

static void on_ptr_leave(void *data, wl_proxy_ *ptr, u32 serial, wl_proxy_ *surface)
{
        (void)data; (void)ptr; (void)serial;
        for (int i = 0; i < S.nwins; i++)
                if (S.wins[i].surface == surface) {
                        S.ptr_win = -1;
                        return;
                }
}

/* wl_pointer frame/axis family (v5+) — not used by the engine, but every
 * listener slot must be non-NULL or libwayland aborts on arrival. */
static void on_ptr_noop(void *data, wl_proxy_ *ptr)
{
        (void)data; (void)ptr;
}

static void on_ptr_noop_u32(void *data, wl_proxy_ *ptr, u32 a, u32 b, u32 c)
{
        (void)data; (void)ptr; (void)a; (void)b; (void)c;
}

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

/* wl_output.name / .description — v4 informational events every real
 * compositor delivers right after the bind (verified live against weston 14
 * and matching the KWin field report). The listener array below MUST carry
 * slots for them: libwayland calls implementation[opcode] unconditionally,
 * and a 4-slot table reads past its end into unrelated data — the silent
 * v0.1.4 daemon segfault at startup. */
static void on_output_name(void *data, wl_proxy_ *out, const char *name)
{
        (void)data; (void)out; (void)name;
}

static void on_output_description(void *data, wl_proxy_ *out, const char *desc)
{
        (void)data; (void)out; (void)desc;
}

/* wl_output.done — real compositors send it after the mode/scale burst; a
 * NULL slot here makes libwayland abort the whole process. */
static void on_output_done(void *data, wl_proxy_ *out)
{
        (void)data; (void)out;
}

/* wl_shm.format — the compositor sprays these after the bind; the shim's
 * wl_shm proxy must carry a listener or the events hit a NULL table. */
static void on_shm_format(void *data, wl_proxy_ *shm, u32 format)
{
        (void)data; (void)shm; (void)format;
}

/* wl_buffer.release — the compositor finished with a slot; mark it free so
 * the next present can reuse it (double-buffer cycling). */
static void on_buffer_release(void *data, wl_proxy_ *buf)
{
        ShimWin *win = (ShimWin *)data;
        if (!win)
                return;
        for (int i = 0; i < 2; i++)
                if (win->bufs[i] == buf) {
                        win->buf_free[i] = true;
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

static void shim_pool_destroy(ShimWin *win);    /* fwd: fini tears pools down */

bool wlshim_active(void)
{
        if (!S.tried) {
                S.tried = true;
                const char *env = getenv("REV_WAYLAND_GPU");
                if (!env || strcmp(env, "1") != 0)
                        return false;
                /* REV_WL_LIB: explicit .so path (tests pin a known library);
                 * default resolves the system libwayland-client. */
                const char *lib = getenv("REV_WL_LIB");
                if (!lib || !*lib)
                        lib = "libwayland-client.so.0";
                S.lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
                if (!S.lib) {
                        LOGI("wlshim: %s unavailable (%s) — staying on the raw client",
                             lib, dlerror());
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
        SYM(proxy_add_listener, "wl_proxy_add_listener");
        SYM(proxy_marshal, "wl_proxy_marshal");
        SYM(proxy_marshal_flags, "wl_proxy_marshal_flags");
        SYM(proxy_marshal_constructor, "wl_proxy_marshal_constructor");
        SYM(proxy_destroy, "wl_proxy_destroy");
#undef SYM
        if (!S.connect || !S.proxy_add_listener || !S.proxy_marshal_flags ||
            !S.proxy_marshal_constructor || !S.proxy_marshal || !S.roundtrip) {
                snprintf(err, errcap, "wlshim: missing libwayland symbols");
                return false;
        }

        S.display = S.connect(NULL);
        if (!S.display) {
                snprintf(err, errcap, "wlshim: wl_display_connect failed");
                return false;
        }

        /* surface/output/pointer/registry/shm interfaces (EXPORTED data
         * symbols) — request marshalling below and the types tables need them */
        *(void **)&S.wl_surface_iface = dlsym(S.lib, "wl_surface_interface");
        *(void **)&S.wl_output_iface = dlsym(S.lib, "wl_output_interface");
        *(void **)&S.wl_pointer_iface = dlsym(S.lib, "wl_pointer_interface");
        *(void **)&S.wl_registry_iface = dlsym(S.lib, "wl_registry_interface");
        *(void **)&S.wl_shm_pool_iface = dlsym(S.lib, "wl_shm_pool_interface");
        *(void **)&S.wl_buffer_iface = dlsym(S.lib, "wl_buffer_interface");
        if (!S.wl_surface_iface || !S.wl_output_iface || !S.wl_pointer_iface ||
            !S.wl_registry_iface) {
                snprintf(err, errcap, "wlshim: interface data symbols missing");
                return false;
        }
        if (!S.wl_shm_pool_iface || !S.wl_buffer_iface) {
                /* the wl_shm presenter needs these; without them GPU tiers
                 * can still WSI-present, but the default platform path cannot */
                LOGW("wlshim: wl_shm_pool/wl_buffer interface symbols missing — "
                     "CPU present disabled (WSI present still available)");
        }

        /* wl_display.get_registry — opcode 1, inline-only wrapper in the
         * headers, marshalled here exactly like the generated binding does
         * (the display singleton is protocol version 1; trailing NULL feeds
         * the new_id vararg slot) */
        S.registry = (wl_registry_ *)S.proxy_marshal_flags((wl_proxy_ *)S.display, WL_DISPLAY_GET_REGISTRY,
                                                          S.wl_registry_iface, 1, 0, NULL);
        if (!S.registry) {
                snprintf(err, errcap, "wlshim: get_registry failed");
                return false;
        }

        static void (*const registry_listener[])(void) = {
                (void (*)(void))on_global,
                (void (*)(void))on_global_remove,
        };
        S.proxy_add_listener((wl_proxy_ *)S.registry, (void (**)(void))registry_listener, NULL);

        get_layer_surface_types[0] = zwlr_layer_surface_v1_iface;
        get_layer_surface_types[1] = S.wl_surface_iface;
        get_layer_surface_types[2] = S.wl_output_iface;
        get_layer_surface_types[3] = NULL;

        S.roundtrip(S.display);

        /* bind the globals we need (via bind_global: wl_registry.bind
         * opcode 0, mirroring the generated inline binding) */
        int have_shell = 0, have_comp = 0;
        for (int i = 0; i < S.nglobals; i++) {
                ShimGlobal *g = &S.globals[i];
                if (strcmp(g->iface, "wl_compositor") == 0) {
                        const struct wl_interface_ *ci =
                                (const struct wl_interface_ *)dlsym(S.lib, "wl_compositor_interface");
                        S.compositor = ci ? (wl_compositor_ *)bind_global(g->name, ci, g->version < 4 ? g->version : 4)
                                          : NULL;
                        if (S.compositor)
                                S.comp_version = g->version < 4 ? g->version : 4;
                        have_comp = S.compositor != NULL;
                } else if (strcmp(g->iface, "wl_shm") == 0) {
                        /* v0.1.5 present path: create_pool/create_buffer are v1 */
                        const struct wl_interface_ *si =
                                (const struct wl_interface_ *)dlsym(S.lib, "wl_shm_interface");
                        S.shm = si ? bind_global(g->name, si, 1) : NULL;
                        if (S.shm) {
                                static void (*const shm_listener[])(void) = {
                                        (void (*)(void))on_shm_format,
                                };
                                S.proxy_add_listener(S.shm, (void (**)(void))shm_listener, NULL);
                        }
                } else if (strcmp(g->iface, "zwlr_layer_shell_v1") == 0) {
                        u32 v = g->version > 4 ? 4 : g->version;
                        S.layer_shell = bind_global(g->name, zwlr_layer_shell_v1_iface, v);
                        if (S.layer_shell)
                                S.shell_version = v;
                        have_shell = S.layer_shell != NULL;
                } else if (strcmp(g->iface, "wl_seat") == 0) {
                        const struct wl_interface_ *si =
                                (const struct wl_interface_ *)dlsym(S.lib, "wl_seat_interface");
                        S.seat = si ? bind_global(g->name, si, g->version < 5 ? g->version : 5) : NULL;
                } else if (strcmp(g->iface, "wl_output") == 0 && S.noutputs < 8) {
                        wl_output_ *out = (wl_output_ *)bind_global(g->name, S.wl_output_iface,
                                                                  g->version < 4 ? g->version : 4);
                        if (out) {
                                static void (*const output_listener[])(void) = {
                                        (void (*)(void))on_output_geometry,
                                        (void (*)(void))on_output_mode,
                                        (void (*)(void))on_output_done,
                                        (void (*)(void))on_output_scale,
                                        (void (*)(void))on_output_name,        /* v4 */
                                        (void (*)(void))on_output_description, /* v4 */
                                };
                                S.proxy_add_listener((wl_proxy_ *)out, (void (**)(void))output_listener, NULL);
                                S.outputs[S.noutputs].output = (wl_proxy_ *)out;
                                S.outputs[S.noutputs].scale = 1;
                                S.outputs[S.noutputs].win = -1;
                                S.noutputs++;
                        }
                }
        }

        /* seat capabilities listener (name event gets a no-op: NULL slots
         * make libwayland abort when the event arrives) */
        if (S.seat) {
                static void (*const seat_listener[])(void) = {
                        (void (*)(void))on_seat_caps,
                        (void (*)(void))on_seat_name,
                };
                S.proxy_add_listener(S.seat, (void (**)(void))seat_listener, NULL);
        }

        /* roundtrip #2 BEFORE the layer-shell gate: this dispatches the
         * post-bind event burst every real compositor sends (wl_output
         * geometry/scale/mode/name/description + done, wl_shm formats, seat
         * caps/name) — dispatching it here means even compositors WITHOUT
         * layer-shell exercise the full listener coverage on the error path
         * (that is how weston catches a short listener table in the E2E). */
        S.roundtrip(S.display);

        if (!have_comp || !have_shell) {
                snprintf(err, errcap,
                         "wlshim: compositor lacks wl_compositor or zwlr_layer_shell_v1 "
                         "(%d globals parsed)", S.nglobals);
                return false;
        }

        /* pointer: wl_seat.get_pointer — an inline-only wrapper in the
         * protocol headers, so marshal opcode 0 ourselves via the exported
         * constructor (new_id consumes the trailing NULL vararg). */
        if (S.seat) {
                S.pointer = S.proxy_marshal_constructor(S.seat, WL_SEAT_GET_POINTER,
                                                        S.wl_pointer_iface, NULL);
                if (S.pointer) {
                        static void (*const pointer_listener[])(void) = {
                                (void (*)(void))on_ptr_enter,
                                (void (*)(void))on_ptr_leave,
                                (void (*)(void))on_ptr_motion,
                                (void (*)(void))on_ptr_button,
                                (void (*)(void))on_ptr_noop_u32,    /* axis          */
                                (void (*)(void))on_ptr_noop,        /* frame    (v5) */
                                (void (*)(void))on_ptr_noop,        /* axis_source */
                                (void (*)(void))on_ptr_noop_u32,    /* axis_stop    */
                                (void (*)(void))on_ptr_noop_u32,    /* axis_discrete*/
                                (void (*)(void))on_ptr_noop_u32,    /* axis_value120*/
                        };
                        S.proxy_add_listener(S.pointer, (void (**)(void))pointer_listener, NULL);
                }
        }

        S.roundtrip(S.display);
        return true;
}

void wlshim_platform_fini(RPlatform *p)
{
        (void)p;
        if (S.display) {
                /* libwayland frees proxies ONLY on explicit wl_proxy_destroy
                 * (wl_display_disconnect releases the id maps, not the proxy
                 * structs) — destroy everything we created, children first,
                 * or every engine cycle leaks them (ASan-visible). */
                if (S.proxy_destroy) {
                        for (int i = 0; i < S.nwins; i++) {
                                shim_pool_destroy(&S.wins[i]);
                                if (S.wins[i].layer_surface)
                                        S.proxy_destroy(S.wins[i].layer_surface);
                                if (S.wins[i].surface)
                                        S.proxy_destroy(S.wins[i].surface);
                                S.wins[i].layer_surface = NULL;
                                S.wins[i].surface = NULL;
                        }
                        if (S.shm) {
                                S.proxy_destroy(S.shm);
                                S.shm = NULL;
                        }
                        if (S.pointer) {
                                S.proxy_destroy(S.pointer);
                                S.pointer = NULL;
                        }
                        if (S.seat) {
                                S.proxy_destroy(S.seat);
                                S.seat = NULL;
                        }
                        for (int i = 0; i < S.noutputs; i++)
                                if (S.outputs[i].output)
                                        S.proxy_destroy(S.outputs[i].output);
                        if (S.layer_shell) {
                                S.proxy_destroy(S.layer_shell);
                                S.layer_shell = NULL;
                        }
                        if (S.compositor) {
                                S.proxy_destroy((wl_proxy_ *)S.compositor);
                                S.compositor = NULL;
                        }
                        if (S.registry) {
                                S.proxy_destroy((wl_proxy_ *)S.registry);
                                S.registry = NULL;
                        }
                }
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

        /* wl_compositor.create_surface — inline-only wrapper in the headers;
         * marshal opcode 0 via the exported constructor (NULL = new_id slot). */
        win->surface = S.proxy_marshal_constructor((wl_proxy_ *)S.compositor,
                                                   WL_COMPOSITOR_CREATE_SURFACE,
                                                   S.wl_surface_iface, NULL);
        if (!win->surface)
                return -1;

        /* zwlr_layer_shell_v1.get_layer_surface — opcode 0, signature "no?ous":
         *   (new_id, surface, output, layer=BACKGROUND(0), namespace)
         * wl_proxy_marshal_flags ABI (verified against libwayland 1.23 and the
         * generated wlr-layer-shell bindings):
         *   (proxy, opcode, new_iface, VERSION, FLAGS, ...args incl. new_id slot)
         * - the new_id slot CONSUMES a vararg — pass NULL as its placeholder;
         *   omitting it shifts every later argument by one (garbage on the wire)
         * - flags: 0 (WL_MARSHAL_FLAG_DESTROY=1 would destroy the shell proxy)
         * - version: the shell's bound version becomes the new proxy's version */
        wl_proxy_ *out_proxy = S.outputs[mon].output;
        win->layer_surface = S.proxy_marshal_flags(S.layer_shell, 0, zwlr_layer_surface_v1_iface,
                                                   S.shell_version, 0,
                                                   NULL, win->surface, out_proxy, 0, "reverie");
        if (!win->layer_surface)
                return -1;

        static void (*const layer_listener[])(void) = {
                (void (*)(void))on_layer_configure,
                (void (*)(void))on_layer_closed,
        };
        S.proxy_add_listener(win->layer_surface, (void (**)(void))layer_listener, NULL);

        /* set_anchor(all) opcode 0; set_exclusive_zone(-1) opcode 1;
         * set_keyboard_interactivity(none) opcode 3 — there is no set_size
         * request; the compositor assigns the size of an all-edges-anchored
         * surface and delivers it via configure(). */
        S.proxy_marshal(win->layer_surface, 0, 15u);
        S.proxy_marshal(win->layer_surface, 1, (i32)-1);
        S.proxy_marshal(win->layer_surface, 3, 0u);
        i32 s = S.outputs[mon].scale < 1 ? 1 : S.outputs[mon].scale;
        /* wl_surface.commit (opcode 6) — inline-only wrapper, marshalled directly */
        S.proxy_marshal(win->surface, WL_SURFACE_COMMIT);
        S.flush(S.display);

        /* REGISTER the window BEFORE waiting: the configure (and any pointer)
         * events dispatched below must find this win in S.wins[] — listeners
         * search by proxy pointer, and S.nwins is the search bound. Registering
         * only after the wait (the old bug) made every configure event MISS,
         * so the dispatch loop blocked forever on the first follow-up read —
         * the daemon stayed alive with a black desktop. */
        S.nwins++;

        /* wait for configure */
        for (int spin = 0; spin < 100 && !win->configured; spin++) {
                if (S.dispatch(S.display) < 0)
                        break;           /* connection error: stop waiting */
        }
        if (!win->configured) {
                LOGW("wlshim: layer surface did not configure");
                S.nwins--;               /* retire the failed slot */
                S.outputs[mon].win = -1;
                win->surface = NULL;
                win->layer_surface = NULL;
                return -1;
        }
        S.proxy_marshal(win->layer_surface, 5, win->cfg_serial);    /* ack_configure */
        S.flush(S.display);

        win->lw = (u32)(S.outputs[mon].pw / s);
        win->lh = (u32)(S.outputs[mon].ph / s);
        return wi;
}

/* ---- wl_shm present path (v0.1.5) -------------------------------------------
 * Mirrors the raw client's field-proven presenter (wayland.c), but every
 * request is marshalled through real libwayland:
 *   wl_shm.create_pool        opcode 0, "nhi"  (new_id pool, fd, size)
 *   wl_shm_pool.create_buffer opcode 0, "niiiiu" (new_id buf, off, w, h, stride, fmt)
 *   wl_surface.attach         opcode 1, "o?ii" (buffer, 0, 0)
 *   wl_surface.set_buffer_scale opcode 8 (v3+)
 *   wl_surface.damage_buffer  opcode 9 (v4+) / damage opcode 2 (older)
 *   wl_surface.commit         opcode 6
 * Buffers cycle on wl_buffer.release events (listener above).                    */

static void shim_pool_destroy(ShimWin *win)
{
        if (!win->pool)
                return;
        for (int b = 0; b < 2; b++) {
                if (win->bufs[b] && S.proxy_destroy)
                        S.proxy_destroy(win->bufs[b]);
                win->bufs[b] = NULL;
                win->buf_free[b] = false;
        }
        if (S.proxy_destroy)
                S.proxy_destroy(win->pool);
        win->pool = NULL;
        if (win->pool_mem) {
                munmap(win->pool_mem, win->pool_size);
                win->pool_mem = NULL;
        }
        if (win->pool_fd >= 0) {
                close(win->pool_fd);
                win->pool_fd = -1;
        }
        win->pool_size = 0;
        win->pw = win->ph = 0;
        win->cur = 0;
}

static bool shim_pool_create(ShimWin *win, u32 w, u32 h)
{
        if (!S.shm || !S.wl_shm_pool_iface || !S.wl_buffer_iface) {
                LOGW("wlshim: wl_shm present unavailable (compositor or symbols)");
                return false;
        }
        shim_pool_destroy(win);

        win->pool_size = (size_t)w * h * 4 * 2;    /* two slots */
        win->pool_fd = memfd_create("reverie-wlshim", 0);
        if (win->pool_fd < 0) {
                LOGW("wlshim: memfd_create failed");
                return false;
        }
        if (ftruncate(win->pool_fd, (off_t)win->pool_size) < 0) {
                close(win->pool_fd);
                win->pool_fd = -1;
                return false;
        }
        win->pool_mem = mmap(NULL, win->pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                             win->pool_fd, 0);
        if (win->pool_mem == MAP_FAILED) {
                win->pool_mem = NULL;
                close(win->pool_fd);
                win->pool_fd = -1;
                return false;
        }

        /* wl_shm.create_pool — the 'h' (fd) arg rides the control channel:
         * libwayland collects it from the vararg list by signature. */
        win->pool = S.proxy_marshal_flags(S.shm, 0, S.wl_shm_pool_iface, 1, 0,
                                          NULL, win->pool_fd, (i32)win->pool_size);
        if (!win->pool) {
                shim_pool_destroy(win);
                return false;
        }
        for (int b = 0; b < 2; b++) {
                win->bufs[b] = S.proxy_marshal_flags(win->pool, 0, S.wl_buffer_iface, 1, 0,
                                                     NULL, (i32)((size_t)w * h * 4 * b),
                                                     (i32)w, (i32)h, (i32)(w * 4), 0u);
                if (!win->bufs[b]) {
                        shim_pool_destroy(win);
                        return false;
                }
                static void (*const buffer_listener[])(void) = {
                        (void (*)(void))on_buffer_release,
                };
                S.proxy_add_listener(win->bufs[b], (void (**)(void))buffer_listener, win);
                win->buf_free[b] = true;
        }
        win->pw = w;
        win->ph = h;
        return true;
}

bool wlshim_present_cpu(int win, const u32 *rgba, u32 w, u32 h)
{
        if (!S.active || !S.display || win < 0 || win >= S.nwins)
                return false;
        ShimWin *winp = &S.wins[win];
        if (winp->closed || !rgba)
                return false;

        /* (re)create the pool when this frame's size changes (output scale
         * changes recreate the window buffers) */
        if (!winp->pool || winp->pw != w || winp->ph != h) {
                if (!shim_pool_create(winp, w, h))
                        return false;
        }

        /* collect wl_buffer.release events — non-blocking read + dispatch,
         * same discipline as wlshim_pump() */
        {
                struct pollfd p = { S.get_fd(S.display), POLLIN, 0 };
                if (poll(&p, 1, 0) > 0) {
                        if (S.dispatch(S.display) < 0)
                                return false;    /* connection error */
                } else {
                        S.dispatch_pending(S.display);
                }
        }

        int b = -1;
        for (int i = 0; i < 2; i++)
                if (winp->buf_free[i]) {
                        b = i;
                        break;
                }
        if (b < 0)
                return true;    /* compositor still holds both slots — drop frame */

        /* premultiply into the pool slot (ARGB8888 is premultiplied alpha) */
        u32 *dst = (u32 *)((u8 *)winp->pool_mem + (size_t)w * h * 4 * b);
        size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; i++) {
                u32 p = rgba[i];
                u32 a = p >> 24;
                if (a == 255) {
                        dst[i] = p;
                } else if (a == 0) {
                        dst[i] = 0;
                } else {
                        u32 r = (p & 0xff), g = (p >> 8) & 0xff, bl = (p >> 16) & 0xff;
                        dst[i] = (a << 24) |
                                 (((bl * a + 127) / 255) << 16) |
                                 (((g * a + 127) / 255) << 8) |
                                 ((r * a + 127) / 255);
                }
        }

        int mon = winp->out >= 0 ? winp->out : 0;
        i32 scale = S.outputs[mon].scale < 1 ? 1 : S.outputs[mon].scale;
        S.proxy_marshal(winp->surface, 1, winp->bufs[b], 0, 0);    /* attach */
        if (scale > 1 && S.comp_version >= 3)
                S.proxy_marshal(winp->surface, 8, scale);            /* set_buffer_scale */
        S.proxy_marshal(winp->surface, S.comp_version >= 4 ? 9 : 2,
                        0, 0, w, h);                                 /* damage(_buffer) */
        S.proxy_marshal(winp->surface, WL_SURFACE_COMMIT);
        if (S.flush(S.display) < 0)
                return false;
        winp->buf_free[b] = false;
        winp->cur = b;

        if (++S.presents == 1)
                LOGI("wlshim: wl_shm present active (%ux%u, scale %d, double-buffered)",
                     w, h, scale);
        else if (S.presents == 1000)
                LOGD("wlshim: 1000 frames presented");
        return true;
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
