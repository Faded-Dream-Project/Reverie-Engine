/* ============================================================================
 * Reverie Engine — main.c
 * The reverie daemon: loads a .rev scene (text or .revpack), picks a display
 * backend (raw X11 or raw Wayland), picks a renderer (vulkan -> opengl ->
 * opengl-on-llvmpipe -> cpu), and runs the interactive frame loop.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arena.h"
#include "log.h"
#include "pack.h"
#include "platform.h"
#include "png.h"
#include "render.h"
#include "rev.h"
#include "scene.h"
#include "util.h"

static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
        (void)sig;
        g_run = 0;
}

/* ---- perf stats helpers ---------------------------------------------------- */

#define PERF_RING 512

static void perf_sort(f64 *a, u32 n)
{
        /* insertion sort: n <= 512, called once per 5s window — plenty */
        for (u32 i = 1; i < n; i++) {
                f64 v = a[i];
                i32 j = (i32)i - 1;
                while (j >= 0 && a[j] > v) {
                        a[j + 1] = a[j];
                        j--;
                }
                a[j + 1] = v;
        }
}

static f64 perf_pct(const f64 *sorted, u32 n, f64 p)
{
        if (n == 0)
                return 0.0;
        u32 idx = (u32)(p * (f64)(n - 1) + 0.5);
        return sorted[idx];
}

static void usage(void)
{
        printf(
"reverie " REV_VERSION " — interactive wallpaper engine\n"
"\n"
"usage: reverie [options] <scene.rev | scene.revpack>\n"
"\n"
"options:\n"
"  -R, --renderer R    force renderer: auto | vulkan | opengl | cpu\n"
"  -B, --backend B     force backend: auto | x11 | wayland\n"
"  -f, --fps N         override the scene's target fps\n"
"  -m, --monitor N     render only on monitor N\n"
"  -o, --once          render one frame to PNG and exit (no display needed)\n"
"  -s, --size WxH      output size for --once (default 1920x1080)\n"
"  -O, --out FILE      output PNG path for --once (default reverie.png)\n"
"  -l, --list          list monitors and exit\n"
"      --shot FILE      screenshot every window to FILE and keep running\n"
"                       (E2E test hook; needs a compositor readback)\n"
"      --shot-after N   seconds to wait before the screenshot\n"
"                       (--shot N FILE is also accepted)\n"
"  -v, --verbose       debug logging\n"
"  -q, --quiet         errors only\n"
"  -h, --help          this help\n"
"\n"
"environment:\n"
"  REV_RENDERER    same as --renderer\n"
"  REV_BACKEND     same as --backend\n"
"  REV_VK_PRESENT  wsi = vulkan tier presents via its swapchain\n"
"                  (default: the platform's proven SHM present path)\n"
"  REV_WAYLAND_GPU 0 = opt OUT of the Wayland GPU WSI shim and use the\n"
"                  raw hand-written client (default: the shim is active\n"
"                  whenever libwayland-client loads, so a present GPU is\n"
"                  actually used)\n"
"  REV_GL_FLUID_SW 1 = run the GPU fluid solver even on software GL\n"
"                  (llvmpipe test hook — normally software keeps the CPU\n"
"                  solver, which is faster there)\n"
"  REV_WL_TRACE    1 = dump raw Wayland wire traffic to stderr\n"
"\n"
"renderer chain (auto): vulkan -> opengl -> opengl-on-llvmpipe -> cpu.\n");
}

typedef struct {
        RArena *ir_arena;
        RSceneIR *ir;
        RTScene *sc;
        f64 mtime;
        char path[512];
        RevPack pack;        /* valid when loaded from a .revpack */
        bool pack_used;
} SceneFile;

static const u8 *pack_asset_lookup(void *ctx, const char *path, size_t *len)
{
        return rev_pack_asset((const RevPack *)ctx, path, len);
}

static bool scene_load(SceneFile *sf, const char *path, const char *base_dir,
                       char *err, size_t errcap)
{
        size_t len = 0;
        u8 *text = NULL;
        RArena *ar = NULL;
        RSceneIR *ir = NULL;
        RevPack pk;
        bool is_pack = rev_pack_probe(path);

        if (is_pack) {
                if (!rev_pack_load(path, &pk, err, errcap))
                        return false;
                len = pk.scene_len;
                text = (u8 *)malloc(len + 1);
                if (!text) {
                        rev_pack_free(&pk);
                        snprintf(err, errcap, "out of memory");
                        return false;
                }
                memcpy(text, pk.scene_text, len);
                text[len] = 0;
        } else {
                text = rev_read_file(path, &len);
                if (!text) {
                        snprintf(err, errcap, "cannot read '%s'", path);
                        return false;
                }
        }

        ir = rev_parse_scene((const char *)text, len, &ar, err, errcap);
        free(text);
        if (!ir) {
                if (is_pack)
                        rev_pack_free(&pk);
                return false;
        }

        RTScene *sc = rtscene_compile(ir, ar, base_dir, err, errcap);
        if (!sc) {
                arena_destroy(ar);
                if (is_pack)
                        rev_pack_free(&pk);
                return false;
        }

        /* free the previous scene, if any */
        if (sf->sc)
                rtscene_free(sf->sc);
        if (sf->ir_arena)
                arena_destroy(sf->ir_arena);
        if (sf->pack_used)
                rev_pack_free(&sf->pack);

        if (is_pack) {
                /* route asset loads through the pack first, disk second */
                rtscene_set_asset_provider(sc, &sf->pack, pack_asset_lookup);
                sf->pack = pk;
                sf->pack_used = true;
        } else {
                sf->pack_used = false;
        }

        sf->sc = sc;
        sf->ir_arena = ar;
        sf->ir = ir;
        snprintf(sf->path, sizeof sf->path, "%s", path);
        sf->mtime = rev_file_mtime(path);
        return true;
}

int main(int argc, char **argv)
{
        const char *scene_path = NULL;
        const char *renderer_pref = NULL;
        const char *backend_pref = NULL;
        int fps_override = 0;
        int only_monitor = -1;
        bool once = false, list = false;
        f64 shot_after = -1;
        const char *shot_file = NULL;
        u32 out_w = 1920, out_h = 1080;
        const char *out_png = "reverie.png";
        char err[512] = { 0 };

        for (int i = 1; i < argc; i++) {
                if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fps")) {
                        if (i + 1 >= argc) { usage(); return 1; }
                        fps_override = atoi(argv[++i]);
                } else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--monitor")) {
                        if (i + 1 >= argc) { usage(); return 1; }
                        only_monitor = atoi(argv[++i]);
                } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--once")) {
                        once = true;
                } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--size")) {
                        if (i + 1 >= argc) { usage(); return 1; }
                        if (sscanf(argv[++i], "%ux%u", &out_w, &out_h) != 2) {
                                fprintf(stderr, "reverie: bad --size (want WxH)\n");
                                return 1;
                        }
                } else if (!strcmp(argv[i], "-O") || !strcmp(argv[i], "--out")) {
                        if (i + 1 >= argc) { usage(); return 1; }
                        out_png = argv[++i];
                } else if (!strcmp(argv[i], "-R") || !strcmp(argv[i], "--renderer")) {
                        if (i + 1 >= argc) { usage(); return 1; }
                        renderer_pref = argv[++i];
                } else if (!strcmp(argv[i], "-B") || !strcmp(argv[i], "--backend")) {
                        if (i + 1 >= argc) { usage(); return 1; }
                        backend_pref = argv[++i];
                } else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list")) {
                        list = true;
                } else if (!strcmp(argv[i], "--shot")) {
                        /* two accepted forms:
                         *   --shot SECONDS FILE           (legacy, scripts/e2e.sh)
                         *   --shot FILE [--shot-after N]  (as documented in
                         *                                 docs/BACKENDS.md) */
                        if (i + 1 >= argc) { usage(); return 1; }
                        {
                                char *end = NULL;
                                double sec = strtod(argv[i + 1], &end);
                                if (end && *end == 0 && end != argv[i + 1]) {
                                        /* numeric first arg: legacy form */
                                        if (i + 2 >= argc) { usage(); return 1; }
                                        shot_after = sec;
                                        shot_file = argv[i + 2];
                                        i += 2;
                                } else {
                                        shot_file = argv[i + 1];
                                        i += 1;
                                }
                        }
                } else if (!strcmp(argv[i], "--shot-after")) {
                        if (i + 1 >= argc) { usage(); return 1; }
                        shot_after = atof(argv[++i]);
                } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
                        rev_log_set_level(REV_LOG_DEBUG);
                } else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
                        rev_log_set_level(REV_LOG_ERROR);
                } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
                        usage();
                        return 0;
                } else if (argv[i][0] == '-' && argv[i][1]) {
                        fprintf(stderr, "reverie: unknown option '%s'\n", argv[i]);
                        usage();
                        return 1;
                } else if (!scene_path) {
                        scene_path = argv[i];
                } else {
                        fprintf(stderr, "reverie: multiple scenes given\n");
                        return 1;
                }
        }

        if (!renderer_pref) {
                renderer_pref = getenv("REV_RENDERER");
                if (!renderer_pref || !*renderer_pref)
                        renderer_pref = "auto";
        }
        if (!backend_pref) {
                backend_pref = getenv("REV_BACKEND");
                if (!backend_pref || !*backend_pref)
                        backend_pref = "auto";
        }

        if (list) {
                RPlatform *p = platform_create(backend_pref, err, sizeof err);
                if (!p) {
                        fprintf(stderr, "reverie: %s\n", err);
                        return 1;
                }
                RRect mons[REV_PLATFORM_MAX_OUTPUTS];
                int n = p->monitors(p, mons, REV_PLATFORM_MAX_OUTPUTS);
                printf("%d monitor(s) on %s:\n", n, p->name);
                for (int i = 0; i < n; i++)
                        printf("  [%d] %dx%d at %d,%d (scale %.2f)\n", i, mons[i].w, mons[i].h,
                               mons[i].x, mons[i].y, p->monitor_scale(p, i));
                platform_destroy(p);
                return 0;
        }

        if (!scene_path) {
                usage();
                return 1;
        }

        LOGI("reverie %s — backend=%s renderer=%s", REV_VERSION, backend_pref, renderer_pref);

        SceneFile sf;
        memset(&sf, 0, sizeof sf);
        char *base_dir = rev_path_dirname(scene_path);
        if (!scene_load(&sf, scene_path, base_dir, err, sizeof err)) {
                fprintf(stderr, "reverie: %s\n", err);
                free(base_dir);
                return 1;
        }
        LOGI("scene '%s': %d layer(s), %d fps, %s%s",
             scene_path, rtscene_layer_count(sf.sc), rtscene_fps(sf.sc),
             rtscene_is_animated(sf.sc) ? "animated" : "static",
             sf.pack_used ? " [packed]" : "");
        if (fps_override > 0)
                LOGI("fps override: %d", fps_override);

        /* ---- offline single-frame mode ---- */
        if (once) {
                RFb fb = { (u32 *)calloc((size_t)out_w * out_h, 4), out_w, out_h };
                if (!fb.px) {
                        fprintf(stderr, "reverie: out of memory\n");
                        return 1;
                }
                rtscene_render(sf.sc, &fb, 0, 0.0, 1.0 / 60.0);
                RevImage img = { out_w, out_h, fb.px };
                if (!rev_png_encode_file(out_png, &img, err, sizeof err)) {
                        fprintf(stderr, "reverie: %s\n", err);
                        return 1;
                }
                printf("wrote %s (%ux%u)\n", out_png, out_w, out_h);
                free(fb.px);
                rtscene_free(sf.sc);
                arena_destroy(sf.ir_arena);
                free(base_dir);
                return 0;
        }

        /* ---- session ---- */
        RPlatform *plat = platform_create(backend_pref, err, sizeof err);
        if (!plat) {
                fprintf(stderr, "reverie: %s\n", err);
                return 1;
        }
        LOGI("backend: %s", plat->name);

        RRect mons[REV_PLATFORM_MAX_OUTPUTS];
        int nmon = plat->monitors(plat, mons, REV_PLATFORM_MAX_OUTPUTS);
        if (only_monitor >= 0) {
                if (only_monitor >= nmon) {
                        fprintf(stderr, "reverie: monitor %d does not exist (%d found)\n",
                                only_monitor, nmon);
                        platform_destroy(plat);
                        return 1;
                }
                mons[0] = mons[only_monitor];
                nmon = 1;
        }
        LOGI("%d monitor(s)", nmon);

        /* framebuffers (device pixels) + windows (logical) */
        RFb fbs[REV_PLATFORM_MAX_OUTPUTS];
        u32 fb_scale[REV_PLATFORM_MAX_OUTPUTS];
        int winidx[REV_PLATFORM_MAX_OUTPUTS];
        int any_window = 0;
        for (int i = 0; i < nmon; i++) {
                f64 scale = plat->monitor_scale(plat, i);
                fb_scale[i] = scale > 1.0 ? (u32)(scale + 0.5) : 1;
                fbs[i].w = (u32)(mons[i].w * fb_scale[i]);
                fbs[i].h = (u32)(mons[i].h * fb_scale[i]);
                fbs[i].px = (u32 *)calloc((size_t)fbs[i].w * fbs[i].h, 4);
                winidx[i] = plat->add_window(plat, mons[i]);
                if (winidx[i] < 0) {
                        LOGW("monitor %d: window creation failed", i);
                } else {
                        any_window = 1;
                }
        }
        if (nmon > 0 && !any_window) {
                /* running with zero windows is pointless (and confusing:
                 * the daemon looks alive in a process monitor while the
                 * desktop shows nothing) — bail out with a clear error. */
                fprintf(stderr, "reverie: no windows could be created on %s — exiting\n",
                        plat->name);
                for (int i = 0; i < nmon; i++)
                        free(fbs[i].px);
                platform_destroy(plat);
                rtscene_free(sf.sc);
                if (sf.pack_used)
                        rev_pack_free(&sf.pack);
                arena_destroy(sf.ir_arena);
                free(base_dir);
                return 1;
        }

        /* renderer selection happens AFTER windows exist (surfaces need them) */
        RRenderer *rend = renderer_create(renderer_pref, plat, err, sizeof err);
        if (!rend) {
                fprintf(stderr, "reverie: %s\n", err);
                platform_destroy(plat);
                return 1;
        }
        LOGI("renderer: %s (tier %d, %s)", rend->name, rend->tier,
             rend->wants_layers ? "GPU compositing" : "CPU compositing");
        if (!rend->wants_layers && strcmp(rend->name, "vulkan") == 0)
                LOGI("vulkan tier: present path follows REV_VK_PRESENT (default: platform SHM)");

        const bool gpu_layers = rend->wants_layers && rtscene_gpu_capable(sf.sc);
        if (rend->wants_layers && !gpu_layers)
                LOGI("scene needs CPU compositing (ripple layers present)");
        /* sim offload: only when the renderer owns simulations (GL tier with
         * float render targets) — the scene then skips the CPU fluid solver
         * entirely and emits GPU descriptors instead */
        const bool sim_offload = rend->wants_sims;
        if (gpu_layers)
                LOGI("fluid simulation: %s",
                     sim_offload ? "GPU (no per-pixel CPU work)"
                                 : "CPU (renderer declined GPU sims)");

        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);

        f64 t0 = rev_monotonic();
        f64 last_frame = t0;
        f64 last_reload_check = t0;
        f64 last_stat = t0;
        u32 frames = 0;
        u64 last_drops = 0;
        bool shot_done = false;
        RTFrameList fl;
        /* REV_PROFILE=1: stage breakdown appended to the periodic perf line.
         * Averages LIE on a contended box (one 300ms stall poisons a 5s
         * mean while fps barely moves), so frame times ALSO report
         * p50/p95/max from a ring buffer, and GPU time comes from the
         * renderer's ASYNC timer queries — labeled as such, never conflated
         * with CPU wall time. This is the field-triage tool for "why is my
         * wallpaper slow" reports. */
        const bool prof_on = getenv("REV_PROFILE") != NULL;
        f64 prof_sim = 0, prof_raster = 0, prof_render = 0;
        f64 prof_upload = 0, prof_gpu = 0;
        u32 prof_gpu_n = 0;
        bool gpu_note_shown = false;
        f64 frame_ms[PERF_RING];
        u32 nframe_ms = 0;

        /* REV_FAKE_POINTER=<speed>: TEST/ BENCH hook — synthesizes a pressed
         * pointer moving on a Lissajous path (speed = circuits/s) so
         * interactive scenes (fluid drag, ripple refraction, spawn=both
         * particles) can be benchmarked headlessly under Xvfb. Never active
         * unless the env var is set explicitly. */
        f64 fake_ptr_speed = 0;
        {
                const char *fp = getenv("REV_FAKE_POINTER");
                if (fp && *fp)
                        fake_ptr_speed = atof(fp);
        }
        f64 fpx = 0.5, fpy = 0.5;
        if (fake_ptr_speed > 0)
                LOGI("fake pointer active (test hook): %.2f circuit/s", fake_ptr_speed);

        while (g_run) {
                f64 now = rev_monotonic();
                f64 dt = now - last_frame;
                /* dt must span START-to-START (render + pacing wait): the
                 * old code set last_frame AFTER the pacing wait, so dt was
                 * the few microseconds between wait-end and loop-top — every
                 * simulation (fluid/ripple/particles) stepped with dt≈0 and
                 * effectively froze (only wall-clock-t animations moved). */
                last_frame = now;
                f64 t = now - t0;
                if (prof_on && nframe_ms < PERF_RING)
                        frame_ms[nframe_ms++] = dt * 1000.0;

                plat->pump(plat);
                if (plat->should_close(plat))
                        break;

                /* window resize handling: recreate in place, notify renderer */
                for (int i = 0; i < nmon; i++) {
                        RRect g;
                        if (winidx[i] >= 0 && plat->win_geometry(plat, winidx[i], &g)) {
                                u32 nw = (u32)(g.w * fb_scale[i]);
                                u32 nh = (u32)(g.h * fb_scale[i]);
                                if (nw != fbs[i].w || nh != fbs[i].h) {
                                        LOGI("monitor %d: geometry now %ux%u, re-creating", i, g.w, g.h);
                                        free(fbs[i].px);
                                        fbs[i].w = nw;
                                        fbs[i].h = nh;
                                        fbs[i].px = (u32 *)calloc((size_t)nw * nh, 4);
                                        plat->resize_window(plat, winidx[i], g);
                                        rend->resize(rend, i, nw, nh);
                                        rtscene_flush_caches(sf.sc);
                                }
                        }
                }

                /* pointer (consume-on-read: dx/dy and click edges are each
                 * delivered exactly once by the platform layer) */
                RPointerState ptr;
                if (plat->pointer(plat, &ptr)) {
                        for (int i = 0; i < nmon; i++) {
                                f64 lx = (ptr.x - mons[i].x) * fb_scale[i];
                                f64 ly = (ptr.y - mons[i].y) * fb_scale[i];
                                f64 dx = ptr.dx * fb_scale[i];
                                f64 dy = ptr.dy * fb_scale[i];
                                rtscene_pointer(sf.sc, i, lx, ly, dx, dy, ptr.pressed);
                                /* a click whose press AND release both landed
                                 * between two polls is still a click — splash
                                 * it at the latched press position */
                                if (ptr.click)
                                        rtscene_click(sf.sc, i,
                                                      (ptr.click_x - mons[i].x) * fb_scale[i],
                                                      (ptr.click_y - mons[i].y) * fb_scale[i]);
                        }
                }

                /* synthetic pointer (test hook): overrides the real one —
                 * benchmarks need a deterministic drag, not a resting cursor */
                if (fake_ptr_speed > 0) {
                        f64 nx = 0.5 + 0.36 * sin(2.0 * M_PI * 0.13 * fake_ptr_speed * t);
                        f64 ny = 0.5 + 0.30 * sin(2.0 * M_PI * 0.21 * fake_ptr_speed * t);
                        for (int i = 0; i < nmon; i++) {
                                rtscene_pointer(sf.sc, i, nx * fbs[i].w, ny * fbs[i].h,
                                                (nx - fpx) * fbs[i].w,
                                                (ny - fpy) * fbs[i].h, true);
                        }
                        fpx = nx;
                        fpy = ny;
                }

                /* render + present */
                for (int i = 0; i < nmon; i++) {
                        if (winidx[i] < 0 || !fbs[i].px)
                                continue;
                        f64 r_t0 = rev_monotonic();
                        if (gpu_layers) {
                                fl.w = fbs[i].w;
                                fl.h = fbs[i].h;
                                fl.n = 0;
                                fl.sim_offload = sim_offload;
                                rtscene_render_layers(sf.sc, &fl, i, t, dt);
                                f64 s_ms, ra_ms;
                                rtscene_profile(sf.sc, &s_ms, &ra_ms);
                                prof_sim += s_ms;
                                prof_raster += ra_ms;
                                if (!rend->begin(rend, winidx[i], fbs[i].w, fbs[i].h))
                                        continue;
                                for (int k = 0; k < fl.n; k++) {
                                        RLayerCmd cmd;
                                        cmd.px = fl.cmds[k].px;
                                        cmd.w = fl.cmds[k].w;
                                        cmd.h = fl.cmds[k].h;
                                        cmd.place_w = fl.cmds[k].place_w;
                                        cmd.place_h = fl.cmds[k].place_h;
                                        cmd.blend = fl.cmds[k].blend;
                                        cmd.opacity = fl.cmds[k].opacity;
                                        cmd.blur = fl.cmds[k].blur;
                                        cmd.dx = fl.cmds[k].dx;
                                        cmd.dy = fl.cmds[k].dy;
                                        cmd.linear = fl.cmds[k].linear;
                                        cmd.content_id = fl.cmds[k].content_id;
                                        cmd.sim = fl.cmds[k].sim;
                                        rend->layer(rend, &cmd);
                                }
                                rend->end(rend, NULL);
                        } else {
                                memset(fbs[i].px, 0, (size_t)fbs[i].w * fbs[i].h * 4);
                                rtscene_render(sf.sc, &fbs[i], i, t, dt);
                                f64 s_ms, ra_ms;
                                rtscene_profile(sf.sc, &s_ms, &ra_ms);
                                prof_sim += s_ms;
                                prof_raster += ra_ms;
                                rend->end(rend, &fbs[i]);
                        }
                        prof_render += rev_monotonic() - r_t0;
                }

                /* renderer-side counters (once per frame, after all windows):
                 * upload wall time (a SUBSET of render above) and the async
                 * GPU timer result for the PREVIOUS frame */
                if (rend->perf) {
                        f64 up = 0, gp = 0;
                        if (rend->perf(rend, &up, &gp)) {
                                prof_upload += up;
                                if (gp > 0) {
                                        prof_gpu += gp;
                                        prof_gpu_n++;
                                }
                        }
                }

                /* test hook: screenshot */
                if (shot_after >= 0 && t >= shot_after && !shot_done) {
                        shot_done = true;
                        for (int i = 0; i < nmon; i++) {
                                if (winidx[i] < 0)
                                        continue;
                                u32 sw = 0, sh = 0;
                                u32 *shot = plat->screenshot(plat, winidx[i], &sw, &sh);
                                if (shot) {
                                        char path[600];
                                        if (nmon > 1)
                                                snprintf(path, sizeof path, "%s.%d.png", shot_file, i);
                                        else
                                                snprintf(path, sizeof path, "%s.png", shot_file);
                                        RevImage img = { sw, sh, shot };
                                        char e2[256] = { 0 };
                                        if (rev_png_encode_file(path, &img, e2, sizeof e2))
                                                LOGI("screenshot: %s (%ux%u)", path, sw, sh);
                                        else
                                                LOGW("screenshot failed: %s", e2);
                                        free(shot);
                                } else {
                                        LOGW("screenshot: readback unavailable on %s", plat->name);
                                }
                        }
                }

                /* hot reload */
                if (now - last_reload_check > 0.5) {
                        last_reload_check = now;
                        f64 mt = rev_file_mtime(sf.path);
                        if (mt > 0 && mt != sf.mtime) {
                                LOGI("scene changed on disk, reloading...");
                                if (scene_load(&sf, sf.path, base_dir, err, sizeof err)) {
                                        LOGI("reload ok (%d layers)", rtscene_layer_count(sf.sc));
                                        if (fps_override <= 0)
                                                LOGI("fps now %d", rtscene_fps(sf.sc));
                                } else {
                                        LOGW("reload failed: %s (keeping old scene)", err);
                                }
                        }
                }

                /* periodic perf stat — the field-debug line: actual frame rate
                 * plus any presents the compositor dropped (both wl_shm slots
                 * still held when the next frame was ready) */
                if (now - last_stat >= 5.0) {
                        u64 drops = plat->dropped_frames ? plat->dropped_frames(plat) : 0;
                        f64 win = now - last_stat;
                        f64 fps = (f64)frames / win;
                        LOGI("perf: %.1f fps", fps);
                        if (prof_on) {
                                f64 nf = frames ? (f64)frames : 1;
                                LOGI("perf: sim=%.1fms raster=%.1fms render=%.1fms "
                                     "(incl upload=%.1fms) gpu=%.1fms*",
                                     prof_sim / nf, prof_raster / nf,
                                     prof_render / nf * 1000.0,
                                     prof_upload / nf,
                                     prof_gpu_n ? prof_gpu / (f64)prof_gpu_n : 0.0);
                                if (!gpu_note_shown) {
                                        gpu_note_shown = true;
                                        if (prof_gpu_n == 0)
                                                LOGI("perf: *gpu timing unavailable "
                                                     "(renderer/driver lacks timer queries)");
                                        else
                                                LOGI("perf: *gpu = async driver timer, "
                                                     "one frame behind — not CPU wall time");
                                }
                                /* frame-time distribution: the avg hides
                                 * multi-hundred-ms stalls; p95/max expose
                                 * them, p50 shows the typical frame */
                                f64 tmp[PERF_RING];
                                u32 nn = nframe_ms < PERF_RING ? nframe_ms : PERF_RING;
                                f64 avg = 0;
                                for (u32 i = 0; i < nn; i++)
                                        avg += frame_ms[i];
                                if (nn)
                                        avg /= (f64)nn;
                                memcpy(tmp, frame_ms, nn * sizeof *tmp);
                                perf_sort(tmp, nn);
                                LOGI("perf: frame avg=%.1f p50=%.1f p95=%.1f max=%.1f ms",
                                     avg, perf_pct(tmp, nn, 0.50),
                                     perf_pct(tmp, nn, 0.95),
                                     nn ? tmp[nn - 1] : 0.0);
                        }
                        if (drops > last_drops) {
                                LOGI("perf: %llu frame(s) dropped by the compositor",
                                     (unsigned long long)(drops - last_drops));
                                last_drops = drops;
                        }
                        frames = 0;
                        prof_sim = prof_raster = prof_render = 0;
                        prof_upload = prof_gpu = 0;
                        prof_gpu_n = 0;
                        nframe_ms = 0;
                        last_stat = now;
                }

                /* frame pacing: hold until the next frame boundary — through
                 * wait_frame() when the platform provides it, so input keeps
                 * flowing during the wait and (Wayland) the wake aligns with
                 * the compositor's repaint cycle instead of a blind nanosleep.
                 * A slow frame can no longer swallow the events that arrived
                 * while it rendered. */
                int fps = fps_override > 0 ? fps_override : rtscene_fps(sf.sc);
                if (fps_override <= 0 && plat->monitor_refresh) {
                        /* cap scene fps to the fastest connected display — the
                         * extra frames could never be shown (they would just
                         * pile up as compositor drops). An explicit --fps
                         * override is always honored as-is. */
                        int hz = 0;
                        for (int i = 0; i < nmon; i++) {
                                int r = plat->monitor_refresh(plat, i);
                                if (r > hz)
                                        hz = r;
                        }
                        if (hz > 0 && fps > hz) {
                                LOGD("pacing: scene %d fps capped to display %d Hz", fps, hz);
                                fps = hz;
                        }
                }
                if (fps < 1) fps = 1;
                if (fps > 240) fps = 240;
                f64 frame_len = 1.0 / fps;
                f64 deadline = now + frame_len;
                for (;;) {
                        f64 rem = deadline - rev_monotonic();
                        if (rem <= 0.0005)
                                break;
                        if (plat->wait_frame)
                                plat->wait_frame(plat, rem);
                        else {
                                struct timespec ts;
                                ts.tv_sec = (time_t)rem;
                                ts.tv_nsec = (long)((rem - (f64)ts.tv_sec) * 1e9);
                                nanosleep(&ts, NULL);
                        }
                        if (!g_run || plat->should_close(plat))
                                break;    /* quit arrived during the wait */
                }
                frames++;
        }

        LOGI("shutting down");
        renderer_destroy(rend);
        for (int i = 0; i < nmon; i++)
                free(fbs[i].px);
        platform_destroy(plat);
        rtscene_free(sf.sc);
        if (sf.pack_used)
                rev_pack_free(&sf.pack);
        arena_destroy(sf.ir_arena);
        free(base_dir);
        return 0;
}
