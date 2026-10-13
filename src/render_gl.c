/* ============================================================================
 * Reverie Engine — render_gl.c
 * OpenGL ES 3 renderer driven entirely through dlopen("libEGL.so.1") +
 * eglGetProcAddress — zero EGL/GL linkage at build time.
 *
 * X11:    eglGetDisplay(EGL_DEFAULT_DISPLAY) makes Mesa open its own X
 *         connection; window surfaces need only the raw XID from our own
 *         hand-written X11 client. No Xlib is ever linked or loaded by us.
 * Wayland: requires the WSI shim (wlshim.c) to provide wl_display/wl_surface
 *          handles; without it init fails and the chain falls to CPU.
 *          dev: the display is opened through the EGL platform API
 *          (EGL_EXT_platform_wayland) when available, and every window
 *          surface is created on a wl_egl_window from libwayland-egl —
 *          handing the RAW wl_surface to eglCreateWindowSurface is the
 *          v0.1.6 field failure "eglCreateWindowSurface failed (0x300b)"
 *          (EGL_BAD_NATIVE_WINDOW). While a wl_egl_window is attached the
 *          shim's wl_shm presenter stays off that surface (see
 *          wlshim_set_gl_surface) so the two present paths cannot fight
 *          over commits.
 *
 * llvmpipe: when Mesa reports a software rasterizer we log it and keep
 *         running — that is precisely the "OpenGL on llvmpipe" tier of the
 *         chain.
 *
 * Frame pipeline (GPU compositing):
 *   upload raster -> premultiply -> [downsample + separable gaussian blur
 *   + upscale] -> W3C blend onto an accumulation FBO -> present (unpremult).
 * The blend shader mirrors composite.c's blend_pixel() exactly, so GPU and
 * CPU output stay comparable.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "render_gl.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "platform.h"
#include "wlshim.h"

/* ---- EGL constants (Khronos registry values) ---------------------------- */
#define EGL_SUCCESS 0x3000
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_EXTENSIONS 0x3055
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#define EGL_SURFACE_TYPE 0x3033
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_WINDOW_BIT 0x0004
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

/* ---- GL constants -------------------------------------------------------- */
#define GL_TRIANGLES 0x0004
#define GL_TEXTURE_2D 0x0DE1
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B4C
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_ACTIVE_TEXTURE 0x84E0

typedef unsigned int EGLBoolean_;
typedef int EGLint_;
typedef void *EGLDisplay_;
typedef void *EGLConfig_;
typedef void *EGLContext_;
typedef void *EGLSurface_;
typedef unsigned int GLuint_;
typedef int GLint_;
typedef int GLsizei_;
typedef char GLchar_;
typedef unsigned int GLenum_;
typedef unsigned char GLboolean_;
typedef float GLfloat_;

/* ---- GL function table --------------------------------------------------- */
typedef struct {
        void *(*eglGetProcAddress)(const char *);

        void (*glActiveTexture)(GLenum_);
        void (*glBindTexture)(GLenum_, GLuint_);
        void (*glGenTextures)(GLsizei_, GLuint_ *);
        void (*glDeleteTextures)(GLsizei_, const GLuint_ *);
        void (*glTexImage2D)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLint_,
                             GLenum_, GLenum_, const void *);
        void (*glTexSubImage2D)(GLenum_, GLint_, GLint_, GLint_, GLsizei_, GLsizei_,
                                GLenum_, GLenum_, const void *);
        void (*glTexParameteri)(GLenum_, GLenum_, GLint_);
        void (*glGenFramebuffers)(GLsizei_, GLuint_ *);
        void (*glDeleteFramebuffers)(GLsizei_, const GLuint_ *);
        void (*glBindFramebuffer)(GLenum_, GLuint_);
        void (*glFramebufferTexture2D)(GLenum_, GLenum_, GLenum_, GLuint_, GLint_);
        GLenum_ (*glCheckFramebufferStatus)(GLenum_);
        GLuint_ (*glCreateShader)(GLenum_);
        void (*glShaderSource)(GLuint_, GLsizei_, const GLchar_ *const *, const GLint_ *);
        void (*glCompileShader)(GLuint_);
        void (*glDeleteShader)(GLuint_);
        void (*glGetShaderiv)(GLuint_, GLenum_, GLint_ *);
        void (*glGetShaderInfoLog)(GLuint_, GLsizei_, GLsizei_ *, GLchar_ *);
        GLuint_ (*glCreateProgram)(void);
        void (*glAttachShader)(GLuint_, GLuint_);
        void (*glLinkProgram)(GLuint_);
        void (*glDeleteProgram)(GLuint_);
        void (*glGetProgramiv)(GLuint_, GLenum_, GLint_ *);
        void (*glGetProgramInfoLog)(GLuint_, GLsizei_, GLsizei_ *, GLchar_ *);
        void (*glUseProgram)(GLuint_);
        GLint_ (*glGetUniformLocation)(GLuint_, const GLchar_ *);
        void (*glUniform1i)(GLint_, GLint_);
        void (*glUniform1f)(GLint_, GLfloat_);
        void (*glUniform2f)(GLint_, GLfloat_, GLfloat_);
        void (*glUniform1fv)(GLint_, GLsizei_, const GLfloat_ *);
        void (*glDrawArrays)(GLenum_, GLint_, GLsizei_);
        void (*glClearColor)(GLfloat_, GLfloat_, GLfloat_, GLfloat_);
        void (*glClear)(GLenum_);
        void (*glViewport)(GLint_, GLint_, GLsizei_, GLsizei_);
        void (*glDisable)(GLenum_);
        GLenum_ (*glGetError)(void);
        void (*glReadPixels)(GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, void *);
        const GLchar_ *(*glGetString)(GLenum_);
} GL;

#define LOADGL(fn, name) \
        do { \
                *(void **)(&g->fn) = g->eglGetProcAddress(name); \
                if (!g->fn) { \
                        snprintf(err, errcap, "eglGetProcAddress(%s) failed", name); \
                        return false; \
                } \
        } while (0)

/* ---- shaders (GLSL ES 3.00) --------------------------------------------- */
static const char *VS =
        "#version 300 es\n"
        "const vec2 P[3] = vec2[3](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));\n"
        "uniform vec2 u_off;\n"
        "uniform vec2 u_scale;\n"
        "out vec2 v_uvsrc;\n"
        "out vec2 v_screen;\n"
        "void main() {\n"
        "    vec2 p = P[gl_VertexID];\n"
        "    v_screen = p * 0.5 + 0.5;\n"
        "    v_uvsrc = v_screen * u_scale + u_off;\n"
        "    gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";

/* straight -> premultiplied (nearest sampling) */
static const char *FS_PREMULT =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D u_tex;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec4 s = texture(u_tex, v_uvsrc);\n"
        "    fragColor = vec4(s.rgb * s.a, s.a);\n"
        "}\n";

/* plain copy (linear filtering; premul-safe) */
static const char *FS_COPY =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D u_tex;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() { fragColor = texture(u_tex, v_uvsrc); }\n";

/* separable 9-tap gaussian */
static const char *FS_BLUR =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec2 u_dir;\n"
        "uniform float u_w[9];\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec4 sum = vec4(0.0);\n"
        "    for (int i = 0; i < 9; i++)\n"
        "        sum += texture(u_tex, v_uvsrc + u_dir * float(i - 4)) * u_w[i];\n"
        "    fragColor = sum;\n"
        "}\n";

/* W3C separable blend of a premultiplied layer over a premultiplied accum —
 * mirrors composite.c blend_pixel() (straight-alpha math, premul storage). */
static const char *FS_COMPOSITE =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D u_src;\n"
        "uniform sampler2D u_dst;\n"
        "uniform int u_blend;\n"
        "uniform float u_opacity;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "vec3 blend_fn(int m, vec3 cb, vec3 cs) {\n"
        "    if (m == 1) return clamp(cb + cs, 0.0, 1.0);\n"
        "    if (m == 2) return cb + cs - cb * cs;\n"
        "    if (m == 3) return cb * cs;\n"
        "    if (m == 4) return mix(2.0 * cb * cs,\n"
        "                           1.0 - 2.0 * (1.0 - cb) * (1.0 - cs),\n"
        "                           step(vec3(0.5), cb));\n"
        "    return cs;\n"
        "}\n"
        "void main() {\n"
        "    if (v_uvsrc.x < 0.0 || v_uvsrc.x > 1.0 ||\n"
        "        v_uvsrc.y < 0.0 || v_uvsrc.y > 1.0) discard;\n"
        "    vec4 sp = texture(u_src, v_uvsrc);\n"
        "    vec4 dp = texture(u_dst, v_screen);\n"
        "    float as = sp.a * u_opacity;\n"
        "    float ad = dp.a;\n"
        "    vec3 cs = sp.a > 0.0 ? sp.rgb / sp.a : vec3(0.0);\n"
        "    vec3 cd = ad  > 0.0 ? dp.rgb / ad : vec3(0.0);\n"
        "    float ao = as + ad * (1.0 - as);\n"
        "    if (ao <= 0.0) { fragColor = vec4(0.0); return; }\n"
        "    float k1 = as * (1.0 - ad);\n"
        "    float k3 = (1.0 - as) * ad;\n"
        "    vec3 b = blend_fn(u_blend, cd, cs);\n"
        "    vec3 co = (k1 * cs + k3 * cd + as * ad * b) / ao;\n"
        "    fragColor = vec4(clamp(co, 0.0, 1.0) * ao, ao);\n"
        "}\n";

/* present: straight texture passthrough (cpu_frame path) */
static const char *FS_PRESENT =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D u_tex;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() { fragColor = texture(u_tex, v_uvsrc); }\n";

/* ---- renderer state ------------------------------------------------------- */
enum { PROG_PREMULT, PROG_COPY, PROG_BLUR, PROG_COMPOSITE, PROG_PRESENT, PROG_COUNT };

typedef struct {
        GLuint_ tex;
        GLuint_ fbo;
        u32 w, h;
} RTarget;

typedef struct {
        void *lib;
        GL g;
        EGLDisplay_ dpy;
        EGLContext_ ctx;
        EGLConfig_ cfg;
        int es_version;
        bool is_wayland;   /* platform-specific window surface creation  */

        /* libwayland-egl.so.1 (Wayland only, dlopen'd): the wl_egl_window
         * factory EGL window surfaces are created on */
        void *wegl_lib;
        void *(*wl_egl_window_create)(void *surface, int w, int h);
        void  (*wl_egl_window_destroy)(void *win);
        void  (*wl_egl_window_resize)(void *win, int w, int h, int dx, int dy);

        /* egl entry points */
        EGLDisplay_ (*eglGetDisplay)(void *);
        EGLDisplay_ (*eglGetPlatformDisplay)(unsigned long, void *, const EGLint_ *);
        const char *(*eglQueryString)(EGLDisplay_, EGLint_);
        EGLSurface_ (*eglCreatePlatformWindowSurface)(EGLDisplay_, EGLConfig_, void *, const EGLint_ *);
        EGLBoolean_ (*eglInitialize)(EGLDisplay_, EGLint_ *, EGLint_ *);
        EGLBoolean_ (*eglChooseConfig)(EGLDisplay_, const EGLint_ *, EGLConfig_ *, EGLint_, EGLint_ *);
        EGLBoolean_ (*eglBindAPI)(unsigned long);
        EGLContext_ (*eglCreateContext)(EGLDisplay_, EGLConfig_, EGLContext_, const EGLint_ *);
        EGLSurface_ (*eglCreateWindowSurface)(EGLDisplay_, EGLConfig_, uintptr_t, const EGLint_ *);
        EGLBoolean_ (*eglDestroySurface)(EGLDisplay_, EGLSurface_);
        EGLBoolean_ (*eglMakeCurrent)(EGLDisplay_, EGLSurface_, EGLSurface_, EGLContext_);
        EGLBoolean_ (*eglSwapBuffers)(EGLDisplay_, EGLSurface_);
        EGLBoolean_ (*eglSwapInterval)(EGLDisplay_, EGLint_);
        EGLint_ (*eglGetError)(void);
        EGLBoolean_ (*eglGetConfigAttrib)(EGLDisplay_, EGLConfig_, EGLint_, EGLint_ *);

        /* per-window surfaces + ping-pong accumulation targets (reading and
         * writing the same FBO texture in one pass is a GL feedback loop) */
        struct {
                EGLSurface_ surf;
                u64 native;
                void *eglwin;    /* Wayland: wl_egl_window wrapping native  */
                u32 w, h;
        } win[REV_PLATFORM_MAX_OUTPUTS];
        RTarget accum[REV_PLATFORM_MAX_OUTPUTS][2];
        int accum_cur[REV_PLATFORM_MAX_OUTPUTS];

        /* scratch textures (sized per layer) */
        GLuint_ tex_src, tex_a, tex_b, tex_c, tex_d;
        u32 src_w, src_h, a_w, a_h, b_w, b_h, c_w, c_h, d_w, d_h;

        /* scratch blur chain FBOs */
        GLuint_ scratch_fbo[4];
        bool fbo_made;

        GLuint_ prog[PROG_COUNT];
        struct {
                int off, scale, tex;
                int dir, weights;
                int src, dst, blend, opacity;
        } u[PROG_COUNT];

        RPlatform *plat;
        int cur_win;
        bool software;    /* llvmpipe/softpipe/swrast detected */
        bool gl_loaded;   /* GL function table is valid            */
} RenderGL;

/* ---- small helpers -------------------------------------------------------- */

static bool gl_make_program(RenderGL *r, int idx, const char *fs, char *err, size_t errcap);

static void set_tex_params(GL *g, GLuint_ t, bool linear)
{
        g->glBindTexture(GL_TEXTURE_2D, t);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool ensure_tex(GL *g, GLuint_ *tex, u32 *tw, u32 *th, u32 w, u32 h, bool linear)
{
        if (!*tex) {
                g->glGenTextures(1, tex);
                set_tex_params(g, *tex, linear);
        }
        if (*tw != w || *th != h) {
                g->glBindTexture(GL_TEXTURE_2D, *tex);
                g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)w, (int)h, 0, GL_RGBA,
                                GL_UNSIGNED_BYTE, NULL);
                *tw = w;
                *th = h;
        }
        return *tex != 0;
}

static bool ensure_target(GL *g, RTarget *t, u32 w, u32 h)
{
        if (t->tex && t->w == w && t->h == h)
                return true;
        if (!t->tex)
                g->glGenTextures(1, &t->tex);
        if (!t->fbo)
                g->glGenFramebuffers(1, &t->fbo);
        set_tex_params(g, t->tex, true);
        g->glBindTexture(GL_TEXTURE_2D, t->tex);
        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)w, (int)h, 0, GL_RGBA,
                        GL_UNSIGNED_BYTE, NULL);
        g->glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                return false;
        t->w = w;
        t->h = h;
        return true;
}

static void draw_full(GL *g)
{
        g->glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void gauss9(double sigma, float w[9])
{
        if (sigma < 0.4)
                sigma = 0.4;
        double sum = 0;
        for (int i = 0; i < 9; i++) {
                double x = (double)(i - 4);
                w[i] = (float)exp(-(x * x) / (2.0 * sigma * sigma));
                sum += w[i];
        }
        for (int i = 0; i < 9; i++)
                w[i] = (float)(w[i] / sum);
}

/* ---- window surface management -------------------------------------------- */

static bool gl_window_surface(RenderGL *r, int win)
{
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return false;
        u64 native = r->plat->native_handle(r->plat, win);
        if (!native)
                return false;
        if (r->win[win].surf && r->win[win].native == native)
                return true;    /* unchanged */
        if (r->win[win].surf) {
                r->eglDestroySurface(r->dpy, r->win[win].surf);
                r->win[win].surf = NULL;
        }
        if (r->win[win].eglwin) {
                r->wl_egl_window_destroy(r->win[win].eglwin);
                r->win[win].eglwin = NULL;
        }
        r->win[win].native = native;

        if (r->is_wayland) {
                /* A wl_surface is NOT an EGLNativeWindow: it must be wrapped
                 * in a wl_egl_window (libwayland-egl), and the surface created
                 * through the platform API. Passing the raw proxy is exactly
                 * the v0.1.6 "eglCreateWindowSurface failed (0x300b)" field
                 * failure. Dimensions are BUFFER pixels — the surface carries
                 * the output scale via set_buffer_scale, and the engine's
                 * framebuffers are already device-pixel sized. */
                if (!r->wl_egl_window_create)
                        return false;
                u32 w = 640, h = 480;
                RRect g;
                if (r->plat->win_geometry(r->plat, win, &g) && g.w > 0 && g.h > 0) {
                        w = (u32)g.w;
                        h = (u32)g.h;
                }
                r->win[win].eglwin = r->wl_egl_window_create((void *)(uintptr_t)native,
                                                             (int)w, (int)h);
                if (!r->win[win].eglwin)
                        return false;
                /* EGL owns this wl_surface from here on — keep the shim's
                 * wl_shm presenter off it (it would fight the EGL swapchain
                 * for attach/commit) */
                wlshim_set_gl_surface(win, true);
                if (r->eglCreatePlatformWindowSurface)
                        r->win[win].surf = r->eglCreatePlatformWindowSurface(
                                r->dpy, r->cfg, r->win[win].eglwin, NULL);
                else
                        r->win[win].surf = r->eglCreateWindowSurface(
                                r->dpy, r->cfg, (uintptr_t)r->win[win].eglwin, NULL);
                return r->win[win].surf != NULL;
        }

        r->win[win].surf = r->eglCreateWindowSurface(r->dpy, r->cfg, (uintptr_t)native, NULL);
        return r->win[win].surf != NULL;
}

/* ---- init ------------------------------------------------------------------ */

static bool gl_init(RRenderer *rr, RPlatform *p, char *err, size_t errcap)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        GL *g = &r->g;
        r->plat = p;

        if (!p->has_gpu) {
                snprintf(err, errcap, "platform '%s' has no GPU path", p->name);
                return false;
        }
        r->lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!r->lib) {
                snprintf(err, errcap, "libEGL.so.1 not loadable: %s", dlerror());
                return false;
        }
        *(void **)&r->eglGetDisplay = dlsym(r->lib, "eglGetDisplay");
        *(void **)&r->eglInitialize = dlsym(r->lib, "eglInitialize");
        *(void **)&r->eglChooseConfig = dlsym(r->lib, "eglChooseConfig");
        *(void **)&r->eglBindAPI = dlsym(r->lib, "eglBindAPI");
        *(void **)&r->eglCreateContext = dlsym(r->lib, "eglCreateContext");
        *(void **)&r->eglCreateWindowSurface = dlsym(r->lib, "eglCreateWindowSurface");
        *(void **)&r->eglDestroySurface = dlsym(r->lib, "eglDestroySurface");
        *(void **)&r->eglMakeCurrent = dlsym(r->lib, "eglMakeCurrent");
        *(void **)&r->eglSwapBuffers = dlsym(r->lib, "eglSwapBuffers");
        *(void **)&r->eglSwapInterval = dlsym(r->lib, "eglSwapInterval");
        *(void **)&r->eglGetError = dlsym(r->lib, "eglGetError");
        *(void **)&r->eglGetConfigAttrib = dlsym(r->lib, "eglGetConfigAttrib");
        if (!r->eglGetDisplay || !r->eglInitialize || !r->eglChooseConfig ||
            !r->eglCreateContext || !r->eglCreateWindowSurface || !r->eglMakeCurrent ||
            !r->eglSwapBuffers) {
                snprintf(err, errcap, "libEGL.so.1 is missing required symbols");
                return false;
        }

        /* display: X11 -> default (Mesa opens its own connection);
         * wayland -> the shim's wl_display*, through the platform API when
         * the vendor advertises EGL_EXT_platform_wayland. */
        r->is_wayland = strcmp(p->name, "wayland") == 0;
        void *native_dpy = p->native_display(p);
        if (r->is_wayland) {
                if (!native_dpy) {
                        snprintf(err, errcap,
                                 "wayland GPU requires the WSI shim (libwayland-client + REV_WAYLAND_GPU=1)");
                        return false;
                }
                *(void **)&r->eglGetPlatformDisplay = dlsym(r->lib, "eglGetPlatformDisplay");
                *(void **)&r->eglQueryString = dlsym(r->lib, "eglQueryString");
                *(void **)&r->eglCreatePlatformWindowSurface = dlsym(r->lib, "eglCreatePlatformWindowSurface");
                bool have_platform = false;
                if (r->eglGetPlatformDisplay && r->eglQueryString) {
                        const char *cext = r->eglQueryString(NULL, EGL_EXTENSIONS);
                        if (cext && (strstr(cext, "EGL_EXT_platform_wayland") ||
                                     strstr(cext, "EGL_KHR_platform_wayland")))
                                have_platform = true;
                }
                if (have_platform) {
                        r->dpy = r->eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
                                                          native_dpy, NULL);
                        LOGD("egl: platform display (EGL_EXT_platform_wayland)");
                } else {
                        r->dpy = r->eglGetDisplay(native_dpy);
                }
                if (!r->dpy) {
                        snprintf(err, errcap, "eglGetDisplay failed (0x%x)",
                                 (unsigned)(uintptr_t)r->eglGetError());
                        return false;
                }

                /* wl_egl_window factory — mandatory for GL on Wayland */
                r->wegl_lib = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
                if (!r->wegl_lib) {
                        snprintf(err, errcap,
                                 "libwayland-egl.so.1 not loadable: %s", dlerror());
                        return false;
                }
                *(void **)&r->wl_egl_window_create = dlsym(r->wegl_lib, "wl_egl_window_create");
                *(void **)&r->wl_egl_window_destroy = dlsym(r->wegl_lib, "wl_egl_window_destroy");
                *(void **)&r->wl_egl_window_resize = dlsym(r->wegl_lib, "wl_egl_window_resize");
                if (!r->wl_egl_window_create || !r->wl_egl_window_destroy ||
                    !r->wl_egl_window_resize) {
                        snprintf(err, errcap, "libwayland-egl.so.1 is missing required symbols");
                        return false;
                }
        } else {
                r->dpy = r->eglGetDisplay(native_dpy);
                if (!r->dpy) {
                        snprintf(err, errcap, "eglGetDisplay failed (0x%x)",
                             (unsigned)(uintptr_t)r->eglGetError());
                        return false;
                }
        }
        EGLint_ maj = 0, min = 0;
        if (!r->eglInitialize(r->dpy, &maj, &min)) {
                snprintf(err, errcap, "eglInitialize failed (0x%x)", (unsigned)(uintptr_t)r->eglGetError());
                return false;
        }
        LOGD("egl %d.%d", maj, min);

        if (!r->eglBindAPI((unsigned long)EGL_OPENGL_ES_API)) {
                snprintf(err, errcap, "eglBindAPI(OPENGL_ES) failed");
                return false;
        }

        /* config: try ES3 bit, then ES2 bit (Mesa exports ES2 on ES3 configs) */
        EGLConfig_ cfgs[16];
        EGLint_ nc = 0;
        for (int attempt = 0; attempt < 2 && nc == 0; attempt++) {
                EGLint_ attrs[16];
                int n = 0;
                attrs[n++] = EGL_SURFACE_TYPE;    attrs[n++] = EGL_WINDOW_BIT;
                attrs[n++] = EGL_RENDERABLE_TYPE; attrs[n++] = attempt == 0 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT;
                attrs[n++] = EGL_RED_SIZE;        attrs[n++] = 8;
                attrs[n++] = EGL_GREEN_SIZE;      attrs[n++] = 8;
                attrs[n++] = EGL_BLUE_SIZE;       attrs[n++] = 8;
                attrs[n++] = EGL_ALPHA_SIZE;      attrs[n++] = strcmp(p->name, "wayland") == 0 ? 8 : 0;
                attrs[n++] = EGL_NONE;
                r->eglChooseConfig(r->dpy, attrs, cfgs, 16, &nc);
        }
        if (nc < 1) {
                snprintf(err, errcap, "no EGL window config (0x%x)", (unsigned)(uintptr_t)r->eglGetError());
                return false;
        }
        r->cfg = cfgs[0];

        /* context: require ES 3.0 (universal in Mesa and hardware GL since ~2017;
         * GLSL ES 3.00 shaders are used throughout). Older stacks simply use the
         * CPU renderer. */
        const EGLint_ ctx3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        r->ctx = r->eglCreateContext(r->dpy, r->cfg, NULL, ctx3);
        r->es_version = 3;
        if (!r->ctx) {
                snprintf(err, errcap, "ES 3.0 context unavailable (0x%x)",
                         (unsigned)(uintptr_t)r->eglGetError());
                return false;
        }

        /* first surface (window 0 must exist — the engine creates windows first) */
        if (!gl_window_surface(r, 0)) {
                snprintf(err, errcap, "%s failed (0x%x)",
                         r->is_wayland ? "eglCreatePlatformWindowSurface(wl_egl_window)"
                                       : "eglCreateWindowSurface",
                         (unsigned)(uintptr_t)r->eglGetError());
                return false;
        }
        if (!r->eglMakeCurrent(r->dpy, r->win[0].surf, r->win[0].surf, r->ctx)) {
                snprintf(err, errcap, "eglMakeCurrent failed (0x%x)", (unsigned)(uintptr_t)r->eglGetError());
                return false;
        }
        r->eglSwapInterval(r->dpy, 1);

        /* GL function table */
        *(void **)&g->eglGetProcAddress = dlsym(r->lib, "eglGetProcAddress");
        if (!g->eglGetProcAddress) {
                snprintf(err, errcap, "eglGetProcAddress missing");
                return false;
        }
        LOADGL(glActiveTexture, "glActiveTexture");
        LOADGL(glBindTexture, "glBindTexture");
        LOADGL(glGenTextures, "glGenTextures");
        LOADGL(glDeleteTextures, "glDeleteTextures");
        LOADGL(glTexImage2D, "glTexImage2D");
        LOADGL(glTexSubImage2D, "glTexSubImage2D");
        LOADGL(glTexParameteri, "glTexParameteri");
        LOADGL(glGenFramebuffers, "glGenFramebuffers");
        LOADGL(glDeleteFramebuffers, "glDeleteFramebuffers");
        LOADGL(glBindFramebuffer, "glBindFramebuffer");
        LOADGL(glFramebufferTexture2D, "glFramebufferTexture2D");
        LOADGL(glCheckFramebufferStatus, "glCheckFramebufferStatus");
        LOADGL(glCreateShader, "glCreateShader");
        LOADGL(glShaderSource, "glShaderSource");
        LOADGL(glCompileShader, "glCompileShader");
        LOADGL(glDeleteShader, "glDeleteShader");
        LOADGL(glGetShaderiv, "glGetShaderiv");
        LOADGL(glGetShaderInfoLog, "glGetShaderInfoLog");
        LOADGL(glCreateProgram, "glCreateProgram");
        LOADGL(glAttachShader, "glAttachShader");
        LOADGL(glLinkProgram, "glLinkProgram");
        LOADGL(glDeleteProgram, "glDeleteProgram");
        LOADGL(glGetProgramiv, "glGetProgramiv");
        LOADGL(glGetProgramInfoLog, "glGetProgramInfoLog");
        LOADGL(glUseProgram, "glUseProgram");
        LOADGL(glGetUniformLocation, "glGetUniformLocation");
        LOADGL(glUniform1i, "glUniform1i");
        LOADGL(glUniform1f, "glUniform1f");
        LOADGL(glUniform2f, "glUniform2f");
        LOADGL(glUniform1fv, "glUniform1fv");
        LOADGL(glDrawArrays, "glDrawArrays");
        LOADGL(glClearColor, "glClearColor");
        LOADGL(glClear, "glClear");
        LOADGL(glViewport, "glViewport");
        LOADGL(glDisable, "glDisable");
        LOADGL(glGetError, "glGetError");
        LOADGL(glGetString, "glGetString");
        LOADGL(glReadPixels, "glReadPixels");

        g->glDisable(0x0BE2 /* GL_BLEND */);
        r->gl_loaded = true;

        const char *rend = (const char *)g->glGetString(GL_RENDERER);
        if (rend && (strstr(rend, "llvmpipe") || strstr(rend, "softpipe") || strstr(rend, "swrast"))) {
                r->software = true;
                LOGI("opengl: software rasterizer active (%s) — llvmpipe tier", rend);
        } else {
                LOGI("opengl: renderer %s", rend ? rend : "?");
        }

        /* programs */
        if (!gl_make_program(r, PROG_PREMULT, FS_PREMULT, err, errcap) ||
            !gl_make_program(r, PROG_COPY, FS_COPY, err, errcap) ||
            !gl_make_program(r, PROG_BLUR, FS_BLUR, err, errcap) ||
            !gl_make_program(r, PROG_COMPOSITE, FS_COMPOSITE, err, errcap) ||
            !gl_make_program(r, PROG_PRESENT, FS_PRESENT, err, errcap))
                return false;

        /* uniform locations + static sampler bindings */
        for (int i = 0; i < PROG_COUNT; i++) {
                GLuint_ pg = r->prog[i];
                g->glUseProgram(pg);
                int lt = g->glGetUniformLocation(pg, "u_tex");
                if (lt >= 0)
                        g->glUniform1i(lt, 0);
                int ls = g->glGetUniformLocation(pg, "u_src");
                if (ls >= 0)
                        g->glUniform1i(ls, 0);
                int ld = g->glGetUniformLocation(pg, "u_dst");
                if (ld >= 0)
                        g->glUniform1i(ld, 1);
                r->u[i].off = g->glGetUniformLocation(pg, "u_off");
                r->u[i].scale = g->glGetUniformLocation(pg, "u_scale");
                r->u[i].dir = g->glGetUniformLocation(pg, "u_dir");
                r->u[i].weights = g->glGetUniformLocation(pg, "u_w[0]");
                r->u[i].blend = g->glGetUniformLocation(pg, "u_blend");
                r->u[i].opacity = g->glGetUniformLocation(pg, "u_opacity");
        }

        /* create remaining window surfaces */
        for (int i = 1; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                if (p->native_handle(p, i))
                        gl_window_surface(r, i);
        }

        return true;
}

static bool gl_make_program(RenderGL *r, int idx, const char *fs, char *err, size_t errcap)
{
        GL *g = &r->g;
        GLuint_ v = g->glCreateShader(GL_VERTEX_SHADER);
        GLuint_ f = g->glCreateShader(GL_FRAGMENT_SHADER);
        const char *vsrc = VS, *fsrc = fs;
        g->glShaderSource(v, 1, &vsrc, NULL);
        g->glShaderSource(f, 1, &fsrc, NULL);
        g->glCompileShader(v);
        g->glCompileShader(f);
        GLint_ ok = 0;
        g->glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
        if (!ok) {
                char log[512];
                GLsizei_ n = 0;
                g->glGetShaderInfoLog(v, sizeof log, &n, log);
                snprintf(err, errcap, "vertex shader: %.*s", (int)n, log);
                g->glDeleteShader(v);
                g->glDeleteShader(f);
                return false;
        }
        g->glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
        if (!ok) {
                char log[512];
                GLsizei_ n = 0;
                g->glGetShaderInfoLog(f, sizeof log, &n, log);
                snprintf(err, errcap, "fragment shader: %.*s", (int)n, log);
                g->glDeleteShader(v);
                g->glDeleteShader(f);
                return false;
        }
        GLuint_ pg = g->glCreateProgram();
        g->glAttachShader(pg, v);
        g->glAttachShader(pg, f);
        g->glLinkProgram(pg);
        g->glDeleteShader(v);
        g->glDeleteShader(f);
        g->glGetProgramiv(pg, GL_LINK_STATUS, &ok);
        if (!ok) {
                char log[512];
                GLsizei_ n = 0;
                g->glGetProgramInfoLog(pg, sizeof log, &n, log);
                snprintf(err, errcap, "link: %.*s", (int)n, log);
                g->glDeleteProgram(pg);
                return false;
        }
        r->prog[idx] = pg;
        return true;
}

/* ---- frame ------------------------------------------------------------------ */

static bool gl_begin(RRenderer *rr, int win, u32 w, u32 h)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        GL *g = &r->g;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return false;
        if (!gl_window_surface(r, win))
                return false;
        if (!r->eglMakeCurrent(r->dpy, r->win[win].surf, r->win[win].surf, r->ctx))
                return false;
        r->cur_win = win;
        r->win[win].w = w;
        r->win[win].h = h;
        if (!ensure_target(g, &r->accum[win][0], w, h) ||
            !ensure_target(g, &r->accum[win][1], w, h))
                return false;
        r->accum_cur[win] = 0;
        g->glBindFramebuffer(GL_FRAMEBUFFER, r->accum[win][0].fbo);
        g->glViewport(0, 0, (int)w, (int)h);
        g->glClearColor(0, 0, 0, 0);
        g->glClear(GL_COLOR_BUFFER_BIT);
        return true;
}

/* renders `src_tex` (bound to unit 0) into `dst` FBO using program `pi`,
 * with placement uniforms set for a full-target pass. */
static void pass_full(RenderGL *r, int pi, RTarget *dst)
{
        GL *g = &r->g;
        g->glBindFramebuffer(GL_FRAMEBUFFER, dst ? dst->fbo : 0);
        g->glViewport(0, 0, dst ? (int)dst->w : (int)r->win[r->cur_win].w,
                      dst ? (int)dst->h : (int)r->win[r->cur_win].h);
        g->glUseProgram(r->prog[pi]);
        if (r->u[pi].off >= 0)
                g->glUniform2f(r->u[pi].off, 0, 0);
        if (r->u[pi].scale >= 0)
                g->glUniform2f(r->u[pi].scale, 1, 1);
        draw_full(g);
}

static bool gl_layer(RRenderer *rr, const RLayerCmd *cmd)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        GL *g = &r->g;
        if (!cmd || !cmd->px || !cmd->w || !cmd->h)
                return false;

        /* 1. upload (straight RGBA). Sub-frame-resolution sims come in small
         *    and let LINEAR sampling do the upscale during placement. */
        if (!ensure_tex(g, &r->tex_src, &r->src_w, &r->src_h, cmd->w, cmd->h, cmd->linear))
                return false;
        g->glBindTexture(GL_TEXTURE_2D, r->tex_src);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                           cmd->linear ? GL_LINEAR : GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                           cmd->linear ? GL_LINEAR : GL_NEAREST);
        g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cmd->w, (int)cmd->h,
                           GL_RGBA, GL_UNSIGNED_BYTE, cmd->px);

        /* 2. premultiply into tex_a (raster size) */
        if (!ensure_tex(g, &r->tex_a, &r->a_w, &r->a_h, cmd->w, cmd->h, true))
                return false;
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, r->tex_src);

        /* scratch FBOs reuse the a/b/c/d textures */
        if (!r->fbo_made) {
                g->glGenFramebuffers(4, r->scratch_fbo);
                r->fbo_made = true;
        }
        /* attach current tex_a to fbo 0 (sizes may have changed) */
        g->glBindFramebuffer(GL_FRAMEBUFFER, r->scratch_fbo[0]);
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex_a, 0);
        g->glBindFramebuffer(GL_FRAMEBUFFER, r->accum[r->cur_win][r->accum_cur[r->cur_win]].fbo);
        {
                RTarget t = { r->tex_a, r->scratch_fbo[0], cmd->w, cmd->h };
                pass_full(r, PROG_PREMULT, &t);
        }

        /* 3. blur (downsample + separable 9-tap + upscale) */
        GLuint_ layer_tex = r->tex_a;
        if (cmd->blur >= 0.5) {
                double sigma = cmd->blur;
                int k = 0;
                while (sigma / (double)(1 << k) > 4.0 && k < 6)
                        k++;
                double sloc = sigma / (double)(1 << k);
                u32 dw = cmd->w >> k, dh = cmd->h >> k;
                if (dw < 1) dw = 1;
                if (dh < 1) dh = 1;
                int spacing = (int)(sloc / 2.0 + 0.5);
                if (spacing < 1) spacing = 1;
                float w9[9];
                gauss9(sloc / (double)spacing, w9);

                if (!ensure_tex(g, &r->tex_b, &r->b_w, &r->b_h, dw, dh, true) ||
                    !ensure_tex(g, &r->tex_c, &r->c_w, &r->c_h, dw, dh, true) ||
                    !ensure_tex(g, &r->tex_d, &r->d_w, &r->d_h, cmd->w, cmd->h, true))
                        return false;
                g->glBindFramebuffer(GL_FRAMEBUFFER, r->scratch_fbo[1]);
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex_b, 0);
                g->glBindFramebuffer(GL_FRAMEBUFFER, r->scratch_fbo[2]);
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex_c, 0);
                g->glBindFramebuffer(GL_FRAMEBUFFER, r->scratch_fbo[3]);
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex_d, 0);

                /* H: tex_a -> tex_b (at dw x dh; linear sampling downscales) */
                g->glBindFramebuffer(GL_FRAMEBUFFER, r->scratch_fbo[1]);
                g->glViewport(0, 0, (int)dw, (int)dh);
                g->glUseProgram(r->prog[PROG_BLUR]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, r->tex_a);
                if (r->u[PROG_BLUR].off >= 0)
                        g->glUniform2f(r->u[PROG_BLUR].off, 0, 0);
                if (r->u[PROG_BLUR].scale >= 0)
                        g->glUniform2f(r->u[PROG_BLUR].scale, 1, 1);
                g->glUniform2f(r->u[PROG_BLUR].dir,
                               (float)((double)spacing / (double)cmd->w), 0.0f);
                g->glUniform1fv(r->u[PROG_BLUR].weights, 9, w9);
                draw_full(g);

                /* V: tex_b -> tex_c */
                g->glBindFramebuffer(GL_FRAMEBUFFER, r->scratch_fbo[2]);
                g->glViewport(0, 0, (int)dw, (int)dh);
                g->glBindTexture(GL_TEXTURE_2D, r->tex_b);
                g->glUniform2f(r->u[PROG_BLUR].dir, 0.0f,
                               (float)((double)spacing / (double)cmd->h));
                draw_full(g);

                /* upscale: tex_c -> tex_d (raster size) */
                g->glBindFramebuffer(GL_FRAMEBUFFER, r->scratch_fbo[3]);
                g->glViewport(0, 0, (int)cmd->w, (int)cmd->h);
                g->glUseProgram(r->prog[PROG_COPY]);
                g->glBindTexture(GL_TEXTURE_2D, r->tex_c);
                if (r->u[PROG_COPY].off >= 0)
                        g->glUniform2f(r->u[PROG_COPY].off, 0, 0);
                if (r->u[PROG_COPY].scale >= 0)
                        g->glUniform2f(r->u[PROG_COPY].scale, 1, 1);
                draw_full(g);

                layer_tex = r->tex_d;
        }

        /* 4. composite: read accum[cur], write accum[cur^1], flip.
         * Placement may exceed the texture (sub-frame-res sims stretched
         * over the frame) — the linear filter upsamples while compositing. */
        u32 pw = cmd->place_w ? cmd->place_w : cmd->w;
        u32 ph = cmd->place_h ? cmd->place_h : cmd->h;
        int wi = r->cur_win;
        RTarget *accum_rd = &r->accum[wi][r->accum_cur[wi]];
        RTarget *accum_wr = &r->accum[wi][r->accum_cur[wi] ^ 1];
        g->glBindFramebuffer(GL_FRAMEBUFFER, accum_wr->fbo);
        g->glViewport(0, 0, (int)accum_wr->w, (int)accum_wr->h);
        g->glUseProgram(r->prog[PROG_COMPOSITE]);
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, layer_tex);
        g->glActiveTexture(GL_TEXTURE1);
        g->glBindTexture(GL_TEXTURE_2D, accum_rd->tex);
        g->glActiveTexture(GL_TEXTURE0);
        if (r->u[PROG_COMPOSITE].off >= 0)
                g->glUniform2f(r->u[PROG_COMPOSITE].off,
                               (float)((double)cmd->dx / (double)accum_wr->w),
                               (float)((double)cmd->dy / (double)accum_wr->h));
        if (r->u[PROG_COMPOSITE].scale >= 0)
                g->glUniform2f(r->u[PROG_COMPOSITE].scale,
                               (float)((double)pw / (double)accum_wr->w),
                               (float)((double)ph / (double)accum_wr->h));
        if (r->u[PROG_COMPOSITE].blend >= 0)
                g->glUniform1i(r->u[PROG_COMPOSITE].blend, cmd->blend);
        if (r->u[PROG_COMPOSITE].opacity >= 0)
                g->glUniform1f(r->u[PROG_COMPOSITE].opacity,
                               (float)(cmd->opacity < 0 ? 0 : cmd->opacity > 1 ? 1 : cmd->opacity));
        draw_full(g);
        r->accum_cur[wi] ^= 1;
        return true;
}

static bool gl_end(RRenderer *rr, const RFb *cpu_frame)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        GL *g = &r->g;
        int win = r->cur_win;

        if (cpu_frame) {
                /* upload the CPU-composited frame and present it directly */
                if (!ensure_tex(g, &r->tex_src, &r->src_w, &r->src_h, cpu_frame->w, cpu_frame->h, false))
                        return false;
                g->glBindTexture(GL_TEXTURE_2D, r->tex_src);
                g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cpu_frame->w, (int)cpu_frame->h,
                                   GL_RGBA, GL_UNSIGNED_BYTE, cpu_frame->px);
                g->glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g->glViewport(0, 0, (int)r->win[win].w, (int)r->win[win].h);
                g->glUseProgram(r->prog[PROG_PRESENT]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, r->tex_src);
                if (r->u[PROG_PRESENT].off >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].off, 0, 0);
                if (r->u[PROG_PRESENT].scale >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].scale, 1, 1);
                draw_full(g);
        } else {
                /* present the latest accum (premul -> straight) */
                RTarget *accum = &r->accum[win][r->accum_cur[win]];
                g->glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g->glViewport(0, 0, (int)r->win[win].w, (int)r->win[win].h);
                g->glUseProgram(r->prog[PROG_PRESENT]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, accum->tex);
                if (r->u[PROG_PRESENT].off >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].off, 0, 0);
                if (r->u[PROG_PRESENT].scale >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].scale, 1, 1);
                draw_full(g);
        }
        return r->eglSwapBuffers(r->dpy, r->win[win].surf) != 0;
}

static void gl_resize(RRenderer *rr, int win, u32 w, u32 h)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return;
        if (r->is_wayland && r->win[win].eglwin && r->wl_egl_window_resize) {
                /* the wl_surface (and thus the EGL surface) survives the
                 * resize — only the wl_egl_window's buffer geometry moves.
                 * Accumulation targets re-allocate via ensure_target. */
                r->wl_egl_window_resize(r->win[win].eglwin, (int)w, (int)h, 0, 0);
                r->win[win].w = w;
                r->win[win].h = h;
                return;
        }
        /* X11: surface is re-created lazily on next begin() when the native
         * handle changes; accum is re-allocated by ensure_target on size
         * change. */
        if (r->win[win].surf) {
                r->eglDestroySurface(r->dpy, r->win[win].surf);
                r->win[win].surf = NULL;
                r->win[win].native = 0;
        }
}

static void gl_fini(RRenderer *rr)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        GL *g = &r->g;
        /* GL teardown only when the function table was loaded */
        if (r->gl_loaded) {
                for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++)
                        if (r->win[i].surf && r->eglDestroySurface)
                                r->eglDestroySurface(r->dpy, r->win[i].surf);
                for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                        /* hand the wl_surface back before the wrapper dies */
                        if (r->win[i].eglwin) {
                                wlshim_set_gl_surface(i, false);
                                if (r->wl_egl_window_destroy)
                                        r->wl_egl_window_destroy(r->win[i].eglwin);
                                r->win[i].eglwin = NULL;
                        }
                }
                if (r->ctx && r->eglMakeCurrent)
                        r->eglMakeCurrent(r->dpy, NULL, NULL, NULL);
                for (int i = 0; i < PROG_COUNT; i++)
                        if (r->prog[i])
                                g->glDeleteProgram(r->prog[i]);
                g->glDeleteTextures(1, &r->tex_src);
                g->glDeleteTextures(1, &r->tex_a);
                g->glDeleteTextures(1, &r->tex_b);
                g->glDeleteTextures(1, &r->tex_c);
                g->glDeleteTextures(1, &r->tex_d);
                for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                        g->glDeleteTextures(1, &r->accum[i][0].tex);
                        g->glDeleteFramebuffers(1, &r->accum[i][0].fbo);
                        g->glDeleteTextures(1, &r->accum[i][1].tex);
                        g->glDeleteFramebuffers(1, &r->accum[i][1].fbo);
                }
        }
        if (r->wegl_lib)
                dlclose(r->wegl_lib);
        if (r->lib)
                dlclose(r->lib);
        free(r);
}

RRenderer *render_gl_create(void)
{
        RRenderer *rr = (RRenderer *)calloc(1, sizeof *rr);
        RenderGL *r = (RenderGL *)calloc(1, sizeof *r);
        if (!rr || !r) {
                free(rr);
                free(r);
                return NULL;
        }
        rr->name = "opengl";
        rr->tier = 2;
        rr->wants_layers = true;
        rr->ctx = r;
        rr->init = gl_init;
        rr->fini = gl_fini;
        rr->begin = gl_begin;
        rr->layer = gl_layer;
        rr->end = gl_end;
        rr->resize = gl_resize;
        return rr;
}
