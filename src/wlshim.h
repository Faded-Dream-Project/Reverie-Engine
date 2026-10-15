/* ============================================================================
 * Reverie Engine — wlshim.h
 * Wayland WSI shim: drives a REAL libwayland-client connection (dlopen'd,
 * never linked) so that EGL/Vulkan can obtain wl_display / wl_surface
 * handles for accelerated presentation on Wayland.
 *
 * ACTIVE BY DEFAULT (a present GPU should be used): the shim engages
 * whenever libwayland-client.so.0 loads. REV_WAYLAND_GPU=0 opts out for
 * the fully hand-written raw protocol client. If the shim cannot
 * initialize (no layer-shell on the compositor), the platform falls back
 * to the raw client and the renderer chain to the CPU tier. All functions
 * below are safe to call when the shim is off (they report "inactive" /
 * fail gracefully).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_WLSHIM_H
#define REV_WLSHIM_H

#include "platform.h"
#include "rev_types.h"

/* True when the shim is engaged (default) and libwayland-client loaded. */
bool wlshim_active(void);

/* ---- platform-level operations (delegated from plat_wayland.c) ---- */
bool wlshim_platform_init(RPlatform *p, char *err, size_t errcap);
void wlshim_platform_fini(RPlatform *p);
int  wlshim_monitors(RRect *out, int max);
f64  wlshim_monitor_scale(int mon);
int  wlshim_monitor_refresh(int mon);   /* Hz, 0 unknown */
int  wlshim_add_window(RRect geom);
/* Present one CPU-readable frame through wl_shm. `premultiplied`: the bytes
 * are already premultiplied against alpha (the Vulkan tier converts on the
 * GPU) — the presenter then does a single memcpy; false = straight alpha and
 * the presenter premultiplies per pixel itself (CPU tier path). */
bool wlshim_present_cpu(int win, const u32 *rgba, u32 w, u32 h, bool premultiplied);
void wlshim_pump(RPointerState *ptr);
bool wlshim_pointer(RPointerState *out);
bool wlshim_wait_frame(f64 seconds);
u64  wlshim_dropped_frames(void);
bool wlshim_win_geometry(int win, RRect *geom);
void wlshim_sync(void);
bool wlshim_should_close(void);

/* Idle power-save: false when every wl_shm-presented window has gone
 * unlatched past the visibility window (fullscreen app over the wallpaper,
 * display asleep) — the caller should stop rendering and pace at a slow
 * probe rate; the shim keeps asking the compositor ~4 Hz and flips back to
 * true on the first latched callback. Surfaces owned by EGL / a Vulkan
 * swapchain are excluded (they throttle through their own present calls). */
bool wlshim_surface_visible(void);
/* Test/field hook: override the starvation window in seconds (<0 restores
 * the env/default 0.25). */
void wlshim_set_vis_timeout(f64 seconds);

/* Marks a window's wl_surface as owned by EGL (render_gl.c calls this once
 * its wl_egl_window is active). The wl_shm presenter then stays off that
 * surface so it cannot fight the EGL swapchain for commits. */
void wlshim_set_gl_surface(int win, bool on);

/* ---- native handles for EGL/Vulkan ---- */
u64   wlshim_surface_handle(int win);    /* wl_surface* */
void *wlshim_display_handle(void);       /* wl_display*  */

#endif /* REV_WLSHIM_H */
