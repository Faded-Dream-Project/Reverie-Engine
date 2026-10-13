/* ============================================================================
 * Reverie Engine — main.c
 * The reverie daemon: loads a .rev scene (text or .revpack), picks a display
 * backend (raw X11 or raw Wayland), picks a renderer (vulkan -> opengl ->
 * opengl-on-llvmpipe -> cpu), and runs the interactive frame loop.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
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
"  REV_WAYLAND_GPU 1 = opt into the experimental Wayland GPU WSI shim\n"
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

        while (g_run) {
                f64 now = rev_monotonic();
                f64 dt = now - last_frame;
                f64 t = now - t0;

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

                /* render + present */
                for (int i = 0; i < nmon; i++) {
                        if (winidx[i] < 0 || !fbs[i].px)
                                continue;
                        if (gpu_layers) {
                                fl.w = fbs[i].w;
                                fl.h = fbs[i].h;
                                fl.n = 0;
                                rtscene_render_layers(sf.sc, &fl, i, t, dt);
                                if (!rend->begin(rend, winidx[i], fbs[i].w, fbs[i].h))
                                        continue;
                                for (int k = 0; k < fl.n; k++) {
                                        RLayerCmd cmd;
                                        cmd.px = fl.cmds[k].px;
                                        cmd.w = fl.cmds[k].w;
                                        cmd.h = fl.cmds[k].h;
                                        cmd.blend = fl.cmds[k].blend;
                                        cmd.opacity = fl.cmds[k].opacity;
                                        cmd.blur = fl.cmds[k].blur;
                                        cmd.dx = fl.cmds[k].dx;
                                        cmd.dy = fl.cmds[k].dy;
                                        rend->layer(rend, &cmd);
                                }
                                rend->end(rend, NULL);
                        } else {
                                memset(fbs[i].px, 0, (size_t)fbs[i].w * fbs[i].h * 4);
                                rtscene_render(sf.sc, &fbs[i], i, t, dt);
                                rend->end(rend, &fbs[i]);
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
                        LOGI("perf: %.1f fps", (f64)frames / (now - last_stat));
                        if (drops > last_drops) {
                                LOGI("perf: %llu frame(s) dropped by the compositor",
                                     (unsigned long long)(drops - last_drops));
                                last_drops = drops;
                        }
                        frames = 0;
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
                last_frame = rev_monotonic();
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
