/* ============================================================================
 * Reverie Engine — x11.h
 * A raw X11 wire-protocol client speaking directly to the X server over a
 * unix socket. Replaces Xlib/XCB entirely. Implements: connection setup
 * with Xauthority MIT-MAGIC-COOKIE-1 auth, visual selection, window
 * creation with EWMH desktop hints, MIT-SHM double-buffered presentation
 * (with core PutImage fallback), Xinerama monitor enumeration, pointer
 * polling, and event reading.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_X11_H
#define REV_X11_H

#include "rev_types.h"

#include "platform.h"

typedef struct X11 X11;

/* Connects to the display in $DISPLAY (unix socket preferred). */
X11 *x11_connect(char *err, size_t errcap);
void x11_disconnect(X11 *x);

/* Monitor list via Xinerama (falls back to the full root window). */
int  x11_monitors(X11 *x, RRect *out, int max);

/* Creates a desktop-type wallpaper window covering `geom`. Returns the
 * window index or -1. */
int  x11_add_window(X11 *x, RRect geom);

/* Presents an RGBA8 (straight alpha) buffer of exactly the window size. */
bool x11_present(X11 *x, int win, const u32 *rgba, u32 w, u32 h);

/* Polls the root pointer: root-relative coords + button mask. */
bool x11_query_pointer(X11 *x, i32 *rx, i32 *ry, u32 *mask);

/* Latched button edges since the last x11_pointer() call: press/release
 * transitions observed in the event stream, so a click whose press AND
 * release both happen between two polls still registers. */
bool x11_pointer(X11 *x, RPointerState *out);

/* Event-loop sleep: polls the X connection fd for up to `seconds` while
 * draining events — input keeps flowing between frames. */
void x11_wait_frame(X11 *x, f64 seconds);

/* Reads pending events, updating internal window geometry. */
void x11_pump_events(X11 *x);

/* True if a window's geometry changed since the last check. */
bool x11_win_geometry(X11 *x, int win, RRect *geom);

/* Round-trip sync (flushes the request pipe). */
void x11_sync(X11 *x);

/* Root geometry (screen size). */
void x11_root_size(X11 *x, u32 *w, u32 *h);

/* Grabs a window's contents as RGBA8 straight-alpha (for tests/tools).
 * Returns a malloc'd buffer of w*h pixels, NULL on failure. */
u32 *x11_screenshot(X11 *x, int win, u32 *w, u32 *h);

#endif /* REV_X11_H */

/* GPU surface support: window XID (for EGL/Vulkan surface creation on the
 * same server from another library's connection) and the window's visual. */
u32 x11_window_xid(X11 *x, int win);
u32 x11_win_visual(X11 *x, int win);

/* Destroy + re-create a window in the same slot (new XID, same index). */
bool x11_resize_window(X11 *x, int win, RRect geom);
