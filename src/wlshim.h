/* ============================================================================
 * Reverie Engine — wlshim.h
 * Optional Wayland WSI shim: drives a REAL libwayland-client connection
 * (dlopen'd, never linked) so that EGL/Vulkan can obtain wl_display /
 * wl_surface handles for accelerated presentation on Wayland.
 *
 * EXPERIMENTAL and opt-in: activated only when REV_WAYLAND_GPU=1 is set AND
 * libwayland-client.so.0 can be dlopened. Without it the engine runs the
 * fully hand-written raw protocol client with the CPU compositor — which
 * always works. All functions below are safe to call when the shim is off
 * (they report "inactive" / fail gracefully).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_WLSHIM_H
#define REV_WLSHIM_H

#include "platform.h"
#include "rev_types.h"

/* True when REV_WAYLAND_GPU=1 and libwayland-client is loadable. */
bool wlshim_active(void);

/* ---- platform-level operations (delegated from plat_wayland.c) ---- */
bool wlshim_platform_init(RPlatform *p, char *err, size_t errcap);
void wlshim_platform_fini(RPlatform *p);
int  wlshim_monitors(RRect *out, int max);
f64  wlshim_monitor_scale(int mon);
int  wlshim_monitor_refresh(int mon);   /* Hz, 0 unknown */
int  wlshim_add_window(RRect geom);
bool wlshim_present_cpu(int win, const u32 *rgba, u32 w, u32 h);  /* unused in GPU mode */
void wlshim_pump(RPointerState *ptr);
bool wlshim_pointer(RPointerState *out);
bool wlshim_wait_frame(f64 seconds);
u64  wlshim_dropped_frames(void);
bool wlshim_win_geometry(int win, RRect *geom);
void wlshim_sync(void);
bool wlshim_should_close(void);

/* Marks a window's wl_surface as owned by EGL (render_gl.c calls this once
 * its wl_egl_window is active). The wl_shm presenter then stays off that
 * surface so it cannot fight the EGL swapchain for commits. */
void wlshim_set_gl_surface(int win, bool on);

/* ---- native handles for EGL/Vulkan ---- */
u64   wlshim_surface_handle(int win);    /* wl_surface* */
void *wlshim_display_handle(void);       /* wl_display*  */

#endif /* REV_WLSHIM_H */
