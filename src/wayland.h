/* ============================================================================
 * Reverie Engine — wayland.h
 * A raw Wayland wire-protocol client speaking directly to the compositor
 * over a unix socket. Replaces libwayland-client entirely. Implements:
 * connection setup, registry + global binding, monitor enumeration via
 * wl_output, zwlr_layer_shell_v1 desktop surfaces, double-buffered wl_shm
 * ARGB8888 presentation with premultiplication, and pointer events via
 * wl_seat/wl_pointer.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_WAYLAND_H
#define REV_WAYLAND_H

#include "rev_types.h"

typedef struct WLDisplay WLDisplay;

/* Connects via $WAYLAND_DISPLAY (default "wayland-0") in $XDG_RUNTIME_DIR.
 * Fails (NULL + message) when the socket is missing, the handshake fails,
 * or the compositor lacks zwlr_layer_shell_v1. */
WLDisplay *wl_connect(char *err, size_t errcap);
void wl_disconnect(WLDisplay *w);

/* Outputs (logical coordinates; pixel size = logical * scale). */
int  wl_monitors(WLDisplay *w, RRect *out, int max);
f64  wl_monitor_scale(WLDisplay *w, int mon);

/* Creates a layer-shell background surface on monitor `mon`. */
int  wl_add_window(WLDisplay *w, int mon);

/* Presents a straight-alpha RGBA8 buffer of exactly (w x h) pixels
 * (premultiplied internally into the wl_shm buffer). */
bool wl_present(WLDisplay *w, int win, const u32 *rgba, u32 width, u32 height);

/* Dispatches pending events (non-blocking). */
void wl_pump(WLDisplay *w);

/* Global pointer state (logical coordinates). */
bool wl_pointer(WLDisplay *w, f64 *x, f64 *y, f64 *dx, f64 *dy, bool *pressed);

/* Window geometry (logical). */
bool wl_win_geometry(WLDisplay *w, int win, RRect *geom);

/* Round-trip sync. */
void wl_sync(WLDisplay *w);

/* The compositor closed a layer surface (e.g. output unplug). */
bool wl_should_close(WLDisplay *w);

/* True when the connection hit a protocol error. */
bool wl_fatal(WLDisplay *w);

#endif /* REV_WAYLAND_H */
