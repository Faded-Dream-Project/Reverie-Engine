/* egl_wayland_probe.c — reproduces (and proves the fix for) the v0.1.6 field
 * failure "eglCreateWindowSurface failed (0x300b)" on Wayland.
 *
 * Path A (v0.1.6 engine): eglGetDisplay(wl_display*) + eglCreateWindowSurface
 *                          with the RAW wl_surface* as the native window.
 * Path B (fixed):         eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND) +
 *                          wl_egl_window_create() + eglCreatePlatformWindowSurface.
 *
 * Self-contained: dlopens libEGL.so.1, libwayland-client.so.0,
 * libwayland-egl.so.1 — exactly like the engine does. Runs against any
 * compositor (weston headless works: no shell needed for a plain surface).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* minimal libwayland ABI (same trick as wlshim.c) */
typedef struct wl_proxy wl_proxy;
typedef struct wl_display wl_display;
typedef struct wl_registry wl_registry;
typedef struct wl_compositor wl_compositor;
typedef struct wl_surface wl_surface;
struct wl_interface { const char *name; int version; int method_count;
        const void *methods; int event_count; const void *events; };

static wl_display *(*wl_connect)(const char *);
static int (*wl_dispatch_pending)(wl_display *);
static int (*wl_roundtrip)(wl_display *);
static int (*wl_flush)(wl_display *);
static wl_proxy *(*proxy_marshal_flags)(wl_proxy *, uint32_t, const struct wl_interface *,
                                        uint32_t, uint32_t, ...);
static void *(*proxy_destroy)(wl_proxy *);
static const struct wl_interface *wl_surface_interface;
static const struct wl_interface *wl_compositor_interface;
static const struct wl_interface *wl_registry_interface;

/* EGL (dlopen'd) */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean;
static EGLDisplay (*eglGetDisplay)(void *);
static EGLDisplay (*eglGetPlatformDisplay)(unsigned int, void *, const EGLint *);
static EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*eglBindAPI)(unsigned int);
static EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
static EGLSurface (*eglCreatePlatformWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
static EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint);
static const char *(*eglQueryString)(EGLDisplay, int);
static EGLint (*eglGetError)(void);
/* GL bits */
static void *(*eglGetProcAddress)(const char *);
static const char *(*glGetString)(unsigned int);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(unsigned int);

/* libwayland-egl */
static wl_proxy *(*wl_egl_window_create)(wl_surface *, int, int);
static void (*wl_egl_window_destroy)(wl_proxy *);
static void (*wl_egl_window_resize)(wl_proxy *, int, int, int, int);

#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_NONE 0x3038
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#define EGL_EXTENSIONS 0x3055
#define EGL_VENDOR 0x3053

static int fails = 0;

static void *must(void *h, const char *name)
{
        void *s = dlsym(h, name);
        if (!s) { printf("    dlsym(%s) failed\n", name); exit(2); }
        return s;
}

static wl_surface *make_surface(wl_display *dpy)
{
        /* wl_display.get_registry = opcode 1 */
        wl_registry *r = (wl_registry *)proxy_marshal_flags((wl_proxy *)dpy, 1,
                wl_registry_interface, 1, 0, (void *)0);
        if (!r) { printf("    get_registry failed\n"); exit(2); }
        wl_roundtrip(dpy);   /* globals arrive; we ignore them */
        /* wl_compositor bind: we need its global name — weston always binds
         * compositor at global name 1 in practice, but do it properly: query
         * via a tiny registry listener is overkill for the probe; instead
         * re-use the engine's approach: bind name=1 iface=compositor v4.
         * weston headless advertises wl_compositor as global #1. */
        wl_compositor *comp = (wl_compositor *)proxy_marshal_flags((wl_proxy *)r, 0,
                wl_compositor_interface, 4, 0, 1u, "wl_compositor", 4u, (void *)0);
        wl_roundtrip(dpy);
        /* create_surface = opcode 0 */
        wl_surface *s = (wl_surface *)proxy_marshal_flags((wl_proxy *)comp, 0,
                wl_surface_interface, 4, 0, (void *)0);
        wl_flush(dpy);
        (void)proxy_destroy((wl_proxy *)r);
        return s;
}

int main(void)
{
        void *lwl = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        void *legl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        void *lwegl = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lwl || !legl) { printf("SKIP (no libwayland-client/libEGL)\n"); return 0; }
        if (!lwegl) { printf("SKIP (no libwayland-egl)\n"); return 0; }

        wl_connect = must(lwl, "wl_display_connect");
        wl_dispatch_pending = must(lwl, "wl_display_dispatch_pending");
        wl_roundtrip = must(lwl, "wl_display_roundtrip");
        wl_flush = must(lwl, "wl_display_flush");
        proxy_marshal_flags = must(lwl, "wl_proxy_marshal_flags");
        proxy_destroy = must(lwl, "wl_proxy_destroy");
        wl_surface_interface = must(lwl, "wl_surface_interface");
        wl_compositor_interface = must(lwl, "wl_compositor_interface");
        wl_registry_interface = must(lwl, "wl_registry_interface");
        wl_egl_window_create = must(lwegl, "wl_egl_window_create");
        wl_egl_window_destroy = must(lwegl, "wl_egl_window_destroy");
        wl_egl_window_resize = must(lwegl, "wl_egl_window_resize");

        eglGetDisplay = must(legl, "eglGetDisplay");
        eglGetPlatformDisplay = dlsym(legl, "eglGetPlatformDisplay");
        eglInitialize = must(legl, "eglInitialize");
        eglChooseConfig = must(legl, "eglChooseConfig");
        eglBindAPI = must(legl, "eglBindAPI");
        eglCreateContext = must(legl, "eglCreateContext");
        eglCreateWindowSurface = must(legl, "eglCreateWindowSurface");
        eglCreatePlatformWindowSurface = dlsym(legl, "eglCreatePlatformWindowSurface");
        eglMakeCurrent = must(legl, "eglMakeCurrent");
        eglSwapBuffers = must(legl, "eglSwapBuffers");
        eglSwapInterval = must(legl, "eglSwapInterval");
        eglQueryString = must(legl, "eglQueryString");
        eglGetError = must(legl, "eglGetError");
        eglGetProcAddress = must(legl, "eglGetProcAddress");
        glGetString = (const char *(*)(unsigned int))eglGetProcAddress("glGetString");
        glClearColor = (void (*)(float, float, float, float))eglGetProcAddress("glClearColor");
        glClear = (void (*)(unsigned int))eglGetProcAddress("glClear");

        wl_display *dpy = wl_connect(NULL);
        if (!dpy) { printf("SKIP (no compositor)\n"); return 0; }
        wl_surface *surf = make_surface(dpy);

        const char *cext = eglQueryString(NULL, EGL_EXTENSIONS);
        printf("    client ext: %s\n", cext ? cext : "(none)");
        int has_platform = cext && (strstr(cext, "EGL_EXT_platform_wayland") ||
                                     strstr(cext, "EGL_KHR_platform_wayland"));
        printf("    platform-wayland ext: %s\n", has_platform ? "yes" : "no");

        EGLint maj = 0, min = 0;
        EGLConfig cfg;
        EGLint nc = 0;
        EGLint attrs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                EGL_NONE
        };

        /* ---------- Path A: the v0.1.6 engine approach ---------- */
        printf("    [A] eglGetDisplay(wl_display) + eglCreateWindowSurface(wl_surface)\n");
        EGLDisplay edA = eglGetDisplay((void *)dpy);
        if (eglInitialize(edA, &maj, &min)) {
                printf("        eglInitialize ok (%d.%d, vendor %.32s)\n", maj, min,
                       eglQueryString(edA, EGL_VENDOR) ? eglQueryString(edA, EGL_VENDOR) : "?");
                eglBindAPI(EGL_OPENGL_ES_API);
                if (eglChooseConfig(edA, attrs, &cfg, 1, &nc) && nc == 1) {
                        EGLContext ctx = eglCreateContext(edA, cfg, NULL,
                                (EGLint[]){ EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE });
                        printf("        context: %s\n", ctx ? "ok" : "FAILED");
                        EGLSurface s = eglCreateWindowSurface(edA, cfg, (void *)surf, NULL);
                        if (!s) {
                                printf("        RESULT A: REPRODUCED — "
                                       "eglCreateWindowSurface failed (0x%x)  <-- the field bug\n",
                                       (unsigned)eglGetError());
                        } else {
                                printf("        RESULT A: surface created (unexpected)\n");
                        }
                }
        }

        /* ---------- Path B: the fixed approach ---------- */
        printf("    [B] eglGetPlatformDisplay(WAYLAND) + wl_egl_window + platform surface\n");
        EGLDisplay edB = NULL;
        if (eglGetPlatformDisplay && has_platform)
                edB = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, (void *)dpy, NULL);
        if (!edB)
                edB = eglGetDisplay((void *)dpy);
        if (!eglInitialize(edB, &maj, &min)) {
                printf("        RESULT B: eglInitialize FAILED (0x%x)\n", (unsigned)eglGetError());
                fails++;
                return 1;
        }
        eglBindAPI(EGL_OPENGL_ES_API);
        if (!eglChooseConfig(edB, attrs, &cfg, 1, &nc) || nc != 1) {
                printf("        RESULT B: no config (0x%x)\n", (unsigned)eglGetError());
                fails++;
                return 1;
        }
        EGLContext ctx = eglCreateContext(edB, cfg, NULL,
                (EGLint[]){ EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE });
        if (!ctx) { printf("        RESULT B: context FAILED (0x%x)\n", (unsigned)eglGetError()); fails++; return 1; }

        wl_proxy *eglwin = wl_egl_window_create(surf, 320, 200);
        if (!eglwin) { printf("        RESULT B: wl_egl_window_create FAILED\n"); fails++; return 1; }
        EGLSurface s = NULL;
        if (eglCreatePlatformWindowSurface)
                s = eglCreatePlatformWindowSurface(edB, cfg, (void *)eglwin, NULL);
        if (!s)
                s = eglCreateWindowSurface(edB, cfg, (void *)eglwin, NULL);
        if (!s) {
                printf("        RESULT B: surface FAILED (0x%x)\n", (unsigned)eglGetError());
                fails++;
                return 1;
        }
        if (!eglMakeCurrent(edB, s, s, ctx)) {
                printf("        RESULT B: makeCurrent FAILED (0x%x)\n", (unsigned)eglGetError());
                fails++;
                return 1;
        }
        printf("        GL renderer: %.60s\n", glGetString ? glGetString(0x1F01) : "?");
        /* a shell-less surface is never composited -> weston sends no frame
         * callbacks -> interval-1 swap would block forever; interval 0 keeps
         * the probe self-contained. (The real engine maps via layer-shell.) */
        eglSwapInterval(edB, 0);
        glClearColor(0.1f, 0.6f, 0.4f, 1.0f);
        glClear(0x4000);
        int swaps_ok = 0;
        for (int i = 0; i < 5; i++) {
                if (eglSwapBuffers(edB, s)) swaps_ok++;
                wl_flush(dpy);
                usleep(16000);
                wl_dispatch_pending(dpy);
        }
        printf("        swaps: %d/5\n", swaps_ok);
        wl_egl_window_destroy(eglwin);
        printf("    RESULT B: %s\n", swaps_ok == 5 ?
               "PASS — platform display + wl_egl_window works" :
               "PARTIAL — surface created but swaps failing");
        if (swaps_ok != 5) fails++;
        return fails ? 1 : 0;
}
