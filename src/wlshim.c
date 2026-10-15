#define _GNU_SOURCE
/* ============================================================================
 * Reverie Engine — wlshim.c
 * Wayland WSI shim (ACTIVE BY DEFAULT; REV_WAYLAND_GPU=0 opts out).
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
#include "util.h"

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
        /* request opcode order per wlr-layer-shell-unstable-v1.xml (the
         * canonical upstream protocol sway/wlroots/KWin implement):
         *   0 set_size(uu)                1 set_anchor(u)
         *   2 set_exclusive_zone(i)       3 set_margin(iiii)
         *   4 set_keyboard_interactivity(u)
         *   5 get_popup(o)                6 ack_configure(u)
         *   7 destroy                     8 set_layer(u) since v2
         *   9 set_exclusive_edge(u) since v5 (never sent — we bind v4)
         * set_size(0,0) + all-edges anchor is the canonical wallpaper
         * pattern: 0 means "the anchors determine the size", the
         * compositor's assignment arrives via configure(). The 0.1.5 table
         * believed set_size did not exist and dropped it — libwayland then
         * marshalled the anchor value as a 4-byte opcode-0 frame which
         * strict compositors reject ("invalid arguments for set_size")
         * because set_size(uu) needs 8 payload bytes. */
        { "set_size",                  "uu",   NULL },
        { "set_anchor",                 "u",    NULL },
        { "set_exclusive_zone",         "i",    NULL },
        { "set_margin",                 "iiii", NULL },
        { "set_keyboard_interactivity", "u",    NULL },
        { "get_popup",                  "o",    NULL },
        { "ack_configure",              "u",    NULL },
        { "destroy",                    "",     NULL },
        { "set_layer",                  "u",    NULL },
        { "set_exclusive_edge",         "u",    NULL },
};
static const struct wl_message_ layer_surface_events[] = {
        { "configure", "uuu", NULL },
        { "closed",    "",    NULL },
};

static const struct wl_interface_ zwlr_layer_shell_v1_i = {
        "zwlr_layer_shell_v1", 4, 2, layer_shell_methods, 0, NULL
};
static const struct wl_interface_ zwlr_layer_surface_v1_i = {
        "zwlr_layer_surface_v1", 4, 10, layer_surface_methods, 2, layer_surface_events
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
        /* dev: frame pacing */
        wl_proxy_ *frame_cb;   /* pending wl_surface.frame callback       */
        bool frame_ready;      /* callback fired since last wait          */
        bool gl_surface;       /* EGL owns this surface: the wl_shm
                                * presenter must NOT touch it (the GL
                                * renderer presents via eglSwapBuffers)    */
        /* visibility (idle power-save): a wl_shm-presented window is
         * "visible" while the compositor keeps latching its frames; the
         * callback timestamps are the evidence. gl_surface / never-presented
         * windows (WSI swapchains, EGL) are excluded — EXCEPT WSI-owned
         * windows, which carry their own frame-request evidence through
         * wlshim_wsi_frame_request(). */
        bool presented;        /* ever presented through wl_shm            */
        f64 last_done;         /* last wl_callback.done (0 = never)        */
        f64 last_present;      /* last wl_shm commit                       */
        f64 last_probe;        /* last idle re-present probe               */
        bool wsi;              /* WSI swapchain owns this surface: the
                                 * Vulkan renderer requested frame
                                 * callbacks on it (wsi_frame_request)   */
        f64 wsi_since;         /* first frame request — grace window       */
} ShimWin;

typedef struct {
        u32 name;
        char iface[64];
        u32 version;
} ShimGlobal;

typedef struct {
        wl_proxy_ *output;
        i32 lx, ly, pw, ph, scale;
        i32 refresh_mhz;   /* mode refresh (mHz; 0 = unreported) */
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
        const struct wl_interface_ *wl_callback_iface;

        ShimGlobal *globals;   /* grown on demand — compositors advertise 50+ */
        int nglobals, globcap;
        ShimOutput outputs[8];
        int noutputs;
        ShimWin wins[8];
        int nwins;

        f64 px, py, pdx, pdy;
        bool pvalid, ppressed, plasted;
        bool press_edge, release_edge, click, click_pending;
        f64 click_x, click_y;
        f64 click_q[4][2];   /* latched clicks between reads (oldest first) */
        u32 click_qn;
        u32 clicks;
        int ptr_win;
        u32 seat_caps;
        bool close_requested;
        u64 presents;        /* frames delivered through wl_shm (log milestone) */
        u64 drops;           /* presents dropped (both slots held)             */

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
                        if (!S.plasted) {
                                S.pdx = 0;
                                S.pdy = 0;
                        }
                        /* enter position is a valid delta baseline (the jump
                         * TO it is not a drag and never counts) */
                        S.plasted = true;
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
                /* accumulate the whole drag between reads (was: last-event
                 * delta only — multi-event drags lost most of their motion) */
                S.pdx += nx - S.px;
                S.pdy += ny - S.py;
        }
        S.px = nx;
        S.py = ny;
        S.plasted = true;
}

static void on_ptr_button(void *data, wl_proxy_ *ptr, u32 serial, u32 time, u32 button, u32 state)
{
        (void)data; (void)ptr; (void)serial; (void)time; (void)button;
        if (state == 1) {
                S.ppressed = true;
                S.press_edge = true;
                S.click_x = S.px;   /* position at press */
                S.click_y = S.py;
                S.click_pending = true;
        } else {
                S.ppressed = false;
                S.release_edge = true;
                if (S.click_pending) {
                        S.click = true;
                        S.clicks++;
                        /* latch position into the queue: each completed
                         * press+release keeps its own spot (a same-poll
                         * double-click used to collapse onto the last one) */
                        if (S.click_qn < 4) {
                                S.click_q[S.click_qn][0] = S.click_x;
                                S.click_q[S.click_qn][1] = S.click_y;
                                S.click_qn++;
                        } else {
                                for (u32 c = 0; c < 3; c++) {
                                        S.click_q[c][0] = S.click_q[c + 1][0];
                                        S.click_q[c][1] = S.click_q[c + 1][1];
                                }
                                S.click_q[3][0] = S.click_x;
                                S.click_q[3][1] = S.click_y;
                        }
                }
                S.click_pending = false;
        }
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
        (void)data;
        if (!(flags & 1))
                return;
        for (int i = 0; i < S.noutputs; i++)
                if (S.outputs[i].output == out) {
                        S.outputs[i].pw = w;
                        S.outputs[i].ph = h;
                        S.outputs[i].refresh_mhz = refresh;
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

/* wl_callback.done — the compositor latched the frame this callback was
 * requested for. data = the ShimWin that presented. */
static void on_frame_done(void *data, wl_proxy_ *cb, u32 time)
{
        (void)cb; (void)time;
        ShimWin *win = (ShimWin *)data;
        if (win) {
                win->frame_ready = true;
                win->last_done = rev_monotonic();
        }
}

/* ---- activation ------------------------------------------------------------- */

static void shim_pool_destroy(ShimWin *win);    /* fwd: fini tears pools down */

bool wlshim_active(void)
{
        if (!S.tried) {
                S.tried = true;
                /* Default: ON. A GPU that is present should be USED — the
                 * maintainer's field reports were exactly "GPU available,
                 * engine still doing the rendering on the CPU". The shim is
                 * attempted whenever libwayland-client loads; the renderer
                 * chain still falls back to the CPU path when EGL fails.
                 * REV_WAYLAND_GPU=0 is the explicit opt-out for the raw
                 * hand-written client. */
                const char *env = getenv("REV_WAYLAND_GPU");
                if (env && strcmp(env, "0") == 0)
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

static bool wlshim_init_inner(RPlatform *p, char *err, size_t errcap)
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
        *(void **)&S.wl_callback_iface = dlsym(S.lib, "wl_callback_interface");
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

bool wlshim_platform_init(RPlatform *p, char *err, size_t errcap)
{
        if (!S.active) {
                snprintf(err, errcap, "wlshim not active");
                return false;
        }
        if (wlshim_init_inner(p, err, errcap))
                return true;
        /* failed half-way (e.g. compositor without layer-shell): tear the
         * connection down and deactivate so the platform falls back to the
         * raw client instead of calling into a half-initialized shim. */
        wlshim_platform_fini(p);
        return false;
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
                                if (S.wins[i].frame_cb) {
                                        S.proxy_destroy(S.wins[i].frame_cb);
                                        S.wins[i].frame_cb = NULL;
                                }
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

        /* REAL request numbering per wlr-layer-shell-unstable-v1.xml:
         * set_size(0,0) opcode 0 (uu — 8 payload bytes; 0 means the anchors
         * determine the size), set_anchor(all) opcode 1,
         * set_exclusive_zone(-1) opcode 2, set_keyboard_interactivity(none)
         * opcode 4 — the compositor's size assignment arrives via
         * configure(). */
        S.proxy_marshal(win->layer_surface, 0, 0u, 0u);
        S.proxy_marshal(win->layer_surface, 1, 15u);
        S.proxy_marshal(win->layer_surface, 2, (i32)-1);
        S.proxy_marshal(win->layer_surface, 4, 0u);
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
        S.proxy_marshal(win->layer_surface, 6, win->cfg_serial);    /* ack_configure — opcode 6 (5 is get_popup) */
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

bool wlshim_present_cpu(int win, const u32 *rgba, u32 w, u32 h, bool premultiplied)
{
        if (!S.active || !S.display || win < 0 || win >= S.nwins)
                return false;
        ShimWin *winp = &S.wins[win];
        if (winp->closed || !rgba)
                return false;
        if (winp->gl_surface) {
                /* EGL owns this surface now (the GL renderer presents via
                 * eglSwapBuffers); attaching wl_shm buffers would fight the
                 * EGL swapchain for the same wl_surface. */
                static bool warned;
                if (!warned) {
                        warned = true;
                        LOGW("wlshim: window %d presented by EGL — wl_shm path idle", win);
                }
                return true;
        }

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
        if (b < 0) {
                S.drops++;
                return true;    /* compositor still holds both slots — drop frame */
        }

        /* fill the pool slot. premultiplied input (the Vulkan tier converts
         * + swizzles on the GPU): one verbatim memcpy — the entire CPU cost
         * of an SHM present, ~1ms at 1080p, and no per-pixel loop over GPU
         * memory (reading an uncached staging heap per pixel was field
         * report #4: 1.1 s/frame on a GTX 1050 Ti). straight input (CPU
         * tier): the scalar premultiply below — the source is plain RAM,
         * ~5ms.
         *
         * BYTE ORDER (field report #5): wl_shm ARGB8888 packs the u32 as
         * 0xAARRGGBB — memory bytes B,G,R,A — while the engine's native
         * u32 is 0xAABBGGRR (memory R,G,B,A). The loop therefore swaps R
         * and B while it premultiplies; skipping the swap made every blue
         * render red (the X11 path never had this: x11.c repacks into the
         * visual's masks). */
        u32 *dst = (u32 *)((u8 *)winp->pool_mem + (size_t)w * h * 4 * b);
        size_t n = (size_t)w * h;
        if (premultiplied) {
                memcpy(dst, rgba, n * 4);
        } else {
                for (size_t i = 0; i < n; i++) {
                        u32 p = rgba[i];
                        u32 a = p >> 24;
                        if (a == 255) {
                                /* opaque: just exchange the R and B bytes
                                 * (A and G keep their positions) */
                                dst[i] = (p & 0xFF00FF00u) |
                                         ((p & 0x00FF0000u) >> 16) |
                                         ((p & 0x000000FFu) << 16);
                        } else if (a == 0) {
                                dst[i] = 0;
                        } else {
                                u32 r = (p & 0xff), g = (p >> 8) & 0xff, bl = (p >> 16) & 0xff;
                                dst[i] = (a << 24) |
                                         (((r * a + 127) / 255) << 16) |
                                         (((g * a + 127) / 255) << 8) |
                                         ((bl * a + 127) / 255);
                        }
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

        /* wl_surface.frame (opcode 3) — the pacing loop sleeps on this
         * callback; the compositor fires it when the frame is latched. */
        if (!winp->frame_cb && S.wl_callback_iface) {
                winp->frame_cb = S.proxy_marshal_constructor(winp->surface, 3,
                                                             S.wl_callback_iface, NULL);
                if (winp->frame_cb) {
                        static void (*const cb_listener[])(void) = {
                                (void (*)(void))on_frame_done,
                        };
                        S.proxy_add_listener(winp->frame_cb,
                                             (void (**)(void))cb_listener, winp);
                }
        }

        if (S.flush(S.display) < 0)
                return false;
        winp->buf_free[b] = false;
        winp->cur = b;
        winp->presented = true;
        winp->last_present = rev_monotonic();

        if (++S.presents == 1)
                LOGI("wlshim: wl_shm present active (%ux%u, scale %d, double-buffered, "
                     "%s)", w, h, scale,
                     premultiplied ? "GPU-premul memcpy" : "CPU premultiply");
        else if (S.presents == 1000)
                LOGD("wlshim: 1000 frames presented");
        return true;
}

/* ---- visibility / idle power-save ------------------------------------------ */

/* A visible surface gets wl_callback.done at the compositor's repaint rate.
 * A gap far past that means the surface is not being drawn at all — most
 * often a fullscreen app covering the wallpaper, or the display asleep —
 * and the engine can stop rendering until asked again. The threshold is
 * deliberately generous (a lazy compositor legitimately drops to a few Hz
 * when nothing changes); REV_WL_VIS_MS tunes it (tests shrink it). */
static f64 vis_override = -1;   /* test hook: <0 = auto (env / default) */

static f64 wl_vis_timeout(void)
{
        if (vis_override >= 0)
                return vis_override;
        static f64 cached = -1;
        if (cached < 0) {
                const char *e = getenv("REV_WL_VIS_MS");
                cached = (e && *e) ? atof(e) / 1000.0 : 0.25;
                if (cached < 0.02)
                        cached = 0.02;
        }
        return cached;
}

/* Test/field hook: override the starvation window (seconds; <0 restores the
 * env/default). Tests shrink it so the idle machinery verifies quickly. */
void wlshim_set_vis_timeout(f64 seconds)
{
        vis_override = seconds < 0 ? -1 : (seconds < 0.001 ? 0.001 : seconds);
}

/* True when at least one wl_shm-presented or WSI-owned window was latched
 * recently (or no window is ours at all — startup, raw surfaces — in which
 * case visibility is not ours to judge and rendering must continue).
 *
 * WSI-owned windows: the ONLY evidence is wl_callback.done (the shim's own
 * frame requests committed by the renderer's swapchain presents). Present
 * timestamps deliberately do NOT refresh a WSI window — backpressure (the
 * compositor releasing images while never latching, an acquire that stalls)
 * must not masquerade as visibility, so a WSI surface that stops getting
 * callbacks goes hidden even mid-presentation. The grace window after the
 * first frame request (2x the visibility window) bridges the startup gap
 * where no callback can have fired yet. */
bool wlshim_surface_visible(void)
{
        if (!S.active)
                return true;
        f64 now = rev_monotonic();
        f64 vis = wl_vis_timeout();
        bool any_ours = false;
        for (int i = 0; i < S.nwins; i++) {
                ShimWin *w = &S.wins[i];
                if (w->wsi) {
                        any_ours = true;
                        if (now - w->wsi_since < 2.0 * vis)
                                return true;    /* grace: no callback yet possible */
                        if (w->last_done > 0 && now - w->last_done < vis)
                                return true;    /* latched recently */
                        continue;
                }
                if (w->gl_surface || !w->presented)
                        continue;    /* EGL swapchain paces itself */
                any_ours = true;
                f64 last = w->last_done > w->last_present ? w->last_done
                                                          : w->last_present;
                if (now - last < vis)
                        return true;
        }
        return !any_ours;
}

/* ---- WSI-present visibility --------------------------------------------- */

/* One frame callback, one at a time — mirrors the wl_shm presenter's
 * pattern (present_cpu / shim_probe): the slot frees when wait_frame()
 * destroys the fired callback, and the next request re-arms it. */
static void shim_request_frame(ShimWin *w)
{
        if (w->frame_cb || !S.wl_callback_iface)
                return;
        w->frame_cb = S.proxy_marshal_constructor(w->surface, 3,
                                                  S.wl_callback_iface, NULL);
        if (!w->frame_cb)
                return;
        static void (*const cb_listener[])(void) = {
                (void (*)(void))on_frame_done,
        };
        S.proxy_add_listener(w->frame_cb,
                             (void (**)(void))cb_listener, w);
}

bool wlshim_wsi_frame_request(int win)
{
        if (!S.active || win < 0 || win >= S.nwins)
                return false;
        ShimWin *w = &S.wins[win];
        if (w->gl_surface || !w->surface)
                return false;    /* not a WSI-presentable surface */
        if (!w->wsi) {
                w->wsi = true;
                w->wsi_since = rev_monotonic();
                LOGD("wlshim: window %d now WSI-owned (frame-request "
                     "visibility armed)", win);
        }
        shim_request_frame(w);
        S.flush(S.display);
        return true;
}

bool wlshim_hidden_render_probe(void)
{
        if (!S.active)
                return false;
        for (int i = 0; i < S.nwins; i++)
                if (S.wins[i].wsi)
                        return true;
        return false;
}

bool wlshim_wsi_test_commit(int win)
{
        if (!S.active || win < 0 || win >= S.nwins)
                return false;
        ShimWin *w = &S.wins[win];
        if (!w->wsi || !w->surface)
                return false;
        S.proxy_marshal(w->surface, WL_SURFACE_COMMIT);
        return S.flush(S.display) >= 0;
}

/* Idle probe: re-attach the CURRENT buffer with full damage + commit. A
 * bare frame request with no commit may never be answered, and a commit
 * with no damage lets lazy compositors skip the repaint — and the callback
 * — entirely; redrawing the same pixels is the cheapest unambiguous "are
 * you still there?". Hidden: the compositor ignores it (cost: nothing).
 * Visible again: the next latch fires the pending frame callback, the
 * visibility check flips, and full-rate rendering resumes. */
static void shim_probe(ShimWin *w)
{
        if (w->cur < 0 || !w->bufs[w->cur] || !w->pool)
                return;
        S.proxy_marshal(w->surface, 1, w->bufs[w->cur], 0, 0);          /* attach */
        S.proxy_marshal(w->surface, S.comp_version >= 4 ? 9 : 2,
                        0, 0, (i32)w->pw, (i32)w->ph);                  /* damage */
        if (!w->frame_cb && S.wl_callback_iface) {
                w->frame_cb = S.proxy_marshal_constructor(w->surface, 3,
                                                          S.wl_callback_iface, NULL);
                if (w->frame_cb) {
                        static void (*const cb_listener[])(void) = {
                                (void (*)(void))on_frame_done,
                        };
                        S.proxy_add_listener(w->frame_cb,
                                             (void (**)(void))cb_listener, w);
                }
        }
        S.proxy_marshal(w->surface, WL_SURFACE_COMMIT);
        S.flush(S.display);
}

/* Probes are throttled to ~4 Hz and only sent for starved windows. */
#define WL_PROBE_INTERVAL 0.25

static void shim_idle_probes(void)
{
        f64 now = rev_monotonic();
        f64 vis = wl_vis_timeout();
        for (int i = 0; i < S.nwins; i++) {
                ShimWin *w = &S.wins[i];
                if (w->gl_surface || !w->presented)
                        continue;
                f64 last = w->last_done > w->last_present ? w->last_done
                                                          : w->last_present;
                if (now - last >= vis && now - w->last_probe >= WL_PROBE_INTERVAL) {
                        w->last_probe = now;
                        shim_probe(w);
                }
        }
}

void wlshim_pump(RPointerState *ptr)
{
        (void)ptr;    /* pointer state is read (and consumed) via wlshim_pointer */
        if (!S.active)
                return;
        struct pollfd p = { S.get_fd(S.display), POLLIN, 0 };
        if (poll(&p, 1, 0) > 0)
                S.dispatch(S.display);
        else
                S.dispatch_pending(S.display);
        shim_idle_probes();
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
        out->press_edge = S.press_edge;
        out->release_edge = S.release_edge;
        out->click = S.click;
        out->click_x = S.click_x;
        out->click_y = S.click_y;
        out->click_qn = S.click_qn;
        for (u32 c = 0; c < S.click_qn && c < 4; c++) {
                out->click_q[c][0] = S.click_q[c][0];
                out->click_q[c][1] = S.click_q[c][1];
        }
        out->clicks = S.clicks;
        /* consume-on-read — deltas/edges are delivered exactly once */
        S.pdx = 0;
        S.pdy = 0;
        S.press_edge = false;
        S.release_edge = false;
        S.click = false;
        S.click_qn = 0;
        return true;
}

bool wlshim_wait_frame(f64 seconds)
{
        if (!S.active || !S.display)
                return false;
        if (seconds < 0)
                seconds = 0;
        f64 deadline = rev_monotonic() + seconds;
        /* starved = no window was latched within the visibility window: the
         * frame callbacks we are waiting for will not come at repaint rate,
         * so there is nothing to gain from 16ms poll slices — sleep coarser
         * (the idle probe in pump() still runs ~4 Hz, and input arriving on
         * the socket still wakes poll() immediately). */
        const bool starved = !wlshim_surface_visible();
        const int slice_cap = starved ? 100 : 16;

        for (;;) {
                struct pollfd p = { S.get_fd(S.display), POLLIN, 0 };
                if (poll(&p, 1, 0) > 0)
                        S.dispatch(S.display);
                else
                        S.dispatch_pending(S.display);

                bool any = false;
                for (int i = 0; i < S.nwins; i++) {
                        if (S.wins[i].frame_cb && S.wins[i].frame_ready) {
                                if (S.proxy_destroy)
                                        S.proxy_destroy(S.wins[i].frame_cb);
                                S.wins[i].frame_cb = NULL;
                                S.wins[i].frame_ready = false;
                                any = true;
                        }
                }
                if (any)
                        return true;

                f64 rem = deadline - rev_monotonic();
                if (rem <= 0)
                        return false;
                int ms = (int)(rem * 1000.0);
                if (ms < 1)
                        ms = 1;
                if (ms > slice_cap)
                        ms = slice_cap;
                poll(&p, 1, ms);
        }
}

u64 wlshim_dropped_frames(void)
{
        return S.drops;
}

int wlshim_monitor_refresh(int mon)
{
        if (mon < 0 || mon >= S.noutputs)
                return 0;
        i32 mhz = S.outputs[mon].refresh_mhz;
        if (mhz < 10000 || mhz > 1000000)
                return 0;
        return (int)((mhz + 500) / 1000);
}

void wlshim_set_gl_surface(int win, bool on)
{
        if (win < 0 || win >= S.nwins)
                return;
        S.wins[win].gl_surface = on;
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
