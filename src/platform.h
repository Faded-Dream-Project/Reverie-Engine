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
 * updated by pump(). dx/dy are deltas accumulated since the last pointer()
 * call (consume-on-read).
 *
 * Edge latching (dev): a click whose press AND release both arrive between
 * two polls must still register. Button transitions are latched as edge flags
 * and consumed by pointer() — a click is reported even when it lived entirely
 * inside a single event batch (or a single slow frame). */
typedef struct {
        bool valid;      /* pointer position known                    */
        bool pressed;    /* any button held (current state)           */
        bool press_edge;     /* a press was seen since last read      */
        bool release_edge;   /* a release was seen since last read    */
        bool click;          /* full press+release since last read    */
        f64 x, y;       /* global coordinates                        */
        f64 dx, dy;      /* accumulated delta since last read         */
        f64 click_x, click_y;  /* position of the FIRST latched click */
        /* Latched click QUEUE (2026-10): two full press+release pairs can
         * land between two polls (fast double-click on a slow frame); a
         * single (click_x, click_y) collapsed them into N splashes at the
         * LAST position. Each latched click keeps its own position here;
         * consumers should walk click_q[0..click_qn). click/click_x/click_y
         * stay as the "first click" compat view. */
        f64 click_q[4][2];   /* up to 4 latched clicks, oldest first   */
        u32 click_qn;        /* 0..4 latched since last read            */
        u32 clicks;     /* total click count (monotonic, for stats)  */
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
        /* Current output refresh rate in Hz (rounded); 0 = unknown (X11 core
         * does not report it — pacing then falls back to the scene fps). */
        int  (*monitor_refresh)(RPlatform *p, int mon);

        /* ---- windows ---- */
        int  (*add_window)(RPlatform *p, RRect geom);
        /* Present a CPU-readable RGBA frame. `premultiplied` says what the
         * bytes in `rgba` already are: true = the caller premultiplied them
         * against alpha (the Vulkan tier does this on the GPU); false =
         * straight alpha, and a premul-expecting presenter (wl_shm
         * ARGB8888) premultiplies itself. X11 has no premul convention and
         * ignores the flag. */
        bool (*present_cpu)(RPlatform *p, int win, const u32 *rgba, u32 w, u32 h,
                            bool premultiplied);
        /* True when this platform's present path natively consumes
         * PREMULTIPLIED input (wl_shm ARGB8888). Renderers check it to
         * decide where the straight->premul conversion runs (GPU kernel +
         * memcpy vs straight pass-through). */
        bool present_premul;
        /* True when the presenter's wire byte order is B,G,R,A — wl_shm
         * ARGB8888 packs the u32 as 0xAARRGGBB, so the FIRST byte in
         * memory is the blue channel, the opposite of the engine's
         * 0xAABBGGRR native order. Renderers with a GPU copy kernel
         * swizzle R/B on store when this is set; wlshim's CPU premultiply
         * leg applies the same mapping. False on X11 (x11.c converts into
         * the visual's masks itself, engine order in). */
        bool present_bgra;

        /* ---- GPU surface support ----
         * has_gpu:   accelerated present possible on this platform
         *            (X11: always true — EGL needs only the XID;
         *             Wayland: only when the WSI shim is active)
         * native:    X11 -> XID; Wayland -> wl_surface* from the shim, else 0
         *            (renderers wrap it: EGL needs a wl_egl_window, Vulkan
         *            the raw wl_surface — see render_gl.c / render_vk.c)
         * display:   Wayland+shim -> wl_display* (for EGL's platform display);
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

        /* ---- frame pacing (dev) ------------------------------------------
         * wait_frame(): block up to `seconds` (>= 0) while PROCESSING INPUT
         * EVENTS, returning early when the display signals readiness (the
         * Wayland frame callback latched the last presented frame). Replaces
         * blind nanosleep pacing: a slow frame can no longer swallow input,
         * and presentation aligns with the compositor's repaint cycle.
         * Returns true if a frame signal arrived before the timeout. */
        bool (*wait_frame)(RPlatform *p, f64 seconds);
        /* Presented-but-dropped frames (both shm slots held by the server).
         * Used by the perf stats — cheap to query, monotonic. */
        u64  (*dropped_frames)(RPlatform *p);

        /* ---- visibility (idle power-save) --------------------------------
         * False when the platform knows no surface is being shown (Wayland:
         * every wl_shm-presented window went unlatched past the visibility
         * window — fullscreen app on top, display asleep). The daemon then
         * stops rendering and paces at a slow probe rate until the platform
         * flips back. NULL = platform cannot tell (X11 classic, raw Wayland
         * client) and the caller must assume always-visible. */
        bool (*surface_visible)(RPlatform *p);

        /* ---- WSI-present visibility support (Wayland shim) -----------------
         * wsi_frame_request(): called by a renderer RIGHT BEFORE it presents
         * through its own WSI swapchain on `win`. The platform attaches a
         * latched-frame signal (Wayland: a wl_surface.frame callback on the
         * very surface the swapchain commits) so surface_visible() gains the
         * same callback-starvation evidence the wl_shm path already uses.
         * The callback request rides the same event queue the driver's
         * present commit flushes, so the compositor fires it exactly when
         * that present is latched. NULL = nothing to do (X11, raw client).
         *
         * hidden_render_probe(): true when the platform's idle detection
         * CANNOT probe on its own and needs the engine to keep rendering at
         * the idle pace while hidden — Vulkan WSI: only a real swapchain
         * present (buffer attach + commit) can carry the frame request a
         * sleeping compositor will eventually answer. NULL/false = the old
         * behavior (skip rendering entirely; wl_shm re-attach probes). */
        bool (*wsi_frame_request)(RPlatform *p, int win);
        bool (*hidden_render_probe)(RPlatform *p);

        /* ---- tests / tools ---- */
        u32 *(*screenshot)(RPlatform *p, int win, u32 *w, u32 *h);
};

/* pref: "auto" | "x11" | "wayland".
 * auto: WAYLAND_DISPLAY set -> wayland; else DISPLAY set -> x11;
 *       else try wayland socket, then X. */
RPlatform *platform_create(const char *pref, char *err, size_t errcap);
void platform_destroy(RPlatform *p);

#endif /* REV_PLATFORM_H */
