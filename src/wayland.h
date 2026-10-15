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

#include "platform.h"
#include "rev_types.h"

typedef struct WLDisplay WLDisplay;

/* Connects via $WAYLAND_DISPLAY (default "wayland-0") in $XDG_RUNTIME_DIR.
 * Fails (NULL + message) when the socket is missing, the handshake fails,
 * or the compositor lacks zwlr_layer_shell_v1. */
WLDisplay *wl_connect(char *err, size_t errcap);
void wl_disconnect(WLDisplay *w);

/* Outputs (logical coordinates; pixel size = logical * scale). REMOVED
 * outputs are filtered out, so the list is packed: monitor indices passed
 * to wl_monitor_scale/refresh/id and wl_add_window refer to the PACKED list
 * (n-th output still present), not internal slots. */
int  wl_monitors(WLDisplay *w, RRect *out, int max);
f64  wl_monitor_scale(WLDisplay *w, int mon);
/* Current mode's refresh rate in Hz (rounded); 0 when unreported. */
int  wl_monitor_refresh(WLDisplay *w, int mon);
/* Stable identity of the n-th present output: the wl_output global name
 * (compositor-assigned, unique for the output's lifetime). Used for
 * hotplug matching — the same physical output keeps its id. */
u64  wl_monitor_id(WLDisplay *w, int mon);

/* Creates a layer-shell background surface on monitor `mon`. Window
 * indices are SLOTS: a window removed with wl_remove_window frees its
 * slot and a later add reuses it (renderer per-window state relies on
 * this). */
int  wl_add_window(WLDisplay *w, int mon);
/* Destroys a window's layer surface + surface + shm pool, frees the slot.
 * Returns false for an unknown index. The caller must have released any
 * renderer-owned handle for the surface first. */
bool wl_remove_window(WLDisplay *w, int win);

/* Presents a straight-alpha RGBA8 buffer of exactly (w x h) pixels
 * (premultiplied internally into the wl_shm buffer). After the commit a
 * wl_surface.frame callback is requested; wl_wait_frame() sleeps on it. */
bool wl_present(WLDisplay *w, int win, const u32 *rgba, u32 width, u32 height);

/* Dispatches pending events (non-blocking) — also binds wl_output globals
 * that appear mid-session (hotplug) and rounds their geometry in. */
void wl_pump(WLDisplay *w);

/* Pointer state, consume-on-read: dx/dy and the button edge flags
 * (press_edge/release_edge/click, click_x/click_y) are reported once and
 * reset — a click that lived entirely between two polls still registers. */
bool wl_pointer(WLDisplay *w, RPointerState *out);

/* Frame pacing: blocks up to `seconds` while dispatching events, returning
 * early (true) once a frame callback fires. See platform.h wait_frame. */
bool wl_wait_frame(WLDisplay *w, f64 seconds);

/* Frames presented-but-dropped (compositor still held both shm slots). */
u64  wl_dropped_frames(WLDisplay *w);

/* Window geometry (logical). */
bool wl_win_geometry(WLDisplay *w, int win, RRect *geom);

/* Round-trip sync. */
void wl_sync(WLDisplay *w);

/* The compositor closed a layer surface (e.g. output unplug). */
bool wl_should_close(WLDisplay *w);

/* True when the connection hit a protocol error. */
bool wl_fatal(WLDisplay *w);

#endif /* REV_WAYLAND_H */
