/* ============================================================================
 * Reverie Engine — x11.h
 * A raw X11 wire-protocol client speaking directly to the X server. No
 * Xlib, no XCB. Implements: connection setup with Xauthority
 * MIT-MAGIC-COOKIE-1 auth (real big-endian interleaved records; unix,
 * abstract-socket and TCP transports), visual selection, window
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

/* Connects to the display in $DISPLAY. Local DISPLAY hosts ("", "unix",
 * "box/unix", "localhost", loopback literals) use the unix socket
 * (localhost also falls back to TCP); remote hosts use TCP 6000+display.
 * Auth: MIT-MAGIC-COOKIE-1 from $XAUTHORITY (or ~/.Xauthority) when a
 * matching entry exists — see x11_xauth_lookup. */
X11 *x11_connect(char *err, size_t errcap);
void x11_disconnect(X11 *x);

/* Finds the MIT-MAGIC-COOKIE-1 for `display` (reached via DISPLAY-host
 * `host`, "" = local) in the .Xauthority file `path`. True + the cookie
 * copied to `out` (capacity 64) and its length to `outlen` on a match.
 * Pure parser — unit-tested against files generated in the real
 * big-endian interleaved wire format; the e2e x11(auth) legs prove the
 * bytes a live enforcing Xorg accepts. */
bool x11_xauth_lookup(const char *path, const char *host, int display,
                       u8 *out, u32 *outlen);

/* Monitor list via Xinerama (falls back to the full root window). The list
 * is re-queried on every call — runtime changes (hotplug) surface here.
 * REV_X11_MON_FILE=<path> overrides the query with a deterministic text
 * file ("x y w h" per line, # comments) for hotplug e2e under Xvfb;
 * malformed files fall back to the server with a warning.
 * x11_mon_file_parse is the pure parser (unit-tested). */
int  x11_monitors(X11 *x, RRect *out, int max);
int  x11_mon_file_parse(const char *path, RRect *out, int max);
/* Stable hotplug identity of monitor `mon` in the LAST x11_monitors() list:
 * a key derived from the Xinerama origin (see x11.c for the tradeoffs). */
u64  x11_monitor_id(X11 *x, int mon);

/* Creates a desktop-type wallpaper window covering `geom`. Returns the
 * window index or -1. Indices are SLOTS: x11_remove_window frees them and
 * a later add reuses the number (renderer per-window state relies on it). */
int  x11_add_window(X11 *x, RRect geom);
/* Destroys a window (XID, GC, SHM segments) and frees its slot. The caller
 * must have released renderer-owned surfaces for the XID first. */
bool x11_remove_window(X11 *x, int win);

/* Presents an RGBA8 (straight alpha) buffer of exactly the window size. */
bool x11_present(X11 *x, int win, const u32 *rgba, u32 w, u32 h);

/* Polls the root pointer: root-relative coords + button mask. */
bool x11_query_pointer(X11 *x, i32 *rx, i32 *ry, u32 *mask);

/* Latched button edges since the last x11_pointer() call: press/release
 * transitions observed in the event stream, so a click whose press AND
 * release both happen between two polls still registers. */
bool x11_pointer(X11 *x, RPointerState *out);

/* Event-loop sleep: polls the X connection fd for up to `seconds` while
 * draining events — input keeps flowing between frames. Windows that
 * latched an Expose (workspace switches under WMs like openbox) are
 * re-presented from the backend's own memory inside the wait, so static
 * content snaps back instantly even at low pacing. */
void x11_wait_frame(X11 *x, f64 seconds);

/* Reads pending events, updating internal window geometry. */
void x11_pump_events(X11 *x);

/* Immediate Expose service: re-presents any window that latched an Expose
 * since the last call (workspace switches under WMs like openbox). Safe
 * only from top-level contexts — wait_frame and the platform pump call it. */
void x11_flush_expose(X11 *x);

/* Wire field offsets for the geometry-carrying events (Expose 12,
 * ConfigureNotify 22) — extracted so unit tests can pin the protocol
 * layout the pump reads. Unknown codes zero every output. */
void x11_event_geometry(const u8 ev[32], u32 *win, i32 *x, i32 *y, u32 *w, u32 *h);

/* True if a window's geometry changed since the last check. */
bool x11_win_geometry(X11 *x, int win, RRect *geom);

/* Round-trip sync (flushes the request pipe). */
void x11_sync(X11 *x);

/* Root geometry (screen size). */
void x11_root_size(X11 *x, u32 *w, u32 *h);

/* Display power state (idle power-save). x11_display_active returns:
 *   1 = display active (DPMS level on / screensaver off)
 *   0 = powered down or screensaver on
 *  -1 = cannot tell (no DPMS/SCREEN-SAVER extension, or a failed query)
 * REV_X11_DPMS_FILE=<path> overrides the wire query with a file whose text
 * is "on" / "standby" / "suspend" / "off" (e2e test hook — same code path
 * a real power transition takes). x11_dpms_file_level is the pure parser. */
int  x11_display_active(X11 *x);
bool x11_display_state_available(X11 *x);   /* any signal source at all */
int  x11_dpms_file_level(const char *text); /* -1, or 0..3 (on..off)     */

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
