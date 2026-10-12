/* ============================================================================
 * Reverie Engine — platform.h
 * Display-server backend abstraction. Two implementations exist:
 *   - plat_x11.c     raw X11 wire protocol (x11.c) — EWMH desktop + MIT-SHM
 *   - plat_wayland.c raw Wayland wire protocol (wayland.c) — layer-shell + wl_shm
 * Neither links Xlib, XCB, or libwayland-client.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_PLATFORM_H
#define REV_PLATFORM_H

#include "rev_types.h"

#define REV_PLATFORM_MAX_OUTPUTS 8

typedef struct RPlatform RPlatform;

/* Aggregated pointer state (global/compositor coordinates, X11 root-style),
 * updated by pump(). dx/dy are deltas since the previous pump. */
typedef struct {
        bool valid;      /* pointer position known                    */
        bool pressed;    /* any button held                           */
        f64 x, y;        /* global coordinates                        */
        f64 dx, dy;      /* delta since last pump                     */
} RPointerState;

struct RPlatform {
        const char *name;          /* "x11" | "wayland"                 */
        void *ctx;                 /* implementation-private state       */

        /* ---- lifecycle ---- */
        bool (*init)(RPlatform *p, char *err, size_t errcap);
        void (*fini)(RPlatform *p);

        /* ---- outputs ---- */
        int  (*monitors)(RPlatform *p, RRect *out, int max);
        /* Per-output scale factor (Wayland fractional/hidpi; X11 reports 1). */
        f64  (*monitor_scale)(RPlatform *p, int mon);

        /* ---- windows ---- */
        int  (*add_window)(RPlatform *p, RRect geom);
        bool (*present_cpu)(RPlatform *p, int win, const u32 *rgba, u32 w, u32 h);

        /* ---- GPU surface support ----
         * has_gpu:   accelerated present possible on this platform
         *            (X11: always true — EGL needs only the XID;
         *             Wayland: only when the WSI shim is active)
         * native:    X11 -> XID; Wayland -> wl_surface* from the shim, else 0
         * display:   Wayland+shim -> wl_display* (for eglGetDisplay);
         *            X11 -> NULL (EGL default display is used) */
        bool has_gpu;
        u64  (*native_handle)(RPlatform *p, int win);
        void *(*native_display)(RPlatform *p);

        /* geometry changed: recreate the window in place (same index) so
         * renderers can rebuild their surfaces */
        bool (*resize_window)(RPlatform *p, int win, RRect geom);

        /* ---- events / state ---- */
        void (*pump)(RPlatform *p);
        bool (*pointer)(RPlatform *p, RPointerState *out);
        bool (*win_geometry)(RPlatform *p, int win, RRect *geom);
        void (*sync)(RPlatform *p);
        /* True when the compositor/server asked to close (Wayland "closed"). */
        bool (*should_close)(RPlatform *p);

        /* ---- tests / tools ---- */
        u32 *(*screenshot)(RPlatform *p, int win, u32 *w, u32 *h);
};

/* pref: "auto" | "x11" | "wayland".
 * auto: WAYLAND_DISPLAY set -> wayland; else DISPLAY set -> x11;
 *       else try wayland socket, then X. */
RPlatform *platform_create(const char *pref, char *err, size_t errcap);
void platform_destroy(RPlatform *p);

#endif /* REV_PLATFORM_H */
