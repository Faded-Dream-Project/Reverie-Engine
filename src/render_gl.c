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
#include "util.h"
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
#define GL_POINTS 0x0000
#define GL_TEXTURE_2D 0x0DE1
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
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
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
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
typedef unsigned long long GLuint64_;

/* ---- EXT_disjoint_timer_query (optional: real GPU execution timings) ------
 * Queried through eglGetProcAddress, both EXT-suffixed (the spec name for
 * ES contexts) and plain desktop names. GPU work is ASYNCHRONOUS: results
 * are polled one frame late and never force a flush or finish. */
#define GL_TIME_ELAPSED_Q 0x88BF
#define GL_QUERY_RESULT_Q 0x8866
#define GL_QUERY_RESULT_AVAILABLE_Q 0x8867
#define GL_EXTENSIONS_Q 0x1F03

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
        void (*glPixelStorei)(GLenum_, GLint_);
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
        void (*glUniform3f)(GLint_, GLfloat_, GLfloat_, GLfloat_);
        void (*glUniform1fv)(GLint_, GLsizei_, const GLfloat_ *);
        void (*glUniform2fv)(GLint_, GLsizei_, const GLfloat_ *);
        void (*glUniform4fv)(GLint_, GLsizei_, const GLfloat_ *);
        void (*glDrawArrays)(GLenum_, GLint_, GLsizei_);
        void (*glClearColor)(GLfloat_, GLfloat_, GLfloat_, GLfloat_);
        void (*glClear)(GLenum_);
        void (*glViewport)(GLint_, GLint_, GLsizei_, GLsizei_);
        void (*glDisable)(GLenum_);
        void (*glEnable)(GLenum_);
        void (*glBlendFuncSeparate)(GLenum_, GLenum_, GLenum_, GLenum_);
        GLenum_ (*glGetError)(void);
        void (*glReadPixels)(GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, void *);
        const GLchar_ *(*glGetString)(GLenum_);

        /* EXT_disjoint_timer_query (optional) */
        void (*glGenQueriesQ)(GLsizei_, GLuint_ *);
        void (*glBeginQueryQ)(GLenum_, GLuint_);
        void (*glEndQueryQ)(GLenum_);
        void (*glGetQueryObjectu64vQ)(GLuint_, GLenum_, GLuint64_ *);
        void (*glDeleteQueriesQ)(GLsizei_, const GLuint_ *);
        void (*glFinishQ)(void);
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

/* W3C blend of a layer over the accum — mirrors composite.c blend_pixel()
 * EXACTLY (straight-alpha math, STRAIGHT storage — like the CPU fb, so
 * the present pass is a plain passthrough with no un-premultiply divide
 * amplifying 8-bit quantization on faint pixels). `u_src_premul` selects
 * the source encoding: 0 = straight (uploads, GPU fluid tonemap), 1 =
 * premultiplied (the blur chain's output). */
static const char *FS_COMPOSITE =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D u_src;\n"
        "uniform sampler2D u_dst;\n"
        "uniform int u_blend;\n"
        "uniform int u_src_premul;\n"
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
        "    vec3 cs = (u_src_premul == 1 && sp.a > 0.0) ? sp.rgb / sp.a : sp.rgb;\n"
        "    vec3 cd = dp.rgb;\n"
        "    float ao = as + ad * (1.0 - as);\n"
        "    if (ao <= 0.0) { fragColor = vec4(0.0); return; }\n"
        "    float k1 = as * (1.0 - ad);\n"
        "    float k3 = (1.0 - as) * ad;\n"
        "    vec3 b = blend_fn(u_blend, cd, cs);\n"
        "    vec3 co = (k1 * cs + k3 * cd + as * ad * b) / ao;\n"
        "    fragColor = vec4(clamp(co, 0.0, 1.0), ao);\n"
        "}\n";

/* present: passthrough of the STRAIGHT-alpha accum (the composite stores
 * straight, exactly like the CPU renderer's framebuffer). v flip: GL window
 * space is y-up, CPU raster rows are y-down — sampling with u_scale=(1,-1),
 * u_off=(0,1) lands row 0 at the TOP of the surface, matching the CPU tier
 * (verified pixel-wise against -R cpu screenshots; without it every GL
 * frame presents upside down). */
static const char *FS_PRESENT =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D u_tex;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() { fragColor = texture(u_tex, v_uvsrc); }\n";

/* ---- GPU fluid solver (GLSL ES 3.00) -------------------------------------
 * Mirrors fluid.c's Stam solver: splats + ambient drift, vorticity
 * confinement, Jacobi pressure projection (the CPU's Gauss-Seidel is
 * sequential; Jacobi is the parallel equivalent), semi-Lagrangian
 * advection with dissipation folded in, and the same tonemap curve as
 * fluid_render()'s LUT (a = 1 - exp(-lum*2.2), k = a/lum). Fields live in
 * RGBA16F ping-pong textures; the CPU never touches a per-pixel loop. */
static const char *FS_FLUID_VELF =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_vel;\n"
        "uniform vec2 u_grid;\n"          /* velocity grid (gw, gh)        */
        "uniform vec2 u_posA;\n"          /* splat centers (uv)            */
        "uniform vec2 u_impA;\n"          /* splat impulses (cells/s)      */
        "uniform vec2 u_posB;\n"
        "uniform vec2 u_impB;\n"
        "uniform float u_radA;\n"         /* radius, grid texels           */
        "uniform float u_radB;\n"
        "uniform float u_ambient;\n"
        "uniform float u_dt;\n"
        "uniform float u_t;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "float splat(vec2 uv, vec2 pos, float rad) {\n"
        "    vec2 d = (uv - pos) * u_grid;\n"
        "    return exp(-dot(d, d) / (rad * rad) * 3.0);\n"
        "}\n"
        "void main() {\n"
        "    vec2 vel = texture(u_vel, v_uvsrc).xy;\n"
        "    vel += u_impA * splat(v_uvsrc, u_posA, u_radA);\n"
        "    vel += u_impB * splat(v_uvsrc, u_posB, u_radB);\n"
        "    if (u_ambient > 0.0) {\n"
        "        vec2 n = v_uvsrc;\n"
        "        float fx = sin(n.y * 6.0 + u_t * 0.35) * cos(n.x * 4.0 - u_t * 0.22);\n"
        "        float fy = cos(n.x * 5.0 + u_t * 0.3) * sin(n.y * 5.0 + u_t * 0.18);\n"
        "        vec2 t = floor(v_uvsrc * u_grid);\n"
        "        bool on = mod(t.x - 1.0, 3.0) < 1.0 && mod(t.y - 1.0, 3.0) < 1.0;\n"
        "        if (on)\n"
        "            vel += vec2(fx, fy) * (u_ambient * u_dt * 8.0);\n"
        "    }\n"
        "    fragColor = vec4(vel, 0.0, 1.0);\n"
        "}\n";

static const char *FS_FLUID_DYEF =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_dye;\n"
        "uniform vec2 u_grid;\n"          /* dye grid (dw, dh)            */
        "uniform vec2 u_posA;\n"
        "uniform vec2 u_posB;\n"
        "uniform vec3 u_colA;\n"
        "uniform vec3 u_colB;\n"
        "uniform float u_radA;\n"         /* radius, dye texels           */
        "uniform float u_radB;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "float splat(vec2 uv, vec2 pos, float rad) {\n"
        "    vec2 d = (uv - pos) * u_grid;\n"
        "    return exp(-dot(d, d) / (rad * rad) * 3.0);\n"
        "}\n"
        "void main() {\n"
        "    vec3 dye = texture(u_dye, v_uvsrc).rgb;\n"
        "    dye += u_colA * (splat(v_uvsrc, u_posA, u_radA) * 0.9);\n"
        "    dye += u_colB * (splat(v_uvsrc, u_posB, u_radB) * 0.9);\n"
        "    fragColor = vec4(dye, 1.0);\n"
        "}\n";

static const char *FS_FLUID_CURL =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_vel;\n"
        "uniform vec2 u_grid;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 r = texture(u_vel, v_uvsrc + vec2(1.0 / u_grid.x, 0.0)).xy;\n"
        "    vec2 l = texture(u_vel, v_uvsrc - vec2(1.0 / u_grid.x, 0.0)).xy;\n"
        "    vec2 u = texture(u_vel, v_uvsrc + vec2(0.0, 1.0 / u_grid.y)).xy;\n"
        "    vec2 d = texture(u_vel, v_uvsrc - vec2(0.0, 1.0 / u_grid.y)).xy;\n"
        "    fragColor = vec4(vec2((r.y - l.y) - (u.x - d.x)) * 0.5, 0.0, 1.0);\n"
        "}\n";

static const char *FS_FLUID_VORT =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_vel;\n"
        "uniform sampler2D u_curl;\n"
        "uniform vec2 u_grid;\n"
        "uniform float u_k;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 vel = texture(u_vel, v_uvsrc).xy;\n"
        "    float c = texture(u_curl, v_uvsrc).x;\n"
        "    float xr = abs(texture(u_curl, v_uvsrc + vec2(1.0 / u_grid.x, 0.0)).x);\n"
        "    float xl = abs(texture(u_curl, v_uvsrc - vec2(1.0 / u_grid.x, 0.0)).x);\n"
        "    float yu = abs(texture(u_curl, v_uvsrc + vec2(0.0, 1.0 / u_grid.y)).x);\n"
        "    float yd = abs(texture(u_curl, v_uvsrc - vec2(0.0, 1.0 / u_grid.y)).x);\n"
        "    vec2 g = vec2((xr - xl) * 0.5, (yu - yd) * 0.5);\n"
        "    vec2 n = g / (length(g) + 1e-9);\n"
        "    fragColor = vec4(vel + vec2(u_k * n.y * c, -u_k * n.x * c), 0.0, 1.0);\n"
        "}\n";

static const char *FS_FLUID_DIV =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_vel;\n"
        "uniform vec2 u_grid;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 r = texture(u_vel, v_uvsrc + vec2(1.0 / u_grid.x, 0.0)).xy;\n"
        "    vec2 l = texture(u_vel, v_uvsrc - vec2(1.0 / u_grid.x, 0.0)).xy;\n"
        "    vec2 u = texture(u_vel, v_uvsrc + vec2(0.0, 1.0 / u_grid.y)).xy;\n"
        "    vec2 d = texture(u_vel, v_uvsrc - vec2(0.0, 1.0 / u_grid.y)).xy;\n"
        "    fragColor = vec4(0.0, -0.5 * ((r.x - l.x) + (u.y - d.y)), 0.0, 1.0);\n"
        "}\n";

static const char *FS_FLUID_JACOBI =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_press;\n"     /* .x = p, .y = div            */
        "uniform vec2 u_grid;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    float dv = texture(u_press, v_uvsrc).y;\n"
        "    float pl = texture(u_press, v_uvsrc - vec2(1.0 / u_grid.x, 0.0)).x;\n"
        "    float pr = texture(u_press, v_uvsrc + vec2(1.0 / u_grid.x, 0.0)).x;\n"
        "    float pd = texture(u_press, v_uvsrc - vec2(0.0, 1.0 / u_grid.y)).x;\n"
        "    float pu = texture(u_press, v_uvsrc + vec2(0.0, 1.0 / u_grid.y)).x;\n"
        "    fragColor = vec4((dv + pl + pr + pd + pu) * 0.25, dv, 0.0, 1.0);\n"
        "}\n";

static const char *FS_FLUID_GRADSUB =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_vel;\n"
        "uniform sampler2D u_press;\n"
        "uniform vec2 u_grid;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 vel = texture(u_vel, v_uvsrc).xy;\n"
        "    float pl = texture(u_press, v_uvsrc - vec2(1.0 / u_grid.x, 0.0)).x;\n"
        "    float pr = texture(u_press, v_uvsrc + vec2(1.0 / u_grid.x, 0.0)).x;\n"
        "    float pd = texture(u_press, v_uvsrc - vec2(0.0, 1.0 / u_grid.y)).x;\n"
        "    float pu = texture(u_press, v_uvsrc + vec2(0.0, 1.0 / u_grid.y)).x;\n"
        "    fragColor = vec4(vel - vec2((pr - pl) * 0.5, (pu - pd) * 0.5), 0.0, 1.0);\n"
        "}\n";

static const char *FS_FLUID_ADVECT_VEL =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_vel;\n"
        "uniform vec2 u_grid;\n"
        "uniform float u_dt;\n"
        "uniform float u_damp;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 vel = texture(u_vel, v_uvsrc).xy;\n"
        "    vec2 back = v_uvsrc - vel * u_dt / (u_grid - vec2(1.0));\n"
        "    fragColor = vec4(texture(u_vel, back).xy * u_damp, 0.0, 1.0);\n"
        "}\n";

static const char *FS_FLUID_ADVECT_DYE =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_dye;\n"
        "uniform sampler2D u_vel;\n"
        "uniform vec2 u_grid;\n"          /* velocity grid — dye and     */
        "uniform float u_dt;\n"            /* velocity share normalized   */
        "uniform float u_fade;\n"          /* uv space                    */
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 vel = texture(u_vel, v_uvsrc).xy;\n"
        "    vec2 back = v_uvsrc - vel * u_dt / (u_grid - vec2(1.0));\n"
        "    fragColor = vec4(texture(u_dye, back).rgb * u_fade, 1.0);\n"
        "}\n";

static const char *FS_FLUID_TONEMAP =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_dye;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec3 d = texture(u_dye, v_uvsrc).rgb;\n"
        "    float lum = max(d.r, max(d.g, d.b));\n"
        "    float a = 1.0 - exp(-lum * 2.2);\n"
        "    if (a <= 0.003) { fragColor = vec4(0.0); return; }\n"
        "    float k = a / lum;\n"
        "    fragColor = vec4(clamp(d * k, 0.0, 1.0), a);\n"
        "}\n";

/* ---- GPU ripple solver (GLSL ES 3.00) ------------------------------------
 * Mirrors ripple.c's wave equation: the height field lives in an RGBA16F
 * ping-pong pair (.r = current height, .g = previous height). The pointer
 * drip and queued click impacts are applied by a SEPARATE SPLAT PASS that
 * writes them into the field FIRST — exactly the order ripple_step()
 * applies them (ripple_click()/drip mutate cur, THEN the update loop runs).
 *
 * RIPPLE-LAKE BUG (2026-10): the old single-pass version folded the
 * injections into the update pass as LOCAL variables while sampling the
 * neighbors from the UN-injected texture, so the Laplacian mixed
 * post-injection (center) with pre-injection (neighbors) values:
 * for a click of depth d, lap = 0 - 4d = -4d, and
 * nh = 2d + 0.24*(-4d) = 1.04d — the 2x launch cancelled, the wave never
 * propagated, and the visible effect was a weak static blob (a diffusion
 *-like smear with a displaced specular) instead of an expanding ring.
 * Verified: probe measured nh = -2.484 for a -2.4 click; the fixed
 * pipeline produces the CPU's -4.8. */
static const char *FS_RIPPLE_SPLAT =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_field;\n"
        "uniform vec2 u_grid;\n"           /* gw, gh                      */
        "uniform vec2 u_ptr;\n"            /* pointer uv                 */
        "uniform float u_ptrRad;\n"        /* radius * gw, grid texels   */
        "uniform float u_ptrAmp;\n"        /* -strength                  */
        "uniform int u_ptrOn;\n"
        "uniform vec2 u_clickPos[4];\n"    /* impact centers, uv         */
        "uniform vec2 u_clickParam[4];\n"  /* x: radius (texels), y: amp */
        "uniform int u_nclicks;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "float splat(vec2 uv, vec2 pos, float rad) {\n"
        "    vec2 d = (uv - pos) * u_grid;\n"
        "    float d2 = dot(d, d) / (rad * rad + 1e-9);\n"
        "    return d2 <= 1.0 ? (1.0 - d2) : 0.0;\n"
        "}\n"
        "void main() {\n"
        "    vec4 f = texture(u_field, v_uvsrc);\n"
        "    float h = f.r;\n"
        "    if (u_ptrOn == 1)\n"
        "        h += u_ptrAmp * splat(v_uvsrc, u_ptr, u_ptrRad);\n"
        "    for (int i = 0; i < 4; i++) {\n"
        "        if (i >= u_nclicks) break;\n"
        "        /* dropped stone: (1 - d^2)^2 falloff, amp -strength*1.5 */\n"
        "        vec2 d = (v_uvsrc - u_clickPos[i]) * u_grid;\n"
        "        float r2 = dot(d, d) / (u_clickParam[i].x * u_clickParam[i].x + 1e-9);\n"
        "        if (r2 <= 1.0) {\n"
        "            float t = 1.0 - r2;\n"
        "            h += u_clickParam[i].y * t * t;\n"
        "        }\n"
        "    }\n"
        "    /* .g (previous height) passes through untouched */\n"
        "    fragColor = vec4(h, f.g, 0.0, 1.0);\n"
        "}\n";

/* Pure wave-equation update pass — mirrors ripple_step()'s STABLE
 * damped form (see ripple.c's header for the full story: the old
 * -(1-d)*prev formulation was ANTI-damped — pole at 1+sqrt(d) — and
 * blew the field up to inf/NaN ~3 s after any click on RGBA16F).
 * Border texels stay untouched, matching the CPU's 1..gw-2 loop bounds.
 * Every constant matches ripple.c exactly (C2, dv, ga, band, edge, fade,
 * snap) so CPU and GPU fields stay in parity. */
static const char *FS_RIPPLE_STEP =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_field;\n"
        "uniform vec2 u_grid;\n"           /* gw, gh                      */
        "uniform float u_damp;\n"          /* dv = damping * 0.05        */
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 texel = v_uvsrc * u_grid;\n"
        "    vec4 f = texture(u_field, v_uvsrc);\n"
        "    /* border: the CPU loops y/x in 1..gw-2 - edge cells never change */\n"
        "    if (texel.x < 1.0 || texel.y < 1.0 ||\n"
        "        texel.x > u_grid.x - 2.0 || texel.y > u_grid.y - 2.0) {\n"
        "        fragColor = f;\n"
        "        return;\n"
        "    }\n"
        "    float h = f.r;\n"
        "    float hl = texture(u_field, v_uvsrc - vec2(1.0 / u_grid.x, 0.0)).r;\n"
        "    float hr = texture(u_field, v_uvsrc + vec2(1.0 / u_grid.x, 0.0)).r;\n"
        "    float hu = texture(u_field, v_uvsrc + vec2(0.0, 1.0 / u_grid.y)).r;\n"
        "    float hd = texture(u_field, v_uvsrc - vec2(0.0, 1.0 / u_grid.y)).r;\n"
        "    float lap = hl + hr + hu + hd - 4.0 * h;\n"
        /* stable damped wave: nh = 2h - prev + c2*lap - dv*(h - prev)
 *      * - ga*h (ga drains the DC/level mode velocity damping leaves) */
        "    float nh = 2.0 * h - f.g + 0.24 * lap - u_damp * (h - f.g)\n"
        "              - 0.0025 * h;\n"
        /* absorbing border band: attx/atty tapers (band 24 cells, edge
 *      * 0.30) — wave energy dies at the screen edges */
        "    vec2 idx = floor(texel);\n"
        "    vec2 dist = min(idx, u_grid - 1.0 - idx);\n"
        "    vec2 band = clamp(dist / 24.0, 0.0, 1.0);\n"
        "    vec2 att = 1.0 - 0.30 * (1.0 - band);\n"
        "    nh *= att.x * att.y;\n"
        /* sub-visible fade + momentum-gated snap (mirrors ripple.c) */
        "    if (abs(nh) < 0.08) nh *= 0.94;\n"
        "    if (abs(nh) < 2.0e-4 && abs(h) < 2.0e-4 && abs(f.g) < 2.0e-4)\n"
        "        nh = 0.0;\n"
        "    fragColor = vec4(nh, h, 0.0, 1.0);\n"   /* new cur; old cur = prev */
        "}\n";

/* Ripple refraction composite — mirrors ripple_apply(): sample the
 * below-composite (accum) offset by the surface-normal gradient, add the
 * upper-left specular glint, then source-over the result onto the accum
 * with the layer opacity. The CPU rounds offsets to whole pixels and
 * samples the height field nearest; the shader uses continuous offsets
 * with linear sampling — smoother, visually equivalent. */
static const char *FS_RIPPLE_COMPOSITE =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_dst;\n"        /* accumulation (below)        */
        "uniform sampler2D u_field;\n"      /* height field                */
        "uniform vec2 u_grid;\n"
        "uniform vec2 u_amp;\n"             /* strength*26 / (w, h)        */
        "uniform float u_opacity;\n"
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 texel = v_screen * u_grid;\n"
        "    /* CPU clamps gradient samples to 1..gw-2 */\n"
        "    vec2 c = clamp(texel, vec2(1.0), u_grid - vec2(2.0));\n"
        "    vec2 base = c / u_grid;\n"
        "    float hl = texture(u_field, base - vec2(1.0 / u_grid.x, 0.0)).r;\n"
        "    float hr = texture(u_field, base + vec2(1.0 / u_grid.x, 0.0)).r;\n"
        "    float hu = texture(u_field, base + vec2(0.0, 1.0 / u_grid.y)).r;\n"
        "    float hd = texture(u_field, base - vec2(0.0, 1.0 / u_grid.y)).r;\n"
        "    float dxn = (hr - hl) * 0.5;\n"
        "    float dyn = (hu - hd) * 0.5;\n"
        "    vec4 dst = texture(u_dst, v_screen);\n"
        "    /* refraction: offset sample of the below-composite */\n"
        "    vec2 suv = clamp(v_screen + vec2(dxn, dyn) * u_amp, vec2(0.0), vec2(1.0));\n"
        "    vec4 p = texture(u_dst, suv);\n"
        "    /* specular highlight: light from the upper-left (ripple_apply) */\n"
        "    float len = sqrt(dxn * dxn + dyn * dyn + 0.25);\n"
        "    float dt = (dyn * -0.7 + 0.5 * 0.7) / len;\n"
        "    float spec = dt > 0.0 ? pow(dt, 24.0) : 0.0;\n"
        "    vec3 rgb = clamp(p.rgb + vec3(spec * (110.0 / 255.0),\n"
        "                                  spec * (112.0 / 255.0),\n"
        "                                  spec * (120.0 / 255.0)), 0.0, 1.0);\n"
        "    /* source-over onto the accum with layer opacity (straight alpha) */\n"
        "    float as = p.a * u_opacity;\n"
        "    float ad = dst.a;\n"
        "    float ao = as + ad * (1.0 - as);\n"
        "    vec3 co = ao > 0.0 ? (as * rgb + (1.0 - as) * ad * dst.rgb) / ao\n"
        "                       : vec3(0.0);\n"
        "    fragColor = vec4(co, ao);\n"
        "}\n";

/* ---- GPU particle layers (GLSL ES 3.00) -----------------------------------
 * Point sprites replacing the full-resolution CPU sprite raster. The CPU
 * stays authoritative for particle STATE (position/velocity/life — the
 * cheap part) and packs live particles into one small RGBA32F texture;
 * this vertex shader places each particle as a gl_Point sized exactly like
 * the CPU sprite (2r+1 px, r = max(1, size*2)) and the fragment shader
 * reproduces the soft coverage falloff cov = (1-d^2)^2 * alpha, with GL
 * source-over blending doing the same straight-alpha math the CPU sprite
 * blit did. Rain's vertical streaks are not point-sprite-shaped and stay
 * on the CPU raster path. */
static const char *VS_PARTICLES =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform highp sampler2D u_parts;\n" /* RGBA32F: band0 pos, band1 col */
        "uniform vec2 u_frame;\n"            /* device pixels                */
        "uniform int u_rows;\n"              /* band rows (slots/1024)       */
        "out vec3 v_col;\n"
        "out float v_alpha;\n"
        "void main() {\n"
        "    int id = gl_VertexID;\n"
        "    ivec2 pa = ivec2(id % 1024, id / 1024);\n"
        "    vec4 pos = texelFetch(u_parts, pa, 0);\n"
        "    vec4 col = texelFetch(u_parts, pa + ivec2(0, u_rows), 0);\n"
        "    vec2 n = pos.xy / u_frame;\n"   /* [0..1], y from the TOP     */
        "    gl_Position = vec4(n.x * 2.0 - 1.0, 1.0 - 2.0 * n.y, 0.0, 1.0);\n"
        "    float r = max(1.0, floor(pos.z * 2.0));\n"
        "    gl_PointSize = min(2.0 * r + 1.0, 63.0);\n"
        "    v_col = col.rgb;\n"
        "    v_alpha = pos.w;\n"
        "}\n";

static const char *FS_PARTICLES =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec3 v_col;\n"
        "in float v_alpha;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 d = gl_PointCoord * 2.0 - 1.0;\n"
        "    float d2 = dot(d, d);\n"        /* |d|=1 at r+0.5 px — the CPU's
                                                  distance normalization     */
        "    if (d2 > 1.0) discard;\n"
        "    float cov = 1.0 - d2;\n"
        "    cov = cov * cov * v_alpha;\n"
        "    if (cov <= 0.02) discard;\n"     /* the CPU skips these pixels */
        "    fragColor = vec4(v_col, cov);\n"
        "}\n";

/* ---- GPU gradient evaluation (GLSL ES 3.00) --------------------------------
 * Mirrors build_lut()/paint_t(): linear t = projection onto the axis,
 * radial t = |P - C| / r (scene radials are concentric), PAD spread, stops
 * sorted with implicit end caps, straight-RGBA channel lerp, AND the same
 * 4x4 ordered dither as the CPU raster (sub-LSB Bayer offset on t —
 * see BAYER4 in raster.c; differences stay under one LUT step). */
static const char *FS_GRADIENT =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform int u_shape;\n"
        "uniform vec2 u_p0;\n"              /* linear axis, pixels       */
        "uniform vec2 u_p1;\n"
        "uniform vec2 u_rc;\n"              /* radial center, pixels     */
        "uniform float u_rr;\n"             /* radial radius, pixels     */
        "uniform vec4 u_cols[17];\n"        /* capped stops (<= 16 + 2)  */
        "uniform float u_offs[17];\n"
        "uniform int u_n;\n"
        "uniform vec2 u_dims;\n"            /* target size, pixels       */
        "in vec2 v_uvsrc;\n"
        "in vec2 v_screen;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 p = v_screen * u_dims;\n" /* pixel centers via v_screen */
        "    float t;\n"
        "    if (u_shape == 0) {\n"
        "        vec2 d = u_p1 - u_p0;\n"
        "        float len2 = dot(d, d);\n"
        "        t = len2 < 1e-18 ? 0.0 : dot(p - u_p0, d) / len2;\n"
        "    } else {\n"
        "        t = u_rr < 1e-12 ? 0.0 : length(p - u_rc) / u_rr;\n"
        "    }\n"
        "    t = clamp(t, 0.0, 1.0);\n"     /* PAD spread                 */
        "    vec4 c = u_cols[0];\n"
        "    for (int k = 1; k < 17; k++) {\n"
        "        if (k >= u_n) break;\n"
        "        if (t <= u_offs[k]) {\n"
        "            float span = u_offs[k] - u_offs[k - 1];\n"
        "            float f = span > 1e-12 ? (t - u_offs[k - 1]) / span : 1.0;\n"
        "            f = clamp(f, 0.0, 1.0);\n"
        "            c = mix(u_cols[k - 1], u_cols[k], f);\n"
        "            break;\n"
        "        }\n"
        "        c = u_cols[k];\n"
        "    }\n"
        "    ivec2 dpix = ivec2(v_screen * u_dims);\n"
        "    int dx = dpix.x & 3, dy = dpix.y & 3;\n"
        "    int db = (((dx & 1) ^ (dy & 1)) << 3) | ((dy & 1) << 2)\n"
        "          | ((((dx >> 1) & 1) ^ ((dy >> 1) & 1)) << 1) | ((dy >> 1) & 1);\n"
        "    c.rgb += vec3((float(db) - 7.5) / 16.0);\n"
        "    fragColor = c;\n"
        "}\n";

/* ---- GPU video composite (GLSL ES 3.00) ------------------------------------
 * YUV420P planes as R8 textures, BT.601 limited-range conversion, then the
 * same W3C blend math as FS_COMPOSITE (video layers participate in the
 * scene's blending like any other layer). Plane upload is 3.1MB at 1080p
 * vs 8.3MB as RGBA — and no CPU color conversion at all. */
static const char *FS_VIDEO =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_y;\n"          /* unit 0                       */
        "uniform sampler2D u_u;\n"          /* unit 1                       */
        "uniform sampler2D u_v;\n"          /* unit 2                       */
        "uniform sampler2D u_dst;\n"        /* unit 3: accumulation         */
        "uniform int u_blend;\n"
        "uniform float u_opacity;\n"
        "in vec2 v_uvsrc;\n"                /* video uv (placed rect)      */
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
        "    float yy = texture(u_y, v_uvsrc).r;\n"
        "    float uu = texture(u_u, v_uvsrc).r - 0.5;\n"
        "    float vv = texture(u_v, v_uvsrc).r - 0.5;\n"
        "    /* BT.601 limited range */\n"
        "    vec3 cs = vec3(1.164 * (yy - 0.0625) + 1.596 * vv,\n"
        "                   1.164 * (yy - 0.0625) - 0.391 * uu - 0.813 * vv,\n"
        "                   1.164 * (yy - 0.0625) + 2.018 * uu);\n"
        "    cs = clamp(cs, 0.0, 1.0);\n"
        "    vec4 dp = texture(u_dst, v_screen);\n"
        "    float as = u_opacity;\n"        /* planes are fully opaque      */
        "    float ad = dp.a;\n"
        "    float ao = as + ad * (1.0 - as);\n"
        "    float k1 = as * (1.0 - ad);\n"
        "    float k3 = (1.0 - as) * ad;\n"
        "    vec3 b = blend_fn(u_blend, dp.rgb, cs);\n"
        "    vec3 co = ao > 0.0 ? (k1 * cs + k3 * dp.rgb + as * ad * b) / ao\n"
        "                       : vec3(0.0);\n"
        "    fragColor = vec4(clamp(co, 0.0, 1.0), ao);\n"
        "}\n";

/* ---- renderer state ------------------------------------------------------- */
enum { PROG_PREMULT, PROG_COPY, PROG_BLUR, PROG_COMPOSITE, PROG_PRESENT,
       PROG_F_VELF, PROG_F_DYEF, PROG_F_CURL, PROG_F_VORT, PROG_F_DIV,
       PROG_F_JACOBI, PROG_F_GRADSUB, PROG_F_ADV_VEL, PROG_F_ADV_DYE,
       PROG_F_TONEMAP, PROG_R_SPLAT, PROG_R_STEP, PROG_R_COMPOSITE,
       PROG_PARTICLES, PROG_GRADIENT, PROG_VIDEO, PROG_COUNT };
#define PROG_BASE_COUNT PROG_F_VELF   /* compiled always                 */

typedef struct {
        GLuint_ tex;
        GLuint_ fbo;
        u32 w, h;
} RTarget;

/* ---- GPU fluid state (one per live fluid layer instance per window) ------ */
#define GL_MAX_FLUIDS 8

typedef struct {
        const RSimFluid *key;    /* scene descriptor address (identity)  */
        GLuint_ vel[2], dye[2], press[2], curl;
        GLuint_ vel_fbo[2], dye_fbo[2], press_fbo[2], curl_fbo;
        GLuint_ out8, out8_fbo;  /* tonemapped RGBA8 layer texture       */
        u32 gw, gh, dw, dh;      /* velocity / dye grid                  */
        int vc, dc, pc;          /* ping-pong current index              */
        f64 amb_t, wisp_acc;     /* CPU-side clocks (match fluid.c)      */
        u64 last_seen;           /* GC stamp (frame counter)             */
        bool ok;                 /* resources valid                      */
} GLFluid;

        /* ---- GPU ripple state (one per live ripple layer per window) ------ */
#define GL_MAX_RIPPLES 8

typedef struct {
        const RSimRipple *key;   /* scene descriptor address (identity)  */
        GLuint_ field[2], fbo[2];
        u32 gw, gh;              /* height field grid                    */
        int cur;                 /* ping-pong current index              */
        u64 last_seen;           /* GC stamp (frame counter)             */
        bool ok;                 /* resources valid                      */
} GLRipple;

/* ---- content-addressed upload cache ---------------------------------------
 * Static layer rasters carry a monotonic content serial (scene.c); a cache
 * slot whose serial matches skips the glTexSubImage2D re-upload — the
 * pre-fix path pushed every cached raster over PCIe again every frame. */
#define GL_TEX_CACHE 16

typedef struct {
        u64 id;                  /* content serial, 0 = uncached frame  */
        u64 stamp;               /* LRU clock                            */
        GLuint_ tex;
        u32 w, h;
} TexSlot;

#define TQ_RING 4

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

        /* GPU particle layer: RGBA32F particle texture (1024 wide, two
         * stacked bands) + the frame-size layer target the sprites blend
         * into before the standard composite pass */
        GLuint_ ptex;
        u32 ptex_h;
        RTarget ptarget;

        /* rendered-gradient cache: shader-evaluated gradient textures keyed
         * by the scene's content serial (static gradients render once) */
        struct {
                u64 id;
                u64 stamp;
                RTarget t;
        } rgcache[8];
        u64 rg_clock;

        /* video plane textures (R8, re-allocated on size change) */
        GLuint_ vtex_y, vtex_u, vtex_v;
        u32 vtex_w;

        /* GPU fluid + ripple solvers: enabled when RGBA16F render targets
         * probe OK AND the rasterizer is hardware (llvmpipe sims would be
         * slower than the CPU solvers — fragment dispatch overhead beats
         * tight C loops there, so software GL keeps the CPU sims). */
        bool sims_ok;
        GLFluid fluids[REV_PLATFORM_MAX_OUTPUTS][GL_MAX_FLUIDS];
        GLRipple ripples[REV_PLATFORM_MAX_OUTPUTS][GL_MAX_RIPPLES];
        u64 frame_no;

        /* content-addressed upload cache (context-global: content serials
         * are globally unique) */
        TexSlot texcache[GL_TEX_CACHE];
        u64 cache_clock;

        GLuint_ prog[PROG_COUNT];
        struct {
                int off, scale, tex;
                int dir, weights;
                int src, dst, blend, opacity, src_premul;
        } u[PROG_COUNT];
        /* fluid program uniforms (indexed like prog[]) */
        struct {
                int vel, dye, press, curl;
                int grid, dt, damp, fade, t, k;
                int posA, posB, impA, impB, radA, radB;
                int colA, colB, ambient;
        } fu[PROG_COUNT];
        /* ripple program uniforms (indexed like prog[]) */
        struct {
                int field, grid, damp, ptr, ptrRad, ptrAmp, ptrOn;
                int clickPos, clickParam, nclicks;
                int amp, opacity;
        } ru[PROG_COUNT];
        /* particle program uniforms */
        struct {
                int frame, rows;
        } pu[PROG_COUNT];
        /* gradient program uniforms */
        struct {
                int shape, p0, p1, rc, rr, cols, offs, n, dims;
        } gu[PROG_COUNT];
        /* video program uniforms */
        struct {
                int y, u, v, dst, blend, opacity;
        } vu[PROG_COUNT];

        RPlatform *plat;
        int cur_win;
        bool software;    /* llvmpipe/softpipe/swrast detected */
        bool gl_loaded;   /* GL function table is valid            */

        /* perf counters (read-and-reset through ->perf):
         * upload wall time is a SUBSET of the caller's render measurement;
         * GPU timings come from one GL_TIME_ELAPSED query per window,
         * polled one frame late — never a glFinish. */
        f64 perf_upload_ms;
        f64 perf_gpu_ms_sum;
        u32 perf_gpu_samples;
        /* GPU timer query state. tq_have: entry points AND a functional
         * probe passed at init (a real begin/end/result cycle). tq[win]:
         * a ring of TQ_RING query objects so each has several frames to
         * land — re-arming a query whose result was never retrieved is a
         * GL INVALID_OPERATION and (observed in the field) permanently
         * stalls sample collection after a single missed poll. tq_pending:
         * per-ring-slot "result not yet collected" flags; tq_misses counts
         * consecutive frames where armed queries produced no result while
         * symbols exist — after 600 such frames the diagnostics say so and
         * timing is disabled instead of silently reporting 0. */
        bool tq_have;
        bool tq_probe_fail;     /* symbols resolved, functional probe failed */
        u32 tq_misses;
        GLuint_ tq[REV_PLATFORM_MAX_OUTPUTS][4];
        bool tq_pending[REV_PLATFORM_MAX_OUTPUTS][4];
        int tq_next[REV_PLATFORM_MAX_OUTPUTS];
        bool tq_armed_now[REV_PLATFORM_MAX_OUTPUTS];
} RenderGL;

/* ---- small helpers -------------------------------------------------------- */

static bool gl_make_program(RenderGL *r, int idx, const char *fs, char *err, size_t errcap);
static bool gl_make_program_vs(RenderGL *r, int idx, const char *vs, const char *fs,
                               char *err, size_t errcap);
static void pass_full(RenderGL *r, int pi, RTarget *dst);

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

/* ---- GPU fluid solver ------------------------------------------------------ */

static void fluid_zero(RenderGL *r, GLuint_ fbo)
{
        GL *g = &r->g;
        g->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        g->glClearColor(0, 0, 0, 0);
        g->glClear(GL_COLOR_BUFFER_BIT);
}

/* (re)create a float ping-pong pair at (w,h); on success the textures hold
 * zeros. Returns false on FBO incompleteness (caller drops the slot). */
static bool fluid_make_pair(RenderGL *r, GLuint_ *tex, GLuint_ *fbo, u32 w, u32 h)
{
        GL *g = &r->g;
        for (int i = 0; i < 2; i++) {
                if (!tex[i])
                        g->glGenTextures(1, &tex[i]);
                if (!fbo[i])
                        g->glGenFramebuffers(1, &fbo[i]);
                set_tex_params(g, tex[i], true);
                g->glBindTexture(GL_TEXTURE_2D, tex[i]);
                g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, (int)w, (int)h, 0,
                                GL_RGBA, GL_HALF_FLOAT, NULL);
                g->glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_TEXTURE_2D, tex[i], 0);
                if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                        return false;
                fluid_zero(r, fbo[i]);
        }
        return true;
}

static void fluid_free_pair(GL *g, GLuint_ *tex, GLuint_ *fbo)
{
        g->glDeleteTextures(2, tex);
        g->glDeleteFramebuffers(2, fbo);
        tex[0] = tex[1] = fbo[0] = fbo[1] = 0;
}

/* run program `pi` into dst (may be NULL = window) at dst's size */
static void fluid_pass(RenderGL *r, int pi, RTarget *dst)
{
        pass_full(r, pi, dst);
}

/* divergence + Jacobi + gradient subtraction on the current velocity */
static void fluid_project(RenderGL *r, GLFluid *f)
{
        GL *g = &r->g;
        const int gw = (int)f->gw, gh = (int)f->gh;

        /* divergence pass: vel[vc] -> press[pc] (writes (0, div)) */
        g->glUseProgram(r->prog[PROG_F_DIV]);
        g->glUniform2f(r->fu[PROG_F_DIV].grid, (float)gw, (float)gh);
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, f->vel[f->vc]);
        {
                RTarget t = { f->press[f->pc], f->press_fbo[f->pc], f->gw, f->gh };
                fluid_pass(r, PROG_F_DIV, &t);
        }

        /* Jacobi (Gauss-Seidel is sequential; Jacobi is the parallel form) */
        g->glUseProgram(r->prog[PROG_F_JACOBI]);
        g->glUniform2f(r->fu[PROG_F_JACOBI].grid, (float)gw, (float)gh);
        for (int i = 0; i < 32; i++) {
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->press[f->pc]);
                RTarget t = { f->press[f->pc ^ 1], f->press_fbo[f->pc ^ 1], f->gw, f->gh };
                fluid_pass(r, PROG_F_JACOBI, &t);
                f->pc ^= 1;
        }

        /* gradient subtraction: vel[vc] + press[pc] -> vel[vc^1] */
        g->glUseProgram(r->prog[PROG_F_GRADSUB]);
        g->glUniform2f(r->fu[PROG_F_GRADSUB].grid, (float)gw, (float)gh);
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, f->vel[f->vc]);
        g->glActiveTexture(GL_TEXTURE1);
        g->glBindTexture(GL_TEXTURE_2D, f->press[f->pc]);
        g->glActiveTexture(GL_TEXTURE0);
        {
                RTarget t = { f->vel[f->vc ^ 1], f->vel_fbo[f->vc ^ 1], f->gw, f->gh };
                fluid_pass(r, PROG_F_GRADSUB, &t);
        }
        f->vc ^= 1;
}

/* find/create the sim slot for a scene descriptor; NULL when the table is
 * full (8 fluid layers per monitor is far past any sane scene). */
static GLFluid *fluid_slot(RenderGL *r, int win, const RSimFluid *sd)
{
        GLFluid *tab = r->fluids[win];
        for (int i = 0; i < GL_MAX_FLUIDS; i++)
                if (tab[i].key == sd && tab[i].ok) {
                        tab[i].last_seen = r->frame_no;
                        return &tab[i];
                }

        /* pick a free or expired slot */
        GLFluid *best = NULL;
        for (int i = 0; i < GL_MAX_FLUIDS; i++) {
                if (!tab[i].key) {
                        best = &tab[i];
                        break;
                }
                if (r->frame_no - tab[i].last_seen > 300 &&
                    (!best || tab[i].last_seen < best->last_seen))
                        best = &tab[i];
        }
        if (!best) {
                static bool warned;
                if (!warned) {
                        warned = true;
                        LOGW("opengl: too many fluid layers, dropping sims");
                }
                return NULL;
        }

        GL *g = &r->g;
        fluid_free_pair(g, best->vel, best->vel_fbo);
        fluid_free_pair(g, best->dye, best->dye_fbo);
        fluid_free_pair(g, best->press, best->press_fbo);
        if (best->curl) {
                g->glDeleteTextures(1, &best->curl);
                g->glDeleteFramebuffers(1, &best->curl_fbo);
                best->curl = best->curl_fbo = 0;
        }
        if (best->out8) {
                g->glDeleteTextures(1, &best->out8);
                g->glDeleteFramebuffers(1, &best->out8_fbo);
                best->out8 = best->out8_fbo = 0;
        }

        best->gw = sd->dye_w / 2;
        best->gh = sd->dye_h / 2;
        best->dw = sd->dye_w;
        best->dh = sd->dye_h;
        best->vc = best->dc = best->pc = 0;
        best->amb_t = 0;
        best->wisp_acc = 0;
        best->ok = false;
        best->key = sd;
        best->last_seen = r->frame_no;

        if (best->gw < 8 || best->gh < 8)
                return NULL;
        if (!fluid_make_pair(r, best->vel, best->vel_fbo, best->gw, best->gh) ||
            !fluid_make_pair(r, best->dye, best->dye_fbo, best->dw, best->dh) ||
            !fluid_make_pair(r, best->press, best->press_fbo, best->gw, best->gh))
                return NULL;

        if (!best->curl)
                g->glGenTextures(1, &best->curl);
        if (!best->curl_fbo)
                g->glGenFramebuffers(1, &best->curl_fbo);
        set_tex_params(g, best->curl, true);
        g->glBindTexture(GL_TEXTURE_2D, best->curl);
        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, (int)best->gw, (int)best->gh, 0,
                        GL_RGBA, GL_HALF_FLOAT, NULL);
        g->glBindFramebuffer(GL_FRAMEBUFFER, best->curl_fbo);
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, best->curl, 0);
        if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                return NULL;

        if (!best->out8)
                g->glGenTextures(1, &best->out8);
        if (!best->out8_fbo)
                g->glGenFramebuffers(1, &best->out8_fbo);
        set_tex_params(g, best->out8, true);
        g->glBindTexture(GL_TEXTURE_2D, best->out8);
        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)best->dw, (int)best->dh, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g->glBindFramebuffer(GL_FRAMEBUFFER, best->out8_fbo);
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, best->out8, 0);
        if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                return NULL;

        best->ok = true;
        return best;
}

/* Advance one fluid step; returns the tonemapped straight-alpha RGBA8 dye
 * texture (dw x dh) ready for the standard premult+composite path. */
static GLuint_ gl_fluid_step(RenderGL *r, GLFluid *f, const RSimFluid *sd)
{
        GL *g = &r->g;
        f64 dt = sd->dt;
        if (dt <= 0)
                dt = 1.0 / 60.0;
        if (dt > 0.1)
                dt = 0.1;
        const double force = sd->force;
        const double radius = sd->radius < 0.002 ? 0.002 :
                              sd->radius > 0.2 ? 0.2 : sd->radius;
        /* fluid_configure()'s clamps — the CPU solver never sees values
         * outside these ranges, so the GPU one must not either (an
         * unclamped ambient over-forces the field and shreds the dye). */
        const double dissip = sd->dissip < 0.0 ? 0.0 :
                              sd->dissip > 1.0 ? 1.0 : sd->dissip;
        const double ambient = sd->ambient < 0.0 ? 0.0 :
                               sd->ambient > 1.0 ? 1.0 : sd->ambient;
        const double rad_g = radius * f->gw * 0.5 + 1.5;   /* grid texels */
        const double rad_d = rad_g * 2.0;                  /* dye texels  */

        /* ---- C-side event bookkeeping (mirrors fluid.c exactly) ---- */
        f64 amb_t = f->amb_t;
        if (ambient > 0)
            amb_t += dt;

        bool wisp_fire = false;
        f64 wisp_x = 0, wisp_y = 0, wisp_vx = 0, wisp_vy = 0;
        RColor wisp_c = sd->dye_a;
        if (ambient > 0) {
                f->wisp_acc += dt;
                if (f->wisp_acc > 0.9) {
                        f->wisp_acc = 0;
                        wisp_fire = true;
                        wisp_x = 0.15 + 0.7 * fabs(sin(amb_t * 1.7));
                        wisp_y = 0.15 + 0.7 * fabs(cos(amb_t * 1.1));
                        wisp_vx = sin(amb_t * 2.0) * 0.6;
                        wisp_vy = cos(amb_t * 1.6) * 0.6;
                        wisp_c = (((int)(amb_t * 3) & 1) != 0) ? sd->dye_a : sd->dye_b;
                }
        }

        /* pointer splat. Orientation chain (all top-down consistent): the
         * present pass flips v so accum row 0 shows at the window TOP; the
         * composite maps the layer 1:1 (dy from top); the tonemap is
         * identity — so dye v=0 is the layer's top and pointer py (measured
         * from the top) maps DIRECTLY onto dye v, +vy moving dye downscreen,
         * exactly like the CPU solver's row-space. */
        bool ptr_fire = sd->pressed;
        f64 ptr_x = sd->px, ptr_y = sd->py;
        f64 ptr_vx = 0, ptr_vy = 0;
        RColor ptr_c = sd->dye_a;
        if (ptr_fire) {
                ptr_vx = sd->pdx * force;
                ptr_vy = sd->pdy * force;
                double speed = hypot(sd->pdx, sd->pdy);
                if (sd->pdx >= 0)
                        ptr_c = sd->dye_a;
                else
                        ptr_c = sd->dye_b;
                if (speed < 1e-4)
                        ptr_c = ((ptr_x * 7.0 + ptr_y * 13.0) > 10.0) ? sd->dye_a : sd->dye_b;
        }

        /* ---- pass 1: velocity forces (splat + ambient) ---- */
        {
                int pi = PROG_F_VELF;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->fu[pi].grid, (float)f->gw, (float)f->gh);
                g->glUniform2f(r->fu[pi].posA, (float)ptr_x, (float)ptr_y);
                g->glUniform2f(r->fu[pi].impA, (float)(ptr_fire ? ptr_vx : 0.0),
                               (float)(ptr_fire ? ptr_vy : 0.0));
                g->glUniform1f(r->fu[pi].radA, (float)(ptr_fire ? rad_g : 1.0));
                g->glUniform2f(r->fu[pi].posB, (float)wisp_x, (float)wisp_y);
                g->glUniform2f(r->fu[pi].impB, (float)(wisp_fire ? wisp_vx : 0.0),
                               (float)(wisp_fire ? wisp_vy : 0.0));
                g->glUniform1f(r->fu[pi].radB, (float)(wisp_fire ? rad_g : 1.0));
                g->glUniform1f(r->fu[pi].ambient, (float)ambient);
                g->glUniform1f(r->fu[pi].dt, (float)dt);
                g->glUniform1f(r->fu[pi].t, (float)amb_t);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->vel[f->vc]);
                RTarget t = { f->vel[f->vc ^ 1], f->vel_fbo[f->vc ^ 1], f->gw, f->gh };
                fluid_pass(r, pi, &t);
                f->vc ^= 1;
        }

        /* ---- pass 2: dye forces (splat colors) ---- */
        if (ptr_fire || wisp_fire) {
                int pi = PROG_F_DYEF;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->fu[pi].grid, (float)f->dw, (float)f->dh);
                g->glUniform2f(r->fu[pi].posA, (float)ptr_x, (float)ptr_y);
                g->glUniform2f(r->fu[pi].posB, (float)wisp_x, (float)wisp_y);
                g->glUniform1f(r->fu[pi].radA, (float)(ptr_fire ? rad_d : 1.0));
                g->glUniform1f(r->fu[pi].radB, (float)(wisp_fire ? rad_d : 1.0));
                g->glUniform3f(r->fu[pi].colA, ptr_c.r / 255.0f, ptr_c.g / 255.0f,
                               ptr_c.b / 255.0f);
                g->glUniform3f(r->fu[pi].colB, wisp_c.r / 255.0f, wisp_c.g / 255.0f,
                               wisp_c.b / 255.0f);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->dye[f->dc]);
                RTarget t = { f->dye[f->dc ^ 1], f->dye_fbo[f->dc ^ 1], f->dw, f->dh };
                fluid_pass(r, pi, &t);
                f->dc ^= 1;
        }

        /* ---- vorticity confinement ---- */
        {
                int pi = PROG_F_CURL;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->fu[pi].grid, (float)f->gw, (float)f->gh);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->vel[f->vc]);
                RTarget t = { f->curl, f->curl_fbo, f->gw, f->gh };
                fluid_pass(r, pi, &t);
        }
        {
                int pi = PROG_F_VORT;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->fu[pi].grid, (float)f->gw, (float)f->gh);
                g->glUniform1f(r->fu[pi].k, (float)(6.0 * dt));
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->vel[f->vc]);
                g->glActiveTexture(GL_TEXTURE1);
                g->glBindTexture(GL_TEXTURE_2D, f->curl);
                g->glActiveTexture(GL_TEXTURE0);
                RTarget t = { f->vel[f->vc ^ 1], f->vel_fbo[f->vc ^ 1], f->gw, f->gh };
                fluid_pass(r, pi, &t);
                f->vc ^= 1;
        }

        fluid_project(r, f);

        /* ---- advect velocity (+ damping folded in) ---- */
        {
                int pi = PROG_F_ADV_VEL;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->fu[pi].grid, (float)f->gw, (float)f->gh);
                g->glUniform1f(r->fu[pi].dt, (float)dt);
                g->glUniform1f(r->fu[pi].damp, (float)pow(0.999, dt * 60.0));
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->vel[f->vc]);
                RTarget t = { f->vel[f->vc ^ 1], f->vel_fbo[f->vc ^ 1], f->gw, f->gh };
                fluid_pass(r, pi, &t);
                f->vc ^= 1;
        }

        fluid_project(r, f);

        /* ---- advect dye (+ dissipation folded in) ---- */
        {
                int pi = PROG_F_ADV_DYE;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->fu[pi].grid, (float)f->gw, (float)f->gh);
                g->glUniform1f(r->fu[pi].dt, (float)dt);
                double fade = 1.0 - dissip * 0.35;
                if (fade < 0)
                        fade = 0;
                g->glUniform1f(r->fu[pi].fade, (float)pow(fade, dt * 60.0));
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->dye[f->dc]);
                g->glActiveTexture(GL_TEXTURE1);
                g->glBindTexture(GL_TEXTURE_2D, f->vel[f->vc]);
                g->glActiveTexture(GL_TEXTURE0);
                RTarget t = { f->dye[f->dc ^ 1], f->dye_fbo[f->dc ^ 1], f->dw, f->dh };
                fluid_pass(r, pi, &t);
                f->dc ^= 1;
        }

        f->amb_t = amb_t;


        /* ---- tonemap: dye (float) -> layer texture (straight RGBA8) ---- */
        {
                int pi = PROG_F_TONEMAP;
                g->glUseProgram(r->prog[pi]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, f->dye[f->dc]);
                RTarget t = { f->out8, f->out8_fbo, f->dw, f->dh };
                fluid_pass(r, pi, &t);
        }
        return f->out8;
}

/* ---- GPU ripple solver ------------------------------------------------------ */

/* find/create the ripple slot for a scene descriptor (same identity contract
 * as fluid_slot); NULL when the table is full. */
static GLRipple *ripple_slot(RenderGL *r, int win, const RSimRipple *sd)
{
        GLRipple *tab = r->ripples[win];
        for (int i = 0; i < GL_MAX_RIPPLES; i++)
                if (tab[i].key == sd && tab[i].ok) {
                        tab[i].last_seen = r->frame_no;
                        return &tab[i];
                }

        GLRipple *best = NULL;
        for (int i = 0; i < GL_MAX_RIPPLES; i++) {
                if (!tab[i].key) {
                        best = &tab[i];
                        break;
                }
                if (r->frame_no - tab[i].last_seen > 300 &&
                    (!best || tab[i].last_seen < best->last_seen))
                        best = &tab[i];
        }
        if (!best) {
                static bool warned;
                if (!warned) {
                        warned = true;
                        LOGW("opengl: too many ripple layers, dropping sims");
                }
                return NULL;
        }

        GL *g = &r->g;
        fluid_free_pair(g, best->field, best->fbo);
        best->gw = sd->gw;
        best->gh = sd->gh;
        best->cur = 0;
        best->ok = false;
        best->key = sd;
        best->last_seen = r->frame_no;

        if (best->gw < 8 || best->gh < 8)
                return NULL;
        if (!fluid_make_pair(r, best->field, best->fbo, best->gw, best->gh))
                return NULL;
        best->ok = true;
        return best;
}

/* Advance the height field; returns the current field texture. */
static GLuint_ gl_ripple_step(RenderGL *r, GLRipple *rp, const RSimRipple *sd)
{
        GL *g = &r->g;
        f64 dt = sd->dt;
        if (dt <= 0)
                dt = 1.0 / 60.0;
        if (dt > 0.1)
                dt = 0.1;
        /* ripple_configure()'s clamps — keep the GPU field inside the same
         * ranges the CPU solver enforces */
        const double strength = sd->strength < 0.1 ? 0.1 :
                                sd->strength > 8.0 ? 8.0 : sd->strength;
        const double damping = sd->damping < 0.001 ? 0.001 :
                                sd->damping > 0.9 ? 0.9 : sd->damping;
        const double radius = sd->radius < 0.002 ? 0.002 :
                              sd->radius > 0.1 ? 0.1 : sd->radius;
        /* ripple_step() runs (int)clamp(dt*60, 0.5, 2.0) iterations, with
         * the pointer drip and click impacts applied once, before them.
         * The splat pass mirrors that order: injections are written into
         * the field FIRST (like ripple_click()/drip mutate cur in place),
         * so every wave step sees a fully post-injection Laplacian. */
        int steps = (int)rev_clampf(dt * 60.0, 0.5, 2.0);

        if (sd->pressed || sd->nclicks > 0) {
                int pi = PROG_R_SPLAT;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->ru[pi].grid, (float)rp->gw, (float)rp->gh);
                g->glUniform2f(r->ru[pi].ptr, (float)sd->px, (float)sd->py);
                g->glUniform1f(r->ru[pi].ptrRad, (float)(radius * rp->gw));
                g->glUniform1f(r->ru[pi].ptrAmp, (float)(-strength));
                g->glUniform1i(r->ru[pi].ptrOn, sd->pressed ? 1 : 0);
                g->glUniform1i(r->ru[pi].nclicks, (int)sd->nclicks);
                if (sd->nclicks > 0) {
                        float cpos[8], cpar[8];
                        const double crad = rev_clampf(radius * 2.5, 0.004, 0.12) * rp->gw;
                        const double camp = -strength * 1.5;
                        for (u32 c = 0; c < sd->nclicks && c < 4; c++) {
                                cpos[c * 2]     = (float)sd->click_x[c];
                                cpos[c * 2 + 1] = (float)sd->click_y[c];
                                cpar[c * 2]     = (float)crad;
                                cpar[c * 2 + 1] = (float)camp;
                        }
                        for (u32 c = sd->nclicks; c < 4; c++) {
                                cpos[c * 2] = cpos[c * 2 + 1] = 0.0f;
                                cpar[c * 2] = cpar[c * 2 + 1] = 0.0f;
                        }
                        g->glUniform2fv(r->ru[pi].clickPos, 4, cpos);
                        g->glUniform2fv(r->ru[pi].clickParam, 4, cpar);
                }
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, rp->field[rp->cur]);
                RTarget t = { rp->field[rp->cur ^ 1], rp->fbo[rp->cur ^ 1],
                              rp->gw, rp->gh };
                pass_full(r, pi, &t);
                rp->cur ^= 1;
        }

        for (int s = 0; s < steps; s++) {
                int pi = PROG_R_STEP;
                g->glUseProgram(r->prog[pi]);
                g->glUniform2f(r->ru[pi].grid, (float)rp->gw, (float)rp->gh);
                g->glUniform1f(r->ru[pi].damp,
                               (float)(damping * 0.05));
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, rp->field[rp->cur]);
                RTarget t = { rp->field[rp->cur ^ 1], rp->fbo[rp->cur ^ 1],
                              rp->gw, rp->gh };
                pass_full(r, pi, &t);
                rp->cur ^= 1;
        }

        /* REV_RIPPLE_DUMP=<f1,f2,...>: debug hook — read the height field
         * back (glReadPixels GL_HALF_FLOAT on the RGBA16F FBO) after the
         * listed step-CALL numbers (comma list, counts from 1) and log
         * statistics + a raw /tmp/ripple-field-<call>.f16 dump. The log line
         * also reports the total step iterations executed so far. */
        {
                static int want[16];
                static int nwant;
                static int calls;
                static u64 total_steps;
                static bool parsed;
                if (!parsed) {
                        parsed = true;
                        total_steps += (u64)steps;
                        const char *spec = getenv("REV_RIPPLE_DUMP");
                        if (spec && *spec) {
                                const char *p = spec;
                                while (nwant < 16) {
                                        int v = atoi(p);
                                        if (v <= 0)
                                                break;
                                        want[nwant++] = v;
                                        const char *c = strchr(p, ',');
                                        if (!c)
                                                break;
                                        p = c + 1;
                                }
                        }
                } else {
                        total_steps += (u64)steps;
                }
                calls++;
                for (int k = 0; k < nwant; k++) {
                        if (want[k] != calls)
                                continue;
                        u16 *buf = (u16 *)malloc((size_t)rp->gw * rp->gh * 4 * 2);
                        if (!buf)
                                break;
#define F16_EXP(v) (((v) >> 10) & 0x1f)
#define F16_MAN(v) ((v) & 0x3ff)
                        g->glBindFramebuffer(GL_FRAMEBUFFER, rp->fbo[rp->cur]);
                        g->glReadPixels(0, 0, (int)rp->gw, (int)rp->gh,
                                        0x1908 /* GL_RGBA */, 0x140B /* GL_HALF_FLOAT */,
                                        buf);
                        long nz = 0;
                        double sum = 0, sx = 0, sy = 0, vmin = 1e9, vmax = -1e9;
                        double gmin = 1e9, gmax = -1e9, amin = 1e9, amax = -1e9;
                        for (u32 y = 0; y < rp->gh; y++)
                                for (u32 x = 0; x < rp->gw; x++) {
                                        double h, gv, av;
                                        {
                                                u16 raw = buf[(size_t)(y * rp->gw + x) * 4];
                                                int e = F16_EXP(raw), m = F16_MAN(raw);
                                                h = (raw & 0x8000) ? -1.0 : 1.0;
                                                if (e == 0) h *= m * 5.9604644775390625e-08;
                                                else if (e == 31) h = m ? (0.0 / 0.0) : h * 1e9;
                                                else h *= ldexp(1024.0 + m, e - 25);
                                        }
                                        {
                                                u16 raw = buf[(size_t)(y * rp->gw + x) * 4 + 1];
                                                int e = F16_EXP(raw), m = F16_MAN(raw);
                                                gv = (raw & 0x8000) ? -1.0 : 1.0;
                                                if (e == 0) gv *= m * 5.9604644775390625e-08;
                                                else if (e == 31) gv = m ? (0.0 / 0.0) : gv * 1e9;
                                                else gv *= ldexp(1024.0 + m, e - 25);
                                        }
                                        {
                                                u16 raw = buf[(size_t)(y * rp->gw + x) * 4 + 3];
                                                int e = F16_EXP(raw), m = F16_MAN(raw);
                                                av = (raw & 0x8000) ? -1.0 : 1.0;
                                                if (e == 0) av *= m * 5.9604644775390625e-08;
                                                else if (e == 31) av = m ? (0.0 / 0.0) : av * 1e9;
                                                else av *= ldexp(1024.0 + m, e - 25);
                                        }
                                        if (gv < gmin) gmin = gv;
                                        if (gv > gmax) gmax = gv;
                                        if (av < amin) amin = av;
                                        if (av > amax) amax = av;
                                        if (h < vmin) vmin = h;
                                        if (h > vmax) vmax = h;
                                        if (fabs(h) > 1e-4) {
                                                nz++;
                                                sum += h;
                                                sx += x;
                                                sy += y;
                                        }
                                }
#undef F16_EXP
#undef F16_MAN
                        LOGI("ripple-dump[call %d, steps %llu]: %ux%d nz=%ld sum=%.3f "
                             "min=%.3f max=%.3f gmin=%.3f gmax=%.3f amin=%.3f amax=%.3f%s",
                             calls, (unsigned long long)total_steps,
                             rp->gw, rp->gh, nz, sum, vmin, vmax, gmin, gmax,
                             amin, amax, nz ? "" : " (flat)");
                        if (nz)
                                LOGI("ripple-dump: centroid=(%.1f, %.1f) texels",
                                     sx / nz, sy / nz);
                        char path[256];
                        snprintf(path, sizeof path, "/tmp/ripple-field-%d.f16", calls);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                                fwrite(buf, 2, (size_t)rp->gw * rp->gh * 4, f);
                                fclose(f);
                        }
                        free(buf);
                        break;
                }
        }
        return rp->field[rp->cur];
}

/* ---- GPU particle layer ---------------------------------------------------- */

/* Uploads the packed particle bands and renders the point-sprite layer
 * into a frame-size transparent target; returns its texture for the
 * standard composite pass. */
static GLuint_ gl_particles_layer(RenderGL *r, const RLayerCmd *cmd)
{
        GL *g = &r->g;
        const RSimParticles *pd = cmd->psim;
        u32 rows = (pd->slots + 1023) / 1024;
        u32 tex_h = rows * 2;

        if (!r->ptex) {
                g->glGenTextures(1, &r->ptex);
                g->glBindTexture(GL_TEXTURE_2D, r->ptex);
                g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        g->glBindTexture(GL_TEXTURE_2D, r->ptex);
        if (r->ptex_h != tex_h) {
                g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1024, (int)tex_h, 0,
                                GL_RGBA, GL_FLOAT, NULL);
                r->ptex_h = tex_h;
        }
        if (pd->n) {
                f64 up_t0 = rev_monotonic();
                g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, (int)tex_h,
                                   GL_RGBA, GL_FLOAT, pd->band0);
                r->perf_upload_ms += (rev_monotonic() - up_t0) * 1000.0;
        }

        if (!ensure_target(g, &r->ptarget, cmd->w, cmd->h))
                return 0;
        g->glBindFramebuffer(GL_FRAMEBUFFER, r->ptarget.fbo);
        g->glViewport(0, 0, (int)cmd->w, (int)cmd->h);
        g->glClearColor(0, 0, 0, 0);
        g->glClear(GL_COLOR_BUFFER_BIT);

        if (pd->n) {
                g->glUseProgram(r->prog[PROG_PARTICLES]);
                g->glUniform2f(r->pu[PROG_PARTICLES].frame,
                               (float)cmd->w, (float)cmd->h);
                g->glUniform1i(r->pu[PROG_PARTICLES].rows, (int)rows);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, r->ptex);
                /* source-over in straight alpha — the same blend the CPU
                 * sprite blit performs into its transparent layer buffer */
                g->glEnable(GL_BLEND);
                g->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                       GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                g->glDrawArrays(GL_POINTS, 0, (int)pd->n);
                g->glDisable(GL_BLEND);
        }
        return r->ptarget.tex;
}

/* ---- GPU gradient evaluation ----------------------------------------------- */

/* Renders (or reuses) the gradient texture for a descriptor; keyed by the
 * layer's content serial — static gradients evaluate once, animated angles
 * re-evaluate (one fullscreen pass, no CPU raster, no upload). */
static GLuint_ gl_grad_layer(RenderGL *r, const RLayerCmd *cmd)
{
        GL *g = &r->g;
        const RGradDesc *gd = cmd->grad;

        RTarget *slot = NULL;
        for (int i = 0; i < 8; i++)
                if (r->rgcache[i].id == cmd->content_id && cmd->content_id &&
                    r->rgcache[i].t.tex) {
                        slot = &r->rgcache[i].t;
                        r->rgcache[i].stamp = ++r->rg_clock;
                        break;
                }
        if (!slot) {
                int pick = 0;
                for (int i = 1; i < 8; i++)
                        if (r->rgcache[i].stamp < r->rgcache[pick].stamp)
                                pick = i;
                if (!ensure_target(g, &r->rgcache[pick].t, cmd->w, cmd->h))
                        return 0;
                r->rgcache[pick].id = cmd->content_id;
                r->rgcache[pick].stamp = ++r->rg_clock;
                slot = &r->rgcache[pick].t;

                /* evaluate: capped stops like build_lut() */
                float cols[17 * 4], offs[17];
                int n = gd->nstops;
                RColor cst[17];
                f64 coff[17];
                for (int i = 0; i < n; i++) {
                        cst[i] = gd->stops[i];
                        coff[i] = gd->offs[i];
                }
                if (n > 0 && coff[0] > 0) {
                        for (int i = n; i > 0; i--) {
                                cst[i] = cst[i - 1];
                                coff[i] = coff[i - 1];
                        }
                        coff[0] = 0.0;
                        n++;
                }
                if (n > 0 && coff[n - 1] < 1) {
                        cst[n] = cst[n - 1];
                        coff[n] = 1.0;
                        n++;
                }
                if (n < 2) {
                        cst[0] = cst[1] = gd->stops[0];
                        coff[0] = 0.0;
                        coff[1] = 1.0;
                        n = 2;
                }
                for (int i = 0; i < n; i++) {
                        cols[i * 4 + 0] = (float)cst[i].r / 255.0f;
                        cols[i * 4 + 1] = (float)cst[i].g / 255.0f;
                        cols[i * 4 + 2] = (float)cst[i].b / 255.0f;
                        cols[i * 4 + 3] = (float)cst[i].a / 255.0f;
                        offs[i] = (float)coff[i];
                }
                for (int i = n; i < 17; i++) {
                        cols[i * 4 + 0] = cols[(n - 1) * 4 + 0];
                        cols[i * 4 + 1] = cols[(n - 1) * 4 + 1];
                        cols[i * 4 + 2] = cols[(n - 1) * 4 + 2];
                        cols[i * 4 + 3] = cols[(n - 1) * 4 + 3];
                        offs[i] = 1.0f;
                }

                int pi = PROG_GRADIENT;
                g->glBindFramebuffer(GL_FRAMEBUFFER, slot->fbo);
                g->glViewport(0, 0, (int)slot->w, (int)slot->h);
                g->glUseProgram(r->prog[pi]);
                g->glUniform1i(r->gu[pi].shape, gd->shape);
                g->glUniform2f(r->gu[pi].p0, (float)gd->p0x, (float)gd->p0y);
                g->glUniform2f(r->gu[pi].p1, (float)gd->p1x, (float)gd->p1y);
                g->glUniform2f(r->gu[pi].rc, (float)gd->cx, (float)gd->cy);
                g->glUniform1f(r->gu[pi].rr, (float)gd->radius);
                g->glUniform4fv(r->gu[pi].cols, 17, cols);
                g->glUniform1fv(r->gu[pi].offs, 17, offs);
                g->glUniform1i(r->gu[pi].n, n);
                g->glUniform2f(r->gu[pi].dims, (float)slot->w, (float)slot->h);
                draw_full(g);
        }
        return slot->tex;
}

/* ---- content-addressed upload ---------------------------------------------- */
static GLuint_ upload_layer_tex(RenderGL *r, const RLayerCmd *cmd)
{
        GL *g = &r->g;
        TexSlot *cache = r->texcache;
        r->cache_clock++;

        if (cmd->content_id) {
                for (int i = 0; i < GL_TEX_CACHE; i++) {
                        if (cache[i].id == cmd->content_id &&
                            cache[i].tex && cache[i].w == cmd->w && cache[i].h == cmd->h) {
                                cache[i].stamp = r->cache_clock;
                                g->glBindTexture(GL_TEXTURE_2D, cache[i].tex);
                                return cache[i].tex;
                        }
                }
        }

        /* miss: evict the least-recently-used slot */
        TexSlot *slot = &cache[0];
        for (int i = 1; i < GL_TEX_CACHE; i++)
                if (cache[i].stamp < slot->stamp)
                        slot = &cache[i];
        if (!slot->tex)
                g->glGenTextures(1, &slot->tex);
        if (slot->w != cmd->w || slot->h != cmd->h) {
                set_tex_params(g, slot->tex, true);
                g->glBindTexture(GL_TEXTURE_2D, slot->tex);
                g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)cmd->w, (int)cmd->h, 0,
                                GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                slot->w = cmd->w;
                slot->h = cmd->h;
        }
        g->glBindTexture(GL_TEXTURE_2D, slot->tex);
        f64 up_t0 = rev_monotonic();
        g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cmd->w, (int)cmd->h,
                           GL_RGBA, GL_UNSIGNED_BYTE, cmd->px);
        r->perf_upload_ms += (rev_monotonic() - up_t0) * 1000.0;
        slot->id = cmd->content_id;    /* 0 = never matches a cached lookup */
        slot->stamp = r->cache_clock;
        return slot->tex;
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

/* ---- runtime window lifecycle (hotplug) ------------------------------------
 * Mirrors init's per-window surface setup and fini's teardown, one window at
 * a time. The reconciler calls add_window after the platform created the
 * window and remove_window BEFORE the platform destroys it. */
static bool gl_add_window(RRenderer *rr, RPlatform *p, int win)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        if (p != r->plat) {
                return false;    /* different platform instance: refuse */
        }
        return gl_window_surface(r, win);
}

static void gl_remove_window(RRenderer *rr, int win)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        if (win < 0 || win >= REV_PLATFORM_MAX_OUTPUTS)
                return;
        if (r->win[win].surf && r->eglDestroySurface) {
                r->eglDestroySurface(r->dpy, r->win[win].surf);
                r->win[win].surf = NULL;
        }
        if (r->win[win].eglwin) {
                /* hand the wl_surface back to the shim before the wrapper dies */
                wlshim_set_gl_surface(win, false);
                if (r->wl_egl_window_destroy)
                        r->wl_egl_window_destroy(r->win[win].eglwin);
                r->win[win].eglwin = NULL;
        }
        r->win[win].native = 0;
}

/* ---- init ------------------------------------------------------------------ */

static bool gl_init(RRenderer *rr, RPlatform *p, char *err, size_t errcap)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        GL *g = &r->g;
        r->plat = p;

        if (!p->has_gpu) {
                snprintf(err, errcap, "platform '%s' exposes no GPU surface handles"
                         " (wayland: REV_WAYLAND_GPU=0 or no libwayland-client)",
                         p->name);
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
                                 "wayland GPU needs the WSI shim (libwayland-client loadable)");
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
        LOADGL(glPixelStorei, "glPixelStorei");
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
        LOADGL(glUniform3f, "glUniform3f");
        LOADGL(glUniform1fv, "glUniform1fv");
        LOADGL(glUniform2fv, "glUniform2fv");
        LOADGL(glUniform4fv, "glUniform4fv");
        LOADGL(glDrawArrays, "glDrawArrays");
        LOADGL(glClearColor, "glClearColor");
        LOADGL(glClear, "glClear");
        LOADGL(glViewport, "glViewport");
        LOADGL(glDisable, "glDisable");
        LOADGL(glEnable, "glEnable");
        LOADGL(glBlendFuncSeparate, "glBlendFuncSeparate");
        LOADGL(glGetError, "glGetError");
        LOADGL(glGetString, "glGetString");
        LOADGL(glReadPixels, "glReadPixels");

        g->glDisable(0x0BE2 /* GL_BLEND */);
        r->gl_loaded = true;

        /* GPU timer queries (EXT_disjoint_timer_query). Detection order
         * (all three must agree before timing is claimed):
         *   1. the EXTENSION STRING on this context — symbols alone lie
         *      (desktop drivers happily resolve core-GL spellings on ES
         *      contexts that never produce results);
         *   2. entry points: the ES spec name glGetQueryObjectui64vEXT
         *      FIRST (the historical lookup used the desktop-flavored
         *      glGetQueryObjectu64vEXT spelling, which resolved on some
         *      drivers without ever yielding results), then the other
         *      spellings in order;
         *   3. a functional probe: begin/end a TIME_ELAPSED query, finish,
         *      and actually retrieve a result. Only a passing probe earns
         *      the "timer queries available" log line. */
        *(void **)&g->glGenQueriesQ = g->eglGetProcAddress("glGenQueriesEXT");
        if (!g->glGenQueriesQ)
                *(void **)&g->glGenQueriesQ = g->eglGetProcAddress("glGenQueries");
        *(void **)&g->glBeginQueryQ = g->eglGetProcAddress("glBeginQueryEXT");
        if (!g->glBeginQueryQ)
                *(void **)&g->glBeginQueryQ = g->eglGetProcAddress("glBeginQuery");
        *(void **)&g->glEndQueryQ = g->eglGetProcAddress("glEndQueryEXT");
        if (!g->glEndQueryQ)
                *(void **)&g->glEndQueryQ = g->eglGetProcAddress("glEndQuery");
        *(void **)&g->glDeleteQueriesQ = g->eglGetProcAddress("glDeleteQueriesEXT");
        if (!g->glDeleteQueriesQ)
                *(void **)&g->glDeleteQueriesQ = g->eglGetProcAddress("glDeleteQueries");
        *(void **)&g->glFinishQ = g->eglGetProcAddress("glFinish");
        *(void **)&g->glGetQueryObjectu64vQ = g->eglGetProcAddress("glGetQueryObjectui64vEXT");
        if (!g->glGetQueryObjectu64vQ)
                *(void **)&g->glGetQueryObjectu64vQ = g->eglGetProcAddress("glGetQueryObjectu64vEXT");
        if (!g->glGetQueryObjectu64vQ)
                *(void **)&g->glGetQueryObjectu64vQ = g->eglGetProcAddress("glGetQueryObjectu64v");
        if (!g->glGetQueryObjectu64vQ)
                *(void **)&g->glGetQueryObjectu64vQ = g->eglGetProcAddress("glGetQueryObjectui64v");
        {
                bool ext_ok = false;
                const char *exts = (const char *)g->glGetString(GL_EXTENSIONS_Q);
                if (exts && strstr(exts, "GL_EXT_disjoint_timer_query"))
                        ext_ok = true;
                r->tq_have = ext_ok && g->glGenQueriesQ && g->glBeginQueryQ &&
                             g->glEndQueryQ && g->glGetQueryObjectu64vQ;
                /* ES 3.0 contexts may also expose the desktop ARB_timer_query
                 * lineage where the extension string differs — allow the
                 * functional probe to decide when symbols all resolved. */
                r->tq_probe_fail = false;
                if (!r->tq_have && g->glGenQueriesQ && g->glBeginQueryQ &&
                    g->glEndQueryQ && g->glGetQueryObjectu64vQ) {
                        r->tq_have = true;    /* probe below decides for real */
                        r->tq_probe_fail = true;
                }
        }
        /* functional probe: a complete query lifecycle must return a result */
        if (r->tq_have) {
                GLuint_ q = 0;
                g->glGenQueriesQ(1, &q);
                bool ok = false;
                if (q) {
                        g->glBeginQueryQ(GL_TIME_ELAPSED_Q, q);
                        g->glEndQueryQ(GL_TIME_ELAPSED_Q);
                        if (g->glFinishQ)
                                g->glFinishQ();
                        GLuint64_ avail = 0, res = 0;
                        /* bounded wait: a healthy driver answers immediately
                         * after Finish; a broken one never does */
                        for (int tries = 0; tries < 100 && !avail; tries++)
                                g->glGetQueryObjectu64vQ(q, GL_QUERY_RESULT_AVAILABLE_Q, &avail);
                        if (avail) {
                                g->glGetQueryObjectu64vQ(q, GL_QUERY_RESULT_Q, &res);
                                ok = true;    /* any value, even 0, is a result */
                        }
                        g->glDeleteQueriesQ(1, &q);
                }
                if (ok) {
                        r->tq_probe_fail = false;
                } else {
                        r->tq_have = false;
                }
        }

        const char *rend = (const char *)g->glGetString(GL_RENDERER);
        const char *vend = (const char *)g->glGetString(GL_VENDOR);
        if (rend && (strstr(rend, "llvmpipe") || strstr(rend, "softpipe") ||
                     strstr(rend, "swrast") || strstr(rend, "SwiftShader") ||
                     strstr(rend, "Software Rasterizer"))) {
                r->software = true;
                rr->software = true;
                LOGI("opengl: SOFTWARE rasterizer active (%s) — llvmpipe-tier; "
                     "rendering will NOT be hardware accelerated", rend);
        } else {
                LOGI("opengl: hardware renderer %s (vendor: %s)",
                     rend ? rend : "?", (vend && vend[0]) ? vend : "unknown");
        }
        /* expose identity through the renderer API (selection + daemon log) */
        {
                const char *id = rend ? rend : "unknown GL renderer";
                size_t n = (size_t)snprintf(rr->device_desc, sizeof rr->device_desc,
                                            "%s", id);
                if (n >= sizeof rr->device_desc)
                        rr->device_desc[sizeof rr->device_desc - 1] = 0;
        }
        /* Timer queries are a hardware-GPU instrument. On software GL they
         * would merely duplicate the CPU wall clock — worse, polling
         * QUERY_RESULT_AVAILABLE serializes llvmpipe's async worker pool
         * (measured: 1080p snowfall 60 fps async -> 6 fps serialized), so
         * they are disabled there outright. */
        if (r->software)
                r->tq_have = false;
        if (r->tq_have)
                LOGI("opengl: GPU timer queries available (extension + entry "
                     "points + functional probe) — REV_PROFILE reports async "
                     "GPU timings");
        else if (r->tq_probe_fail)
                LOGW("opengl: GPU timer queries NOT available (entry points "
                     "resolved but a begin/end/result probe produced no "
                     "result — reporting honestly instead of claiming them)");

        /* Point-sprite particle layers: hardware only. llvmpipe's wide-point
         * rasterization is pathologically slow (measured: 1080p snowfall
         * 6 fps with GPU points vs 60 fps with the CPU sprite raster) —
         * the CPU raster is the right tool there. The REV_GL_FLUID_SW test
         * hook forces the GPU path on so it stays regression-covered. */
        rr->wants_points = !r->software || getenv("REV_GL_FLUID_SW") != NULL;
        /* Shader-evaluated gradients: hardware only (a fullscreen fragment
         * pass costs more than the LUT fill it replaces on software GL). */
        rr->wants_grads = !r->software || getenv("REV_GL_FLUID_SW") != NULL;
        /* Video plane upload + shader conversion: R8 textures are ES3 core —
         * strictly less bandwidth than the CPU path's RGBA upload, and the
         * conversion rides along with a pass we already pay for. */
        rr->wants_video = true;

        /* programs */
        if (!gl_make_program(r, PROG_PREMULT, FS_PREMULT, err, errcap) ||
            !gl_make_program(r, PROG_COPY, FS_COPY, err, errcap) ||
            !gl_make_program(r, PROG_BLUR, FS_BLUR, err, errcap) ||
            !gl_make_program(r, PROG_COMPOSITE, FS_COMPOSITE, err, errcap) ||
            !gl_make_program(r, PROG_PRESENT, FS_PRESENT, err, errcap) ||
            !gl_make_program(r, PROG_GRADIENT, FS_GRADIENT, err, errcap) ||
            !gl_make_program(r, PROG_VIDEO, FS_VIDEO, err, errcap) ||
            !gl_make_program_vs(r, PROG_PARTICLES, VS_PARTICLES, FS_PARTICLES,
                                err, errcap))
                return false;

        /* GPU fluid + ripple solvers: probe RGBA16F renderability by actually
         * building one (extension strings lie less often than FBO status).
         * Hardware GL + float targets -> sims move off the CPU entirely;
         * software GL keeps the CPU solvers (fragment dispatch overhead
         * loses to tight C loops on llvmpipe) unless REV_GL_FLUID_SW=1 opts
         * in for testing. */
        {
                GLuint_ ft = 0, ff = 0;
                g->glGenTextures(1, &ft);
                g->glGenFramebuffers(1, &ff);
                set_tex_params(g, ft, true);
                g->glBindTexture(GL_TEXTURE_2D, ft);
                g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 4, 4, 0,
                                GL_RGBA, GL_HALF_FLOAT, NULL);
                g->glBindFramebuffer(GL_FRAMEBUFFER, ff);
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_TEXTURE_2D, ft, 0);
                r->sims_ok = g->glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                              GL_FRAMEBUFFER_COMPLETE;
                g->glDeleteTextures(1, &ft);
                g->glDeleteFramebuffers(1, &ff);
                if (r->sims_ok && r->software && !getenv("REV_GL_FLUID_SW"))
                        r->sims_ok = false;
        }
        if (r->sims_ok) {
                char ferr[256] = { 0 };
                if (!gl_make_program(r, PROG_F_VELF, FS_FLUID_VELF, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_DYEF, FS_FLUID_DYEF, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_CURL, FS_FLUID_CURL, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_VORT, FS_FLUID_VORT, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_DIV, FS_FLUID_DIV, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_JACOBI, FS_FLUID_JACOBI, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_GRADSUB, FS_FLUID_GRADSUB, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_ADV_VEL, FS_FLUID_ADVECT_VEL, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_ADV_DYE, FS_FLUID_ADVECT_DYE, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_F_TONEMAP, FS_FLUID_TONEMAP, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_R_SPLAT, FS_RIPPLE_SPLAT, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_R_STEP, FS_RIPPLE_STEP, ferr, sizeof ferr) ||
                    !gl_make_program(r, PROG_R_COMPOSITE, FS_RIPPLE_COMPOSITE,
                                     ferr, sizeof ferr)) {
                        LOGW("opengl: sim shaders failed (%s) — sims stay on CPU", ferr);
                        r->sims_ok = false;
                }
        }
        rr->wants_sims = r->sims_ok;
        if (r->sims_ok)
                LOGI("opengl: fluid + ripple solvers on GPU (RGBA16F render targets)");
        else if (!r->software)
                LOGI("opengl: no float render target — sims stay on CPU");

        /* uniform locations + static sampler bindings */
        for (int i = 0; i < PROG_COUNT; i++) {
                GLuint_ pg = r->prog[i];
                if (!pg)
                        continue;
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
                /* fluid samplers: fields on unit 0, secondary field on 1 */
                int lv = g->glGetUniformLocation(pg, "u_vel");
                if (lv >= 0)
                        g->glUniform1i(lv, 0);
                int ldy = g->glGetUniformLocation(pg, "u_dye");
                if (ldy >= 0)
                        g->glUniform1i(ldy, 0);
                int lp = g->glGetUniformLocation(pg, "u_press");
                if (lp >= 0)
                        g->glUniform1i(lp, 1);
                int lc = g->glGetUniformLocation(pg, "u_curl");
                if (lc >= 0)
                        g->glUniform1i(lc, 1);
                /* ripple: height field on unit 0 (u_dst is already unit 1) */
                int lfi = g->glGetUniformLocation(pg, "u_field");
                if (lfi >= 0)
                        g->glUniform1i(lfi, 0);
                r->u[i].off = g->glGetUniformLocation(pg, "u_off");
                r->u[i].scale = g->glGetUniformLocation(pg, "u_scale");
                r->u[i].dir = g->glGetUniformLocation(pg, "u_dir");
                r->u[i].weights = g->glGetUniformLocation(pg, "u_w[0]");
                r->u[i].blend = g->glGetUniformLocation(pg, "u_blend");
                r->u[i].opacity = g->glGetUniformLocation(pg, "u_opacity");
                r->u[i].src_premul = g->glGetUniformLocation(pg, "u_src_premul");
                r->fu[i].grid = g->glGetUniformLocation(pg, "u_grid");
                r->fu[i].dt = g->glGetUniformLocation(pg, "u_dt");
                r->fu[i].damp = g->glGetUniformLocation(pg, "u_damp");
                r->fu[i].fade = g->glGetUniformLocation(pg, "u_fade");
                r->fu[i].t = g->glGetUniformLocation(pg, "u_t");
                r->fu[i].k = g->glGetUniformLocation(pg, "u_k");
                r->fu[i].posA = g->glGetUniformLocation(pg, "u_posA");
                r->fu[i].posB = g->glGetUniformLocation(pg, "u_posB");
                r->fu[i].impA = g->glGetUniformLocation(pg, "u_impA");
                r->fu[i].impB = g->glGetUniformLocation(pg, "u_impB");
                r->fu[i].radA = g->glGetUniformLocation(pg, "u_radA");
                r->fu[i].radB = g->glGetUniformLocation(pg, "u_radB");
                r->fu[i].colA = g->glGetUniformLocation(pg, "u_colA");
                r->fu[i].colB = g->glGetUniformLocation(pg, "u_colB");
                r->fu[i].ambient = g->glGetUniformLocation(pg, "u_ambient");
                r->ru[i].field = g->glGetUniformLocation(pg, "u_field");
                r->ru[i].grid = g->glGetUniformLocation(pg, "u_grid");
                r->ru[i].damp = g->glGetUniformLocation(pg, "u_damp");
                r->ru[i].ptr = g->glGetUniformLocation(pg, "u_ptr");
                r->ru[i].ptrRad = g->glGetUniformLocation(pg, "u_ptrRad");
                r->ru[i].ptrAmp = g->glGetUniformLocation(pg, "u_ptrAmp");
                r->ru[i].ptrOn = g->glGetUniformLocation(pg, "u_ptrOn");
                r->ru[i].clickPos = g->glGetUniformLocation(pg, "u_clickPos[0]");
                r->ru[i].clickParam = g->glGetUniformLocation(pg, "u_clickParam[0]");
                r->ru[i].nclicks = g->glGetUniformLocation(pg, "u_nclicks");
                r->ru[i].amp = g->glGetUniformLocation(pg, "u_amp");
                r->ru[i].opacity = g->glGetUniformLocation(pg, "u_opacity");
                r->pu[i].frame = g->glGetUniformLocation(pg, "u_frame");
                r->pu[i].rows = g->glGetUniformLocation(pg, "u_rows");
                r->gu[i].shape = g->glGetUniformLocation(pg, "u_shape");
                r->gu[i].p0 = g->glGetUniformLocation(pg, "u_p0");
                r->gu[i].p1 = g->glGetUniformLocation(pg, "u_p1");
                r->gu[i].rc = g->glGetUniformLocation(pg, "u_rc");
                r->gu[i].rr = g->glGetUniformLocation(pg, "u_rr");
                r->gu[i].cols = g->glGetUniformLocation(pg, "u_cols[0]");
                r->gu[i].offs = g->glGetUniformLocation(pg, "u_offs[0]");
                r->gu[i].n = g->glGetUniformLocation(pg, "u_n");
                r->gu[i].dims = g->glGetUniformLocation(pg, "u_dims");
                r->vu[i].y = g->glGetUniformLocation(pg, "u_y");
                r->vu[i].u = g->glGetUniformLocation(pg, "u_u");
                r->vu[i].v = g->glGetUniformLocation(pg, "u_v");
                r->vu[i].dst = g->glGetUniformLocation(pg, "u_dst");
                r->vu[i].blend = g->glGetUniformLocation(pg, "u_blend");
                r->vu[i].opacity = g->glGetUniformLocation(pg, "u_opacity");
        }
        /* PROG_VIDEO: y/u/v on units 0/1/2, accum on unit 3 (the generic
         * loop only knows u_dst = unit 1) */
        if (r->prog[PROG_VIDEO]) {
                g->glUseProgram(r->prog[PROG_VIDEO]);
                int ly = g->glGetUniformLocation(r->prog[PROG_VIDEO], "u_y");
                int lu = g->glGetUniformLocation(r->prog[PROG_VIDEO], "u_u");
                int lv = g->glGetUniformLocation(r->prog[PROG_VIDEO], "u_v");
                int ld = g->glGetUniformLocation(r->prog[PROG_VIDEO], "u_dst");
                if (ly >= 0) g->glUniform1i(ly, 0);
                if (lu >= 0) g->glUniform1i(lu, 1);
                if (lv >= 0) g->glUniform1i(lv, 2);
                if (ld >= 0) g->glUniform1i(ld, 3);
        }
        /* PROG_F_ADV_DYE samples BOTH u_dye and u_vel — the generic loop
         * above bound both to unit 0, so the dye advection read the DYE
         * texture as its velocity field (dye drifted by its own brightness
         * instead of following the fluid). Its velocity comes from unit 1. */
        if (r->prog[PROG_F_ADV_DYE]) {
                g->glUseProgram(r->prog[PROG_F_ADV_DYE]);
                int lv = g->glGetUniformLocation(r->prog[PROG_F_ADV_DYE], "u_vel");
                if (lv >= 0)
                        g->glUniform1i(lv, 1);
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
        return gl_make_program_vs(r, idx, VS, fs, err, errcap);
}

static bool gl_make_program_vs(RenderGL *r, int idx, const char *vs, const char *fs,
                               char *err, size_t errcap)
{
        GL *g = &r->g;
        GLuint_ v = g->glCreateShader(GL_VERTEX_SHADER);
        GLuint_ f = g->glCreateShader(GL_FRAGMENT_SHADER);
        const char *vsrc = vs, *fsrc = fs;
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

        /* GPU frame timing: poll the query RING (non-blocking; results lag
         * several frames — asynchronous execution, reported as such), then
         * arm this frame's slot. Each ring slot is only re-armed after its
         * previous result was COLLECTED: re-arming a query with an
         * un-retrieved result is a GL INVALID_OPERATION and, observed in
         * the field, permanently stalls sample collection. */
        if (r->tq_have) {
                bool any_collected = false;
                for (int k = 0; k < TQ_RING; k++) {
                        GLuint_ q = r->tq[win][k];
                        if (q && r->tq_pending[win][k]) {
                                GLuint64_ avail = 0, res = 0;
                                g->glGetQueryObjectu64vQ(q, GL_QUERY_RESULT_AVAILABLE_Q, &avail);
                                if (avail) {
                                        g->glGetQueryObjectu64vQ(q, GL_QUERY_RESULT_Q, &res);
                                        r->perf_gpu_ms_sum += (f64)res / 1000000.0;
                                        r->perf_gpu_samples++;
                                        r->tq_pending[win][k] = false;
                                        any_collected = true;
                                }
                        }
                }
                if (r->tq_next[win] >= TQ_RING)
                        r->tq_next[win] = 0;
                int slot = r->tq_next[win];
                if (r->tq_pending[win][slot]) {
                        /* this slot's result has not landed yet — rotate to
                         * a free one instead of forcing the INVALID_OPERATION */
                        for (int k = 0; k < TQ_RING && r->tq_pending[win][slot]; k++) {
                                slot = (slot + 1) % TQ_RING;
                        }
                        r->tq_next[win] = slot;
                }
                r->tq_armed_now[win] = false;
                if (!r->tq_pending[win][slot]) {
                        if (!r->tq[win][slot])
                                g->glGenQueriesQ(1, &r->tq[win][slot]);
                        if (r->tq[win][slot]) {
                                g->glBeginQueryQ(GL_TIME_ELAPSED_Q, r->tq[win][slot]);
                                r->tq_pending[win][slot] = true;
                                r->tq_armed_now[win] = true;
                        }
                        r->tq_next[win] = (slot + 1) % TQ_RING;
                }
                /* consecutive-miss watchdog: symbols exist, queries are
                 * armed, but no result ever arrives — say so and stop
                 * claiming GPU timing instead of printing 0.0ms forever */
                if (any_collected)
                        r->tq_misses = 0;
                else if (r->perf_gpu_samples == 0 && ++r->tq_misses == 600) {
                        r->tq_have = false;
                        LOGW("opengl: GPU timer queries produced no results "
                             "after 600 frames (driver issue) — GPU timing "
                             "disabled; REV_PROFILE will not report it");
                }
        }
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
        if (!cmd || !cmd->w || !cmd->h)
                return false;

        /* 0. GPU ripple layers: height-field step + refraction composite in
         *    ONE specialized pass — the refraction reads the accumulation
         *    target (the below-composite, exactly what ripple_apply() gets
         *    handed on the CPU) and writes the flipped accum, so the normal
         *    blur/composite machinery below never touches it. */
        if (cmd->rsim) {
                if (!r->sims_ok)
                        return false;    /* scene should not have offloaded */
                GLRipple *rp = ripple_slot(r, r->cur_win, cmd->rsim);
                if (!rp)
                        return false;
                GLuint_ field = gl_ripple_step(r, rp, cmd->rsim);
                if (!field)
                        return false;
                int wi = r->cur_win;
                RTarget *accum_rd = &r->accum[wi][r->accum_cur[wi]];
                RTarget *accum_wr = &r->accum[wi][r->accum_cur[wi] ^ 1];
                g->glBindFramebuffer(GL_FRAMEBUFFER, accum_wr->fbo);
                g->glViewport(0, 0, (int)accum_wr->w, (int)accum_wr->h);
                g->glUseProgram(r->prog[PROG_R_COMPOSITE]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, field);
                g->glActiveTexture(GL_TEXTURE1);
                g->glBindTexture(GL_TEXTURE_2D, accum_rd->tex);
                g->glActiveTexture(GL_TEXTURE0);
                {
                        int pi = PROG_R_COMPOSITE;
                        const double strength = cmd->rsim->strength < 0.1 ? 0.1 :
                                                cmd->rsim->strength > 8.0 ? 8.0 :
                                                cmd->rsim->strength;
                        g->glUniform2f(r->ru[pi].grid, (float)rp->gw, (float)rp->gh);
                        g->glUniform2f(r->ru[pi].amp,
                                       (float)(strength * 26.0 / (double)accum_wr->w),
                                       (float)(strength * 26.0 / (double)accum_wr->h));
                        g->glUniform1f(r->ru[pi].opacity,
                                       (float)(cmd->opacity < 0 ? 0 :
                                               cmd->opacity > 1 ? 1 : cmd->opacity));
                }
                draw_full(g);
                r->accum_cur[wi] ^= 1;
                return true;
        }

        /* 1. obtain the layer's source texture:
         *    - GPU-owned fluid layers are simulated here (no CPU pixels,
         *      no upload — the whole per-pixel pipeline stays on the GPU)
         *    - GPU particle layers rasterize point sprites here (the CPU
         *      ships ~128KB of packed floats, not an 8MB full-frame raster)
         *    - pixel layers go through the content-addressed cache: static
         *      rasters re-upload ONLY when their content serial changes
         *      (the old path pushed every cached raster every frame). */
        GLuint_ src_tex;
        if (cmd->vsim) {
                /* video: upload Y/U/V planes as R8 textures and composite
                 * through the dedicated YUV->RGB blend program (placement
                 * math shared with the standard composite below) */
                const RSimVideo *vd = cmd->vsim;
                g->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                g->glPixelStorei(GL_UNPACK_ROW_LENGTH, vd->ls_y);
                src_tex = 0;
                if (!r->vtex_y)
                        g->glGenTextures(1, &r->vtex_y);
                if (!r->vtex_u)
                        g->glGenTextures(1, &r->vtex_u);
                if (!r->vtex_v)
                        g->glGenTextures(1, &r->vtex_v);
                g->glBindTexture(GL_TEXTURE_2D, r->vtex_y);
                if (r->vtex_w != vd->w) {
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (int)vd->w, (int)vd->h, 0,
                                        GL_RED, GL_UNSIGNED_BYTE, NULL);
                        r->vtex_w = vd->w;
                }
                f64 up_t0 = rev_monotonic();
                g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)vd->w, (int)vd->h,
                                   GL_RED, GL_UNSIGNED_BYTE, vd->y);
                g->glPixelStorei(GL_UNPACK_ROW_LENGTH, vd->ls_u);
                g->glBindTexture(GL_TEXTURE_2D, r->vtex_u);
                {
                        u32 cw = vd->w / 2, ch = vd->h / 2;
                        if (cw < 1) cw = 1;
                        if (ch < 1) ch = 1;
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (int)cw, (int)ch, 0,
                                        GL_RED, GL_UNSIGNED_BYTE, NULL);
                        g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cw, (int)ch,
                                           GL_RED, GL_UNSIGNED_BYTE, vd->u);
                }
                g->glPixelStorei(GL_UNPACK_ROW_LENGTH, vd->ls_v);
                g->glBindTexture(GL_TEXTURE_2D, r->vtex_v);
                {
                        u32 cw = vd->w / 2, ch = vd->h / 2;
                        if (cw < 1) cw = 1;
                        if (ch < 1) ch = 1;
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (int)cw, (int)ch, 0,
                                        GL_RED, GL_UNSIGNED_BYTE, NULL);
                        g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cw, (int)ch,
                                           GL_RED, GL_UNSIGNED_BYTE, vd->v);
                }
                r->perf_upload_ms += (rev_monotonic() - up_t0) * 1000.0;
                g->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                g->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                /* composite: planes + accum -> flipped accum */
                int wi = r->cur_win;
                u32 pw = cmd->place_w ? cmd->place_w : cmd->w;
                u32 ph = cmd->place_h ? cmd->place_h : cmd->h;
                RTarget *accum_rd = &r->accum[wi][r->accum_cur[wi]];
                RTarget *accum_wr = &r->accum[wi][r->accum_cur[wi] ^ 1];
                g->glBindFramebuffer(GL_FRAMEBUFFER, accum_wr->fbo);
                g->glViewport(0, 0, (int)accum_wr->w, (int)accum_wr->h);
                g->glUseProgram(r->prog[PROG_VIDEO]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, r->vtex_y);
                g->glActiveTexture(GL_TEXTURE1);
                g->glBindTexture(GL_TEXTURE_2D, r->vtex_u);
                g->glActiveTexture(GL_TEXTURE2);
                g->glBindTexture(GL_TEXTURE_2D, r->vtex_v);
                g->glActiveTexture(GL_TEXTURE3);
                g->glBindTexture(GL_TEXTURE_2D, accum_rd->tex);
                g->glActiveTexture(GL_TEXTURE0);
                {
                        int pi = PROG_VIDEO;
                        if (r->u[pi].off >= 0)
                                g->glUniform2f(r->u[pi].off,
                                               (float)(-(double)cmd->dx / (double)pw),
                                               (float)(-(double)cmd->dy / (double)ph));
                        if (r->u[pi].scale >= 0)
                                g->glUniform2f(r->u[pi].scale,
                                               (float)((double)accum_wr->w / (double)pw),
                                               (float)((double)accum_wr->h / (double)ph));
                        if (r->vu[pi].blend >= 0)
                                g->glUniform1i(r->vu[pi].blend, cmd->blend);
                        if (r->vu[pi].opacity >= 0)
                                g->glUniform1f(r->vu[pi].opacity,
                                               (float)(cmd->opacity < 0 ? 0 :
                                                       cmd->opacity > 1 ? 1 : cmd->opacity));
                }
                draw_full(g);
                r->accum_cur[wi] ^= 1;
                return true;
        }
        if (cmd->grad) {
                src_tex = gl_grad_layer(r, cmd);
                if (!src_tex)
                        return false;
        } else if (cmd->psim) {
                src_tex = gl_particles_layer(r, cmd);
                if (!src_tex)
                        return false;
        } else if (cmd->sim) {
                if (!r->sims_ok)
                        return false;    /* scene should not have offloaded */
                GLFluid *f = fluid_slot(r, r->cur_win, cmd->sim);
                if (!f)
                        return false;
                src_tex = gl_fluid_step(r, f, cmd->sim);
                if (!src_tex)
                        return false;
                r->src_w = cmd->w;
                r->src_h = cmd->h;
        } else {
                if (!cmd->px)
                        return false;
                src_tex = upload_layer_tex(r, cmd);
                if (!src_tex)
                        return false;
        }
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, src_tex);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                           cmd->linear ? GL_LINEAR : GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                           cmd->linear ? GL_LINEAR : GL_NEAREST);

        /* 2. blur (downsample + separable 9-tap + upscale). The blur chain
         *    operates on PREMULTIPLIED pixels (gaussian across alpha edges);
         *    unblurred layers composite directly from their STRAIGHT source
         *    — no premult round-trip, so faint semi-transparent pixels keep
         *    their exact 8-bit values like the CPU path. */
        GLuint_ layer_tex = src_tex;
        bool src_premul = false;
        if (cmd->blur >= 0.5) {
                /* premultiply into tex_a (raster size) first */
                if (!ensure_tex(g, &r->tex_a, &r->a_w, &r->a_h, cmd->w, cmd->h, true))
                        return false;
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, src_tex);

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
                src_premul = true;
        }

        /* 4. composite: read accum[cur], write accum[cur^1], flip.
         * Inverse placement map — the fragment at accum position s samples
         * the layer at uvsrc = (s*frame - (dx,dy)) / (pw,ph), so the layer's
         * [0,1] uv lands exactly on its placed rect (1:1, dy from top) and
         * everything outside the rect discards. The old forward map
         * (scale=pw/W, off=dx/W) was only correct for fullscreen layers —
         * centered/oversize layers rendered zoomed with edge smear instead
         * of placed. Placement may exceed the frame (sub-frame-res sims
         * stretched over it): uvsrc stays within [0,1] across the window and
         * the linear filter upsamples while compositing. */
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
                               (float)(-(double)cmd->dx / (double)pw),
                               (float)(-(double)cmd->dy / (double)ph));
        if (r->u[PROG_COMPOSITE].scale >= 0)
                g->glUniform2f(r->u[PROG_COMPOSITE].scale,
                               (float)((double)accum_wr->w / (double)pw),
                               (float)((double)accum_wr->h / (double)ph));
        if (r->u[PROG_COMPOSITE].blend >= 0)
                g->glUniform1i(r->u[PROG_COMPOSITE].blend, cmd->blend);
        if (r->u[PROG_COMPOSITE].src_premul >= 0)
                g->glUniform1i(r->u[PROG_COMPOSITE].src_premul, src_premul ? 1 : 0);
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
                /* CPU-composited mode (scene needs CPU compositing — e.g.
                 * ripple layers when the renderer declined GPU sims): begin()
                 * never ran, so THIS path must make itself current, ensure
                 * the window surface exists, and set the present viewport
                 * from the frame. The old code read win[win].w/h — which
                 * only begin() sets — presenting into a 0x0 viewport, i.e.
                 * a BLACK SCREEN, whenever a ripple scene fell back to CPU
                 * compositing on the GL tier (found by the 2026-10 parity
                 * sweep; every other scene type composites in GPU mode and
                 * masked it). */
                if (!gl_window_surface(r, win))
                        return false;
                if (!r->eglMakeCurrent(r->dpy, r->win[win].surf, r->win[win].surf, r->ctx))
                        return false;
                r->win[win].w = cpu_frame->w;
                r->win[win].h = cpu_frame->h;
                /* upload the CPU-composited straight frame and present it
                 * directly (the accum/present chain is straight-alpha) */
                if (!ensure_tex(g, &r->tex_src, &r->src_w, &r->src_h, cpu_frame->w, cpu_frame->h, false))
                        return false;
                g->glBindTexture(GL_TEXTURE_2D, r->tex_src);
                f64 up_t0 = rev_monotonic();
                g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cpu_frame->w, (int)cpu_frame->h,
                                   GL_RGBA, GL_UNSIGNED_BYTE, cpu_frame->px);
                r->perf_upload_ms += (rev_monotonic() - up_t0) * 1000.0;
                g->glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g->glViewport(0, 0, (int)r->win[win].w, (int)r->win[win].h);
                g->glUseProgram(r->prog[PROG_PRESENT]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, r->tex_src);
                /* v flip: CPU rasters are top-down, GL window space is y-up */
                if (r->u[PROG_PRESENT].off >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].off, 0, 1);
                if (r->u[PROG_PRESENT].scale >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].scale, 1, -1);
                draw_full(g);
        } else {
                /* present the latest accum (premul -> straight) */
                RTarget *accum = &r->accum[win][r->accum_cur[win]];
                g->glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g->glViewport(0, 0, (int)r->win[win].w, (int)r->win[win].h);
                g->glUseProgram(r->prog[PROG_PRESENT]);
                g->glActiveTexture(GL_TEXTURE0);
                g->glBindTexture(GL_TEXTURE_2D, accum->tex);
                /* v flip — same reason (accum rows follow CPU top-down order) */
                if (r->u[PROG_PRESENT].off >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].off, 0, 1);
                if (r->u[PROG_PRESENT].scale >= 0)
                        g->glUniform2f(r->u[PROG_PRESENT].scale, 1, -1);
                draw_full(g);
        }
        r->frame_no++;
        bool swapped = r->eglSwapBuffers(r->dpy, r->win[win].surf) != 0;
        if (r->tq_have && r->tq_armed_now[win])
                g->glEndQueryQ(GL_TIME_ELAPSED_Q);
        return swapped;
}

static bool gl_perf(RRenderer *rr, f64 *cpu_ms, f64 *gpu_ms)
{
        RenderGL *r = (RenderGL *)rr->ctx;
        if (cpu_ms)
                *cpu_ms = r->perf_upload_ms;
        if (gpu_ms)
                *gpu_ms = r->perf_gpu_samples ?
                          r->perf_gpu_ms_sum / (f64)r->perf_gpu_samples : 0.0;
        r->perf_upload_ms = 0;
        r->perf_gpu_ms_sum = 0;
        r->perf_gpu_samples = 0;
        return true;
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
                /* fluid + ripple solver state + upload cache */
                for (int i = 0; i < REV_PLATFORM_MAX_OUTPUTS; i++) {
                        for (int j = 0; j < GL_MAX_FLUIDS; j++) {
                                GLFluid *f = &r->fluids[i][j];
                                if (!f->key)
                                        continue;
                                fluid_free_pair(g, f->vel, f->vel_fbo);
                                fluid_free_pair(g, f->dye, f->dye_fbo);
                                fluid_free_pair(g, f->press, f->press_fbo);
                                if (f->curl) {
                                        g->glDeleteTextures(1, &f->curl);
                                        g->glDeleteFramebuffers(1, &f->curl_fbo);
                                }
                                if (f->out8) {
                                        g->glDeleteTextures(1, &f->out8);
                                        g->glDeleteFramebuffers(1, &f->out8_fbo);
                                }
                                f->key = NULL;
                                f->ok = false;
                        }
                        for (int j = 0; j < GL_MAX_RIPPLES; j++) {
                                GLRipple *rp = &r->ripples[i][j];
                                if (!rp->key)
                                        continue;
                                fluid_free_pair(g, rp->field, rp->fbo);
                                rp->key = NULL;
                                rp->ok = false;
                        }
                }
                for (int i = 0; i < GL_TEX_CACHE; i++) {
                        if (r->texcache[i].tex)
                                g->glDeleteTextures(1, &r->texcache[i].tex);
                        r->texcache[i].tex = 0;
                }
                /* GPU particle layer resources */
                if (r->ptex)
                        g->glDeleteTextures(1, &r->ptex);
                if (r->ptarget.tex) {
                        g->glDeleteTextures(1, &r->ptarget.tex);
                        g->glDeleteFramebuffers(1, &r->ptarget.fbo);
                }
                /* rendered-gradient cache */
                for (int i = 0; i < 8; i++) {
                        if (r->rgcache[i].t.tex) {
                                g->glDeleteTextures(1, &r->rgcache[i].t.tex);
                                g->glDeleteFramebuffers(1, &r->rgcache[i].t.fbo);
                        }
                }
                /* video plane textures */
                if (r->vtex_y)
                        g->glDeleteTextures(1, &r->vtex_y);
                if (r->vtex_u)
                        g->glDeleteTextures(1, &r->vtex_u);
                if (r->vtex_v)
                        g->glDeleteTextures(1, &r->vtex_v);
                /* pending timer queries need no teardown: the context dies */
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
        rr->add_window = gl_add_window;
        rr->remove_window = gl_remove_window;
        rr->perf = gl_perf;
        return rr;
}
