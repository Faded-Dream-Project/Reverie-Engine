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
#include <unistd.h>

#include "arena.h"
#include "cli.h"
#include "geom.h"
#include "ipc.h"
#include "log.h"
#include "pack.h"
#include "platform.h"
#include "png.h"
#include "render.h"
#include "rev.h"
#include "scene.h"
#include "session.h"
#include "theme.h"
#include "util.h"
#include "video.h"

static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
        (void)sig;
        g_run = 0;
}

/* ---- perf stats helpers ---------------------------------------------------- */

#define PERF_RING 512

/* idle pacing: loop cadence while no surface is visible (matches the wlshim
 * compositor-probe interval) */
#define IDLE_PACE_S 0.25

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
"usage: reverie [options] <scene.rev | theme dir | theme.revpack>\n"
"       reverie [options] --video <file.mp4 | file.gif | ...>\n"
"\n"
"options:\n"
"  -R, --renderer R    force renderer: auto | vulkan | opengl | cpu\n"
"  -B, --backend B     force backend: auto | x11 | wayland\n"
"      --scene PATH    explicit scene/theme path (same slot as the\n"
"                       positional; giving both is an error)\n"
"      --video PATH    media mode: play a video / animated GIF as the\n"
"                       wallpaper (independent of .rev scenes; needs the\n"
"                       FFmpeg libraries at runtime, which are OPTIONAL\n"
"                       at build time)\n"
"      --scale MODE    media placement: fill (cover, crop — default) |\n"
"                       fit (letterbox) | stretch | center (native size)\n"
"      --no-loop       media mode: hold the last frame at end-of-file\n"
"                       instead of looping (the default loops)\n"
"      --exit-if-running\n"
"                       exit 0 when another daemon already owns the\n"
"                       control socket — the duplicate-instance guard\n"
"                       (autostart entries carry it; needs the IPC\n"
"                       socket, see docs/AUTOSTART.md)\n"
"      --restore       launch whatever was launched last (theme, video,\n"
"                       or pack) instead of a path on the command line —\n"
"                       the remembered session in\n"
"                       ~/.local/state/reverie/last-session.json, updated\n"
"                       by every launch and runtime theme switch. Explicit\n"
"                       flags given now win over the remembered ones\n"
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
"\n");
        printf(
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
"  REV_WL_VIS_MS   idle power-save starvation window in ms (default 250):\n"
"                  a wl_shm-presented surface unlatched this long pauses\n"
"                  rendering until the compositor latches the idle probe\n"
"  REV_IDLE_AUDIT_S idle watchdog interval in seconds (default 30, 0 = off):\n"
"                  after this long continuously hidden, a 1 s full-rate\n"
"                  audit burst guards against a wedged idle detector\n"
"  REV_IPC          0 = disable the runtime control socket (default: on;\n"
"                  see docs/IPC.md — revtool ipc is the client)\n"
"  REV_IPC_SOCK     override the control socket path (default:\n"
"                  $XDG_RUNTIME_DIR/reverie.sock)\n"
"  REV_FADE_MS      default theme-switch fade duration in ms (default 400,\n"
"                  0 = hard cut)\n"
"  REV_AUTO_VK     1 = let auto also keep SOFTWARE Vulkan (lavapipe)\n"
"                  (off by default: a software stack delivers what the\n"
"                  CPU renderer does natively)\n"
"  REV_AUTO_GL_SW  1 = let auto keep software GL (llvmpipe) instead of\n"
"                  falling back to the native CPU renderer\n"
"\n"
"renderer policy: explicit requests are honored exactly and never fall\n"
"back silently (init failure exits with the reason). auto = hardware\n"
"Vulkan, else hardware OpenGL, else the native CPU renderer (software\n"
"Vulkan/GL stay opt-in via the env vars above; see docs/BACKENDS.md\n"
"for the measured rationale).\n");
}

typedef struct {
        RArena *ir_arena;
        RSceneIR *ir;
        RTScene *sc;
        f64 mtime;
        char path[512];
        RevPack pack;        /* valid when loaded from a .revpack */
        bool pack_used;
        RTheme *theme;       /* valid when loaded as a theme       */
} SceneFile;

static const u8 *pack_asset_lookup(void *ctx, const char *path, size_t *len)
{
        return rev_pack_asset((const RevPack *)ctx, path, len);
}

static const u8 *theme_asset_lookup(void *ctx, const char *path, size_t *len)
{
        /* scene asset paths are theme-relative; a leading "assets/" is the
         * theme convention but scenes may reference any theme resource */
        return theme_asset((RTheme *)ctx, path, len);
}

static bool scene_load(SceneFile *sf, const char *path, const char *base_dir,
                       char *err, size_t errcap)
{
        size_t len = 0;
        u8 *text = NULL;
        RArena *ar = NULL;
        RSceneIR *ir = NULL;
        RevPack pk;

        /* theme? (folder with theme.json, or a packed theme .revpack) —
         * one resource interface serves both; the scene loader never
         * learns which it is reading from */
        {
                RTheme *th = theme_load(path, err, errcap);
                if (th) {
                        if (!theme_engine_ok(theme_meta(th), err, errcap)) {
                                theme_free(th);
                                return false;
                        }
                        const char *stext = theme_scene_text(th, &len);
                        u8 *scopy = (u8 *)malloc(len + 1);
                        if (!scopy) {
                                theme_free(th);
                                snprintf(err, errcap, "out of memory");
                                return false;
                        }
                        memcpy(scopy, stext, len);
                        scopy[len] = 0;
                        ar = NULL;
                        ir = rev_parse_scene((const char *)scopy, len, &ar, err, errcap);
                        if (!ir) {
                                free(scopy);
                                theme_free(th);
                                return false;
                        }
                        RTScene *sc = rtscene_compile_assets(ir, ar, theme_base_dir(th),
                                                             th, theme_asset_lookup,
                                                             err, errcap);
                        if (!sc) {
                                arena_destroy(ar);
                                free(scopy);
                                theme_free(th);
                                return false;
                        }
                        /* free the previous scene, if any */
                        if (sf->sc)
                                rtscene_free(sf->sc);
                        if (sf->ir_arena)
                                arena_destroy(sf->ir_arena);
                        if (sf->pack_used)
                                rev_pack_free(&sf->pack);
                        if (sf->theme)
                                theme_free(sf->theme);
                        /* packed themes resolve assets through the container;
                         * folder themes may ALSO use the provider (relative
                         * paths hit the theme root first, disk second) */
                        rtscene_set_asset_provider(sc, th, theme_asset_lookup);
                        sf->theme = th;
                        sf->sc = sc;
                        sf->ir_arena = ar;
                        sf->ir = ir;
                        sf->pack_used = false;
                        free(scopy);
                        snprintf(sf->path, sizeof sf->path, "%s", path);
                        sf->mtime = rev_file_mtime(path);
                        return true;
                }
                /* not a theme — fall through to the plain scene paths */
                if (err)
                        err[0] = 0;
        }

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

        /* packs resolve assets through the container at compile AND decode
         * time — loading a pack must not require the assets on disk */
        RTScene *sc = rtscene_compile_assets(ir, ar, base_dir,
                                             is_pack ? (void *)&pk : NULL,
                                             is_pack ? pack_asset_lookup : NULL,
                                             err, errcap);
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
        if (sf->theme)
                theme_free(sf->theme);
        sf->theme = NULL;

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

/* Release everything a SceneFile owns (scene, IR arena, pack, theme). Used
 * at shutdown, at IPC theme swaps, and when replacing a fade target. */
static void scene_file_free(SceneFile *sf)
{
        if (sf->sc)
                rtscene_free(sf->sc);
        if (sf->ir_arena)
                arena_destroy(sf->ir_arena);
        if (sf->pack_used)
                rev_pack_free(&sf->pack);
        if (sf->theme)
                theme_free(sf->theme);
        memset(sf, 0, sizeof *sf);
}

/* ---- runtime IPC: theme switching, playlist rotation, fades -----------------
 *
 * The daemon exposes a per-user control socket (ipc.c, protocol v1). Theme
 * switches dip through black: the NEW scene loads up front (a stall while
 * the old one still shows), then a synthetic black overlay fades in over
 * the rendered frame — as a plain 1x1 black LAYER on the GPU tiers (blend
 * NORMAL with opacity = darkness, cached by a reserved content id) and as
 * rev_comp_fade() on the CPU tier. At full black the active scene swaps and
 * the overlay fades back out. A switch requested DURING a fade continues
 * from the current darkness — the obsolete target never flashes in. */

#define REV_PLAYLIST_MAX 64
/* reserved raster serial for the synthetic fade layer: real serials are
 * small process-global monotonic counts, so this can never alias one */
#define REV_FADE_CID 0x7FFFFFFFull

typedef struct {
        SceneFile *sf;          /* ACTIVE scene (rendered + watched)      */
        SceneFile next;         /* fade target                            */
        bool have_next;
        int phase;              /* 0 none, 1 fading out, 2 fading in      */
        f64 t0;                 /* phase start, monotonic seconds         */
        f64 ms;                 /* one-way fade duration                  */
        f64 fade_ms_default;    /* default for switches without [fade_ms] */
        char pl[REV_PLAYLIST_MAX][512];
        int pl_n, pl_i;
        f64 rotate_s;           /* auto-advance seconds, 0 = off          */
        f64 last_advance;
        RRenderer *rend;
        RPlatform *plat;
        volatile int *run;      /* QUIT clears it                        */
} IpcState;

static f64 fade_black_now(const IpcState *st, f64 now)
{
        if (st->phase == 0 || st->ms <= 0)
                return 0.0;
        f64 k = (now - st->t0) / (st->ms / 1000.0);
        if (k < 0.0)
                k = 0.0;
        if (k > 1.0)
                k = 1.0;
        return st->phase == 1 ? k : 1.0 - k;
}

/* A runtime theme switch (IPC THEME / playlist NEXT/PREV) is also "what
 * was last launched": the session record's content follows it, so
 * --restore replays what the user actually ended up on, not what they
 * booted with. Best-effort — a failed update warns, the daemon keeps
 * rendering. */
static void session_note_switch(const char *path)
{
        char serr[256];
        if (!rev_session_record_content(path, serr, sizeof serr))
                LOGW("cannot update the session record (%s)", serr);
}

/* Loads `path` into `st->next` and starts (or redirects) the fade. Returns
 * false with a message when the theme cannot load — the active scene is
 * untouched in that case. */
static bool daemon_switch_theme(IpcState *st, const char *path, f64 fade_ms,
                                f64 now, char *err, size_t errcap)
{
        if (fade_ms < 0.0)
                fade_ms = st->fade_ms_default;
        if (fade_ms > 10000.0)
                fade_ms = 10000.0;
        if (strlen(path) >= sizeof st->next.path) {
                snprintf(err, errcap, "theme path too long");
                return false;
        }

        if (fade_ms <= 0.5) {
                /* instant switch: replace the active scene outright */
                char *bd = rev_path_dirname(path);
                if (!scene_load(st->sf, path, bd ? bd : "", err, errcap)) {
                        free(bd);
                        return false;
                }
                free(bd);
                scene_file_free(&st->next);
                st->have_next = false;
                st->phase = 0;
                LOGI("ipc: theme '%s' active (%d layers, instant)",
                     st->sf->path, rtscene_layer_count(st->sf->sc));
                session_note_switch(st->sf->path);
                return true;
        }

        /* fading: load the target NOW (one stall while the old scene still
         * shows), then dip through black */
        SceneFile cand;
        memset(&cand, 0, sizeof cand);
        char *bd = rev_path_dirname(path);
        if (!scene_load(&cand, path, bd ? bd : "", err, errcap)) {
                free(bd);
                return false;
        }
        free(bd);
        scene_file_free(&st->next);
        st->next = cand;
        st->have_next = true;
        /* a switch during a running fade CONTINUES from the current
         * darkness: the obsolete target never flashes in */
        f64 cur = fade_black_now(st, now);
        st->ms = fade_ms;
        st->phase = 1;
        st->t0 = now - cur * (fade_ms / 1000.0);
        LOGI("ipc: switching to '%s' (%.0f ms fade)", path, fade_ms);
        return true;
}

/* Drives the fade timeline; performs the scene swap at full black. */
static void fade_advance(IpcState *st, f64 now)
{
        if (st->phase == 0)
                return;
        f64 dur = st->ms / 1000.0;
        if (dur <= 0 || now - st->t0 < dur)
                return;
        if (st->phase == 1) {
                if (st->have_next) {
                        scene_file_free(st->sf);
                        *st->sf = st->next;
                        memset(&st->next, 0, sizeof st->next);
                        st->have_next = false;
                        LOGI("ipc: theme '%s' active (%d layers)",
                             st->sf->path, rtscene_layer_count(st->sf->sc));
                        session_note_switch(st->sf->path);
                        if (st->pl_n > 0) {
                                /* keep the playlist index honest when the
                                 * switch came from THEME, not NEXT/PREV */
                                for (int i = 0; i < st->pl_n; i++)
                                        if (!strcmp(st->pl[i], st->sf->path))
                                                st->pl_i = i;
                        }
                }
                st->phase = 2;
                st->t0 = now;
        } else {
                st->phase = 0;
        }
}

static int playlist_advance(IpcState *st, int dir, f64 now, char *err, size_t errcap)
{
        if (st->pl_n <= 0) {
                snprintf(err, errcap, "no-playlist");
                return -1;
        }
        st->pl_i = (st->pl_i + dir + st->pl_n) % st->pl_n;
        st->last_advance = now;
        if (!daemon_switch_theme(st, st->pl[st->pl_i], -1.0, now, err, errcap)) {
                /* unavailable entry: report, keep the playlist running —
                 * the daemon keeps rendering whatever it already has */
                LOGW("ipc: playlist entry '%s' failed: %s", st->pl[st->pl_i], err);
                return -1;
        }
        return st->pl_i;
}

static void ipc_handler(void *ctx, const IpcMsg *msg, IpcReply *rep)
{
        IpcState *st = (IpcState *)ctx;
        const char *cmd = msg->argv[0];
        f64 now = rev_monotonic();
        char err[512] = { 0 };

        rep->ok = false;
        snprintf(rep->text, sizeof rep->text, "?");

        if (!strcmp(cmd, "PING")) {
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "PONG");
        } else if (!strcmp(cmd, "STATUS")) {
                bool vis = !st->plat->surface_visible ||
                           st->plat->surface_visible(st->plat);
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text,
                         "theme=%.200s fps=%d renderer=%s backend=%s visible=%d "
                         "fade=%.0f playlist=%d rotate=%.0f",
                         st->sf->path, rtscene_fps(st->sf->sc), st->rend->name,
                         st->plat->name, vis ? 1 : 0, st->fade_ms_default,
                         st->pl_n, st->rotate_s);
        } else if (!strcmp(cmd, "THEME")) {
                if (msg->argc < 2 || msg->argc > 3 || !*msg->argv[1]) {
                        snprintf(rep->text, sizeof rep->text,
                                 "usage: THEME <path> [fade_ms]");
                        return;
                }
                f64 fade = msg->argc == 3 ? atof(msg->argv[2]) : -1.0;
                if (daemon_switch_theme(st, msg->argv[1], fade, now,
                                        err, sizeof err)) {
                        rep->ok = true;
                        snprintf(rep->text, sizeof rep->text, "switched");
                } else {
                        snprintf(rep->text, sizeof rep->text, "%.220s", err);
                }
        } else if (!strcmp(cmd, "PLAYLIST")) {
                if (msg->argc < 2) {
                        snprintf(rep->text, sizeof rep->text,
                                 "usage: PLAYLIST <theme> [<theme> ...]");
                        return;
                }
                int n = msg->argc - 1;
                if (n > REV_PLAYLIST_MAX)
                        n = REV_PLAYLIST_MAX;
                for (int i = 0; i < n; i++) {
                        if (strlen(msg->argv[i + 1]) >= sizeof st->pl[i]) {
                                snprintf(rep->text, sizeof rep->text,
                                         "playlist path too long");
                                return;
                        }
                        snprintf(st->pl[i], sizeof st->pl[i], "%s", msg->argv[i + 1]);
                }
                st->pl_n = n;
                st->pl_i = 0;
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "playlist %d entries", n);
        } else if (!strcmp(cmd, "NEXT") || !strcmp(cmd, "PREV")) {
                f64 fade = msg->argc == 2 ? atof(msg->argv[1]) : -1.0;
                if (msg->argc > 2) {
                        snprintf(rep->text, sizeof rep->text,
                                 "usage: %s [fade_ms]", cmd);
                        return;
                }
                /* an explicit fade overrides the default for this switch */
                if (fade >= 0.0) {
                        f64 old = st->fade_ms_default;
                        st->fade_ms_default = fade;
                        int r = playlist_advance(st, !strcmp(cmd, "NEXT") ? 1 : -1,
                                                 now, err, sizeof err);
                        st->fade_ms_default = old;
                        if (r < 0) {
                                snprintf(rep->text, sizeof rep->text, "%.220s", err);
                                return;
                        }
                } else {
                        int r = playlist_advance(st, !strcmp(cmd, "NEXT") ? 1 : -1,
                                                 now, err, sizeof err);
                        if (r < 0) {
                                snprintf(rep->text, sizeof rep->text, "%.220s", err);
                                return;
                        }
                }
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "%.200s", st->pl[st->pl_i]);
        } else if (!strcmp(cmd, "ROTATE")) {
                if (msg->argc != 2) {
                        snprintf(rep->text, sizeof rep->text,
                                 "usage: ROTATE <seconds|0>");
                        return;
                }
                f64 sec = atof(msg->argv[1]);
                if (sec < 0.0)
                        sec = 0.0;
                if (sec > 86400.0)
                        sec = 86400.0;
                st->rotate_s = sec;
                st->last_advance = now;
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "rotate=%.0f", sec);
        } else if (!strcmp(cmd, "FADE")) {
                if (msg->argc != 2) {
                        snprintf(rep->text, sizeof rep->text,
                                 "usage: FADE <ms>");
                        return;
                }
                f64 ms = atof(msg->argv[1]);
                if (ms < 0.0)
                        ms = 0.0;
                if (ms > 10000.0)
                        ms = 10000.0;
                st->fade_ms_default = ms;
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "fade=%.0f", ms);
        } else if (!strcmp(cmd, "QUIT")) {
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "bye");
                *st->run = 0;
                LOGI("ipc: QUIT — shutting down");
        } else {
                snprintf(rep->text, sizeof rep->text, "unknown-command");
        }
}

/* ---- media mode (--video): playback state, IPC, placement ------------------- */

static const char *scale_name(int mode)
{
        switch (mode) {
        case REV_SCALE_FIT:      return "fit";
        case REV_SCALE_STRETCH:  return "stretch";
        case REV_SCALE_CENTER:   return "center";
        default:                 return "fill";
        }
}

typedef struct {
        const char *path;      /* the media file                        */
        bool loop;
        int scale;
        RRenderer *rend;
        RPlatform *plat;
        volatile int *run;     /* QUIT clears it                        */
} MediaIpc;

/* The media-mode IPC subset: PING/STATUS/QUIT work everywhere; the theme
 * machinery is scene-mode-only and reports that clearly instead of
 * silently doing nothing. */
static void ipc_handler_media(void *ctx, const IpcMsg *msg, IpcReply *rep)
{
        MediaIpc *m = (MediaIpc *)ctx;
        const char *cmd = msg->argv[0];

        rep->ok = false;
        snprintf(rep->text, sizeof rep->text, "?");

        if (!strcmp(cmd, "PING")) {
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "PONG");
        } else if (!strcmp(cmd, "STATUS")) {
                bool vis = !m->plat->surface_visible ||
                           m->plat->surface_visible(m->plat);
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text,
                         "mode=video path=%.200s renderer=%s backend=%s "
                         "visible=%d scale=%s loop=%d",
                         m->path, m->rend->name, m->plat->name,
                         vis ? 1 : 0, scale_name(m->scale), m->loop ? 1 : 0);
        } else if (!strcmp(cmd, "QUIT")) {
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "bye");
                *m->run = 0;
                LOGI("ipc: QUIT — shutting down");
        } else {
                snprintf(rep->text, sizeof rep->text,
                         "scene-mode-only command (media mode: restart "
                         "with a scene to use %s)", cmd);
        }
}

/* Media-mode scratch (grown on demand, bounded by the viewport because
 * cropped placements only convert the VISIBLE region). One video: one
 * scratch is enough; freed by media_scratch_free() at exit. */
static u32 *g_media_scratch;
static size_t g_media_scratch_cap;      /* u32 elements */

static void media_scratch_free(void)
{
        free(g_media_scratch);
        g_media_scratch = NULL;
        g_media_scratch_cap = 0;
}

static bool media_scratch_grow(size_t need_u32)
{
        if (need_u32 <= g_media_scratch_cap)
                return true;
        u32 *np = (u32 *)realloc(g_media_scratch, need_u32 * 4);
        if (!np)
                return false;
        g_media_scratch = np;
        g_media_scratch_cap = need_u32;
        return true;
}

/* The visible region of a placed rect, in both destination and source
 * coordinates. Bounds every allocation to the viewport: a 100x10000
 * source under fill would scale to 1920x192000 — the visible strip is
 * converted instead, never the whole placed rect. */
typedef struct {
        i32 dx0, dy0;          /* destination top-left (>= 0)             */
        u32 dw, dh;            /* visible size (<= viewport)             */
        u32 sx, sy, sw, sh;    /* source region mapping onto it          */
} RMediaClip;

static bool media_visible_rect(const RVideoFrame *vf, const RScaleRect *r,
                               u32 W, u32 H, RMediaClip *c)
{
        const i32 dx1 = (i32)W < r->dx + (i32)r->w ? (i32)W : r->dx + (i32)r->w;
        const i32 dy1 = (i32)H < r->dy + (i32)r->h ? (i32)H : r->dy + (i32)r->h;
        c->dx0 = r->dx > 0 ? r->dx : 0;
        c->dy0 = r->dy > 0 ? r->dy : 0;
        if (dx1 <= c->dx0 || dy1 <= c->dy0)
                return false;
        c->dw = (u32)(dx1 - c->dx0);
        c->dh = (u32)(dy1 - c->dy0);
        const f64 kx = (f64)vf->w / (f64)r->w;    /* source px per placed px */
        const f64 ky = (f64)vf->h / (f64)r->h;
        c->sx = (u32)(((f64)c->dx0 - (f64)r->dx) * kx + 0.5);
        c->sy = (u32)(((f64)c->dy0 - (f64)r->dy) * ky + 0.5);
        c->sw = (u32)((f64)c->dw * kx + 0.5);
        c->sh = (u32)((f64)c->dh * ky + 0.5);
        if (c->sx + c->sw > vf->w) c->sw = vf->w > c->sx ? vf->w - c->sx : 0;
        if (c->sy + c->sh > vf->h) c->sh = vf->h > c->sy ? vf->h - c->sy : 0;
        return c->sw && c->sh;
}

/* Places a video frame into a CPU framebuffer under a scaling mode. The
 * pre-clear is the letterbox/pillarbox color (black).
 *
 * EVERYTHING goes through the padded scratch: sws can spill up to one
 * extra output row past the last (observed 8 u32 on sub-rect SIMD paths
 * — an FFmpeg-internal behavior that varies by build), so conversions
 * never target the framebuffer directly. The scratch is bounded by the
 * VISIBLE region, which is bounded by the viewport — a 100x10000 source
 * under fill converts the visible strip, not a 1920x192000 intermediate.
 *
 * Media mode uses video-player semantics: the background is OPAQUE
 * black — letterbox/pillarbox bars are black (not see-through to the
 * desktop), and transparent source texels (animated GIFs) composite
 * over that black. Scenes keep the opposite convention (transparent
 * background = desktop shows through); the two modes stay separate by
 * design.
 *
 * YUV frames carry no alpha — the row copy into the framebuffer IS the
 * composite against black. RGB-family frames may carry transparency
 * (animated GIFs); they go through the engine compositor so a
 * transparent texel shows the black background instead of leaking its
 * underlying RGB. */
static void place_media_rgba(const RVideoFrame *vf, int mode, RFb *fb)
{
        const u32 W = fb->w, H = fb->h;
        /* opaque black, not transparent: the bars are BLACK on every
         * renderer, byte-identically */
        for (size_t i = 0; i < (size_t)W * H; i++)
                fb->px[i] = 0xFF000000u;
        if (!vf || !vf->w || !vf->h)
                return;
        RScaleRect r = rev_scale_place(mode, W, H, vf->w, vf->h);
        RMediaClip c;
        if (!media_visible_rect(vf, &r, W, H, &c))
                return;
        if (!media_scratch_grow((size_t)c.dw * (c.dh + 1) + 16))
                return;
        if (!rev_video_to_rgba_sub(vf, c.sx, c.sy, c.sw, c.sh,
                                   g_media_scratch, c.dw, c.dw, c.dh))
                return;
        if (vf->rgb) {
                RFb src = { g_media_scratch, c.dw, c.dh };
                rev_comp_blend_at(fb, &src, c.dx0, c.dy0,
                                  REV_BLEND_NORMAL, 1.0);
        } else {
                for (u32 y = 0; y < c.dh; y++)
                        memcpy(fb->px + (size_t)(c.dy0 + (i32)y) * W + (u32)c.dx0,
                               g_media_scratch + (size_t)y * c.dw,
                               (size_t)c.dw * 4);
        }
}

/* One media frame through the active renderer. GPU tiers receive a
 * single layer command (YUV planes as vsim, RGBA frames as a pixel
 * layer); the CPU tier converts straight into the framebuffer. A frame
 * that is not decoded yet still presents — black on the CPU tier, the
 * cleared accumulation on GPU tiers — so the compositor always has a
 * buffer. `vd` must be address-stable across frames (renderers key
 * per-layer GPU state on the descriptor's address). */
static void render_media_frame(RRenderer *rend, const RVideoFrame *vf,
                               int mode, RFb *fb, int win)
{
        const u32 W = fb->w, H = fb->h;
        const bool gpu_layers = rend->wants_layers && rend->wants_video;
        if (!gpu_layers) {
                if (!rend->begin(rend, win, W, H))
                        return;
                place_media_rgba(vf, mode, fb);
                rend->end(rend, fb);
                return;
        }
        if (!rend->begin(rend, win, W, H))
                return;
        /* opaque black background first — the same 1x1-black-layer trick
         * the theme fade uses (cached upload, reserved id next to it), so
         * GPU-tier letterbox bars are black exactly like the CPU tier's
         * and transparent source texels composite over black, never
         * through to the desktop */
        {
                static const u32 black_px[1] = { 0xFF000000u };
                RLayerCmd bg;
                memset(&bg, 0, sizeof bg);
                bg.px = black_px;
                bg.w = 1;
                bg.h = 1;
                bg.place_w = W;
                bg.place_h = H;
                bg.blend = REV_BLEND_NORMAL;
                bg.opacity = 1.0;
                bg.linear = false;
                bg.content_id = REV_FADE_CID - 1;
                rend->layer(rend, &bg);
        }
        if (vf && vf->w) {
                static RSimVideo vd;    /* one video: one stable descriptor */
                RScaleRect r = rev_scale_place(mode, W, H, vf->w, vf->h);
                RLayerCmd cmd;
                memset(&cmd, 0, sizeof cmd);
                if (vf->rgb) {
                        cmd.px = (const u32 *)vf->rgba;
                        cmd.w = vf->w;
                        cmd.h = vf->h;
                } else {
                        vd.y = vf->y;
                        vd.u = vf->u;
                        vd.v = vf->v;
                        vd.ls_y = vf->ls_y;
                        vd.ls_u = vf->ls_u;
                        vd.ls_v = vf->ls_v;
                        vd.w = vf->w;
                        vd.h = vf->h;
                        cmd.vsim = &vd;
                        cmd.w = vf->w;
                        cmd.h = vf->h;
                }
                cmd.place_w = r.w;
                cmd.place_h = r.h;
                cmd.dx = r.dx;
                cmd.dy = r.dy;
                cmd.blend = REV_BLEND_NORMAL;
                cmd.opacity = 1.0;
                cmd.blur = 0.0;
                cmd.linear = true;
                cmd.content_id = 0;
                rend->layer(rend, &cmd);
        }
        rend->end(rend, NULL);
}

/* ---- output hotplug reconcile -----------------------------------------------
 * Re-enumerates the platform's monitor list and reconciles the daemon's
 * windows with it: new monitors get a window + framebuffer + renderer
 * surface, gone monitors lose theirs, changed geometry feeds the same
 * resize dance the per-tick poll uses. SLOTS ARE NEVER COMPACTED — the
 * scene's per-monitor instances and the renderers' per-window state are
 * indexed by slot, so a gone monitor tombstones (winidx[i] = -1) and a new
 * one reuses the first free slot. Identity comes from monitor_id (Wayland:
 * wl_output global name; X11: Xinerama-origin key). Caller gates the
 * cadence (REV_HOTPLUG_S, default 2 s; 0 = off) and skips when -m pinned a
 * monitor or the platform lacks the identity/removal primitives.
 * Returns the (possibly grown) high-water slot count. */
static int hotplug_reconcile(RPlatform *plat, RRenderer *rend,
                              RRect *mons, RFb *fbs, u32 *fb_scale, int *mon_hz,
                              u64 *mon_id, int *winidx, int nmon,
                              bool media, RTScene *sc, bool *zero_logged)
{
        RRect fresh[REV_PLATFORM_MAX_OUTPUTS];
        int nfresh = plat->monitors(plat, fresh, REV_PLATFORM_MAX_OUTPUTS);
        if (nfresh < 0)
                return nmon;
        u64 fid[REV_PLATFORM_MAX_OUTPUTS];
        for (int j = 0; j < nfresh; j++)
                fid[j] = plat->monitor_id(plat, j);
        /* claimed[j]: the fresh entry was consumed by an active slot. Slots
         * claim DISTINCT entries even when identities collide (mirrored
         * Xinerama screens share an origin key) — otherwise every reconcile
         * pass would re-add a duplicate and grow forever. */
        bool claimed[REV_PLATFORM_MAX_OUTPUTS];
        memset(claimed, 0, sizeof claimed);

        /* ---- removals + geometry updates ---------------------------------- */
        for (int i = 0; i < nmon; i++) {
                if (winidx[i] < 0)
                        continue;    /* already tombstoned */
                int j = -1;
                for (int k = 0; k < nfresh; k++) {
                        if (!claimed[k] && fid[k] == mon_id[i]) {
                                j = k;
                                break;
                        }
                }
                if (j < 0) {
                        /* output gone: release the renderer's surface FIRST
                         * (its native handle dies with the platform window),
                         * then the window, then the framebuffer */
                        LOGI("hotplug: monitor gone (%ux%u at %d,%d) — releasing window %d",
                             mons[i].w, mons[i].h, mons[i].x, mons[i].y, winidx[i]);
                        if (rend->remove_window)
                                rend->remove_window(rend, winidx[i]);
                        if (plat->remove_window && !plat->remove_window(plat, winidx[i]))
                                LOGW("hotplug: window %d removal refused — "
                                     "leaking the slot", winidx[i]);
                        free(fbs[i].px);
                        fbs[i].px = NULL;
                        winidx[i] = -1;
                        continue;
                }
                claimed[j] = true;

                /* survived: pull the fresh geometry/scale/refresh into the
                 * slot (the per-tick poll only sees the WINDOW's geometry,
                 * which never changes on its own for monitor-driven changes) */
                f64 mscale = plat->monitor_scale(plat, j);
                u32 nscale = mscale > 1.0 ? (u32)(mscale + 0.5) : 1;
                int nhz = plat->monitor_refresh ? plat->monitor_refresh(plat, j) : 0;
                u32 nw = (u32)(fresh[j].w * nscale);
                u32 nh = (u32)(fresh[j].h * nscale);
                bool size_changed = nw != fbs[i].w || nh != fbs[i].h;
                bool moved = fresh[j].x != mons[i].x || fresh[j].y != mons[i].y;
                if (size_changed || moved) {
                        LOGI("monitor %d: geometry now %ux%u at %d,%d%s",
                             i, fresh[j].w, fresh[j].h, fresh[j].x, fresh[j].y,
                             size_changed ? ", re-creating" : "");
                        free(fbs[i].px);
                        fbs[i].w = nw;
                        fbs[i].h = nh;
                        fbs[i].px = (u32 *)calloc((size_t)nw * nh, 4);
                        plat->resize_window(plat, winidx[i], fresh[j]);
                        rend->resize(rend, winidx[i], nw, nh);
                        if (!media && sc)
                                rtscene_flush_caches(sc);
                }
                mons[i] = fresh[j];
                fb_scale[i] = nscale;
                mon_hz[i] = nhz;
        }

        /* ---- additions ------------------------------------------------------ */
        for (int j = 0; j < nfresh; j++) {
                if (claimed[j])
                        continue;
                int slot = -1;
                for (int i = 0; i < nmon; i++) {
                        if (winidx[i] < 0) {
                                slot = i;
                                break;
                        }
                }
                if (slot < 0 && nmon < REV_PLATFORM_MAX_OUTPUTS)
                        slot = nmon++;
                if (slot < 0) {
                        static bool warned;
                        if (!warned) {
                                warned = true;
                                LOGW("hotplug: output beyond the %d-slot cap ignored",
                                     REV_PLATFORM_MAX_OUTPUTS);
                        }
                        continue;
                }

                f64 mscale = plat->monitor_scale(plat, j);
                fb_scale[slot] = mscale > 1.0 ? (u32)(mscale + 0.5) : 1;
                mon_hz[slot] = plat->monitor_refresh ? plat->monitor_refresh(plat, j) : 0;
                mon_id[slot] = fid[j];
                mons[slot] = fresh[j];
                fbs[slot].w = (u32)(fresh[j].w * fb_scale[slot]);
                fbs[slot].h = (u32)(fresh[j].h * fb_scale[slot]);
                fbs[slot].px = (u32 *)calloc((size_t)fbs[slot].w * fbs[slot].h, 4);
                winidx[slot] = plat->add_window(plat, fresh[j]);
                if (winidx[slot] < 0) {
                        LOGW("hotplug: monitor %dx%d at %d,%d — window creation failed",
                             fresh[j].w, fresh[j].h, fresh[j].x, fresh[j].y);
                        free(fbs[slot].px);
                        fbs[slot].px = NULL;
                        continue;
                }
                if (rend->add_window && !rend->add_window(rend, plat, winidx[slot])) {
                        /* GPU surface failed (logged by the renderer): a
                         * renderer-owned present path cannot use the window,
                         * so release it again instead of presenting black */
                        if (rend->remove_window)
                                rend->remove_window(rend, winidx[slot]);
                        if (plat->remove_window)
                                plat->remove_window(plat, winidx[slot]);
                        free(fbs[slot].px);
                        fbs[slot].px = NULL;
                        winidx[slot] = -1;
                        continue;
                }
                *zero_logged = false;
                LOGI("hotplug: monitor added (%ux%u at %d,%d) — window %d",
                     fresh[j].w, fresh[j].h, fresh[j].x, fresh[j].y, winidx[slot]);
        }

        /* ---- zero-window transition (keep idling until an output returns) -- */
        bool any = false;
        for (int i = 0; i < nmon && !any; i++)
                any = winidx[i] >= 0;
        if (!any && !*zero_logged) {
                LOGI("hotplug: no outputs left — idling until one returns");
                *zero_logged = true;
        }
        return nmon;
}

int main(int argc, char **argv)
{
        char err[512] = { 0 };
        RevArgs a;
        if (!rev_cli_parse(argc, argv, &a, err, sizeof err)) {
                fprintf(stderr, "reverie: %s\n", err);
                if (a.show_usage)
                        usage();
                return 1;
        }
        if (a.want_help) {
                usage();
                return 0;
        }
        if (a.verbosity > 0)
                rev_log_set_level(REV_LOG_DEBUG);
        else if (a.verbosity < 0)
                rev_log_set_level(REV_LOG_ERROR);

        /* ---- --restore: the content slot arrives from the session record */
        RevSession sess;   /* owns the restored tokens: a.scene_path /
                              a.video_path point into sess.line — keep it
                              alive for the whole of main() */
        memset(&sess, 0, sizeof sess);
        if (a.restore) {
                if (!rev_session_apply(&a, &sess, err, sizeof err)) {
                        fprintf(stderr, "reverie: %s\n", err);
                        return 1;
                }
                LOGI("restore: replaying the last session (%s)",
                     a.video_path ? a.video_path : a.scene_path);
        }

        /* locals carried from the historical parser so the rest of main()
         * stays untouched; cli.c now owns parsing + validation */
        const char *scene_path = a.scene_path;
        const char *renderer_pref = a.renderer_pref;
        const char *backend_pref = a.backend_pref;
        int fps_override = a.fps_override;
        int only_monitor = a.only_monitor;
        bool once = a.once, list = a.list;
        f64 shot_after = a.shot_after;
        const char *shot_file = a.shot_file;
        u32 out_w = a.out_w, out_h = a.out_h;
        const char *out_png = a.out_png;

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
        /* the env vars bypass the parser's vocabulary check — validate here
         * so a typo is a hard error, not a silent fall-back into auto */
        if (!rev_cli_canon_renderer(renderer_pref)) {
                fprintf(stderr, "reverie: bad REV_RENDERER '%s' (want auto | "
                                "vulkan | opengl | cpu)\n", renderer_pref);
                return 1;
        }
        if (!rev_cli_canon_backend(backend_pref)) {
                fprintf(stderr, "reverie: bad REV_BACKEND '%s' (want auto | "
                                "x11 | wayland)\n", backend_pref);
                return 1;
        }

        /* --exit-if-running: the duplicate-instance guard the autostart
         * entry carries. Probe the per-user control socket BEFORE any
         * heavy setup — a live daemon means this launch yields cleanly
         * instead of stacking a second set of wallpaper windows. (The
         * probe needs the IPC socket; REV_IPC=0 removes the probe point.) */
        if (a.exit_if_running && ipc_daemon_live()) {
                LOGI("another reverie daemon owns the control socket — "
                     "exiting (--exit-if-running)");
                return 0;
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

        if (!scene_path && !a.video_path) {
                usage();
                return 1;
        }

        LOGI("reverie %s — backend=%s renderer=%s", REV_VERSION, backend_pref, renderer_pref);

        /* ---- content: a scene/theme, or the --video media mode ---------- */
        const bool media = a.video_path != NULL;
        SceneFile sf;
        memset(&sf, 0, sizeof sf);
        char *base_dir = NULL;
        RevVideo *vid = NULL;
        if (media) {
                RVideoSource vsrc = { .path = a.video_path };
                vid = rev_video_open(&vsrc, !a.no_loop, err, sizeof err);
                if (!vid) {
                        fprintf(stderr, "reverie: %s\n", err);
                        return 1;
                }
                LOGI("media '%s': video mode, scale=%s, loop=%s",
                     a.video_path, scale_name(a.scale),
                     a.no_loop ? "off (hold last frame at EOF)" : "on");
        } else {
                base_dir = rev_path_dirname(scene_path);
                if (!scene_load(&sf, scene_path, base_dir, err, sizeof err)) {
                        fprintf(stderr, "reverie: %s\n", err);
                        free(base_dir);
                        return 1;
                }
                LOGI("scene '%s': %d layer(s), %d fps, %s%s",
                     scene_path, rtscene_layer_count(sf.sc), rtscene_fps(sf.sc),
                     rtscene_is_animated(sf.sc) ? "animated" : "static",
                     sf.pack_used ? " [packed]" : "");
        }
        /* ---- remember what was launched (--restore's memory) -------------
         * Content just loaded successfully: this is "what was last
         * launched". --once runs are screenshots, not wallpaper launches —
         * they never touch the memory. Best-effort: a state-dir failure
         * warns and keeps going (the wallpaper matters more than the
         * note-taking). */
        if (!once) {
                char serr[256];
                if (!rev_session_record(&a, serr, sizeof serr))
                        LOGW("cannot record the session (%s) — --restore "
                             "will replay the previous one", serr);
        }
        if (fps_override > 0)
                LOGI("fps override: %d", fps_override);

        /* ---- offline single-frame mode ---- */
        if (once) {
                RFb fb = { (u32 *)calloc((size_t)out_w * out_h, 4), out_w, out_h };
                if (!fb.px) {
                        fprintf(stderr, "reverie: out of memory\n");
                        return 1;
                }
                if (media) {
                        /* first decoded frame, placed per --scale; the
                         * decode thread needs a moment to start */
                        const RVideoFrame *vf = NULL;
                        for (int i = 0; i < 100 && !vf; i++) {
                                vf = rev_video_display(vid, 0.0);
                                usleep(2000);
                        }
                        place_media_rgba(vf, a.scale, &fb);
                } else {
                        rtscene_render(sf.sc, &fb, 0, 0.0, 1.0 / 60.0);
                }
                RevImage img = { out_w, out_h, fb.px };
                if (!rev_png_encode_file(out_png, &img, err, sizeof err)) {
                        fprintf(stderr, "reverie: %s\n", err);
                        free(fb.px);
                        if (vid)
                                rev_video_close(vid);
                        free(base_dir);
                        return 1;
                }
                printf("wrote %s (%ux%u)\n", out_png, out_w, out_h);
                free(fb.px);
                if (vid)
                        rev_video_close(vid);
                media_scratch_free();
                scene_file_free(&sf);
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
        int mon_hz[REV_PLATFORM_MAX_OUTPUTS];     /* cached output refresh  */
        u64 mon_id[REV_PLATFORM_MAX_OUTPUTS];      /* stable hotplug identity */
        int any_window = 0;
        for (int i = 0; i < nmon; i++) {
                f64 scale = plat->monitor_scale(plat, i);
                fb_scale[i] = scale > 1.0 ? (u32)(scale + 0.5) : 1;
                mon_hz[i] = plat->monitor_refresh ? plat->monitor_refresh(plat, i) : 0;
                mon_id[i] = plat->monitor_id ? plat->monitor_id(plat, i) : (u64)(i + 1);
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
                if (vid)
                        rev_video_close(vid);
                scene_file_free(&sf);
                free(base_dir);
                return 1;
        }

        /* renderer selection happens AFTER windows exist (surfaces need them) */
        RRenderer *rend = renderer_create(renderer_pref, plat, err, sizeof err);
        if (!rend) {
                /* full cleanup — the explicit-failure path used to leak the
                 * frame buffers + the scene/theme (ASan: 4 MB per attempt) */
                fprintf(stderr, "reverie: %s\n", err);
                for (int i = 0; i < nmon; i++)
                        free(fbs[i].px);
                platform_destroy(plat);
                if (vid)
                        rev_video_close(vid);
                scene_file_free(&sf);
                free(base_dir);
                return 1;
        }

        /* sim offload: only when the renderer owns simulations (GL tier with
         * float render targets) — the scene then skips the CPU fluid/ripple
         * solvers entirely and emits GPU descriptors instead. Media mode
         * has no scene — its GPU path is decided per frame in
         * render_media_frame(). */
        const bool sim_offload = rend->wants_sims;
        const bool gpu_layers = !media && rend->wants_layers &&
                                rtscene_gpu_capable(sf.sc, sim_offload);
        if (!media && rend->wants_layers && !gpu_layers)
                LOGI("scene needs CPU compositing (ripple layers need GPU sims "
                     "this renderer declined)");
        if (!media && gpu_layers)
                LOGI("simulations: %s",
                     sim_offload ? "GPU (no per-pixel CPU work)"
                                 : "CPU (renderer declined GPU sims)");
        const bool point_offload = rend->wants_points;
        if (gpu_layers && rtscene_has_particles(sf.sc))
                LOGI("particles: %s",
                     point_offload ? "GPU point sprites"
                                   : "CPU sprite raster");
        const bool grad_offload = rend->wants_grads;
        if (gpu_layers && rtscene_has_gradients(sf.sc))
                LOGI("gradients: %s",
                     grad_offload ? "GPU shader evaluation"
                                  : "CPU rasterization");
        const bool video_offload = rend->wants_video;
        if (gpu_layers && rtscene_has_video(sf.sc))
                LOGI("video: %s",
                     video_offload ? "GPU planes + shader color convert"
                                   : "CPU swscale");

        /* ---- runtime IPC: theme switching / playlists / QUIT ------------------
         * REV_IPC=0 opts out. A live socket owned by another daemon is left
         * alone ("in-use") — this instance just runs without control.
         * Media mode serves its own PING/STATUS/QUIT subset. */
        IpcState st;
        memset(&st, 0, sizeof st);
        st.sf = &sf;
        st.rend = rend;
        st.plat = plat;
        st.run = &g_run;
        MediaIpc mipc;
        memset(&mipc, 0, sizeof mipc);
        mipc.path = a.video_path;
        mipc.loop = !a.no_loop;
        mipc.scale = a.scale;
        mipc.rend = rend;
        mipc.plat = plat;
        mipc.run = &g_run;
        {
                const char *fm = getenv("REV_FADE_MS");
                st.fade_ms_default = (fm && *fm) ? atof(fm) : 400.0;
                if (st.fade_ms_default < 0)
                        st.fade_ms_default = 0;
                if (st.fade_ms_default > 10000)
                        st.fade_ms_default = 10000;
        }
        IpcServer *ipc = NULL;
        {
                const char *dis = getenv("REV_IPC");
                if (dis && strcmp(dis, "0") == 0) {
                        LOGI("ipc: disabled (REV_IPC=0)");
                } else {
                        char ierr[256] = { 0 };
                        ipc = ipc_serve_start(NULL, ierr, sizeof ierr);
                        if (!ipc) {
                                if (!strcmp(ierr, "in-use"))
                                        LOGW("ipc: another daemon owns the "
                                             "socket — control disabled");
                                else
                                        LOGW("ipc: %s — control disabled", ierr);
                        } else {
                                LOGI("ipc: control socket %s (protocol v%d, "
                                     "per-user; revtool ipc is the client)",
                                     ipc_serve_path(ipc), IPC_VERSION);
                        }
                }
        }

        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);

        f64 t0 = rev_monotonic();
        f64 last_frame = t0;
        f64 last_reload_check = t0;
        f64 last_stat = t0;
        st.last_advance = t0;
        u32 frames = 0;
        u64 last_drops = 0;
        bool shot_done = false;
        RTFrameList fl;
        /* scene clock + pacing grid:
         * t_vis — the animation clock FREEZES while no surface is visible
         *   (idle power-save): sims, procedural animation and video resume
         *   exactly where they stopped instead of fast-forwarding through
         *   the hidden gap. While always visible it is identical to the old
         *   wall clock (it accumulates the same dts).
         * next_deadline — the absolute frame grid; see the pacing block. */
        f64 t_vis = 0;
        f64 next_deadline = 0;
        bool prev_visible = true;
        bool prev_active = true;
        bool idle_note_shown = false;
        /* idle watchdog (REV_IDLE_AUDIT_S, default 30, 0 = off): after this
         * many SECONDS of continuous "hidden", force one 1 s full-rate
         * audit burst. If the visibility evidence ever wedges (a compositor
         * that never answers probes while actually showing the surface),
         * the burst's presents re-arm the latching evidence and flip the
         * daemon back to rendering — no platform's idle detection can
         * permanently sleep the engine. On a genuinely dark display the
         * burst costs a handful of dropped acquires, once per interval. */
        f64 idle_audit_s = 30.0;
        {
                const char *e = getenv("REV_IDLE_AUDIT_S");
                if (e && *e)
                        idle_audit_s = atof(e);
                if (idle_audit_s < 0)
                        idle_audit_s = 0;
        }
        f64 hidden_since = 0;
        f64 audit_until = -1;
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

        /* REV_FAKE_CLICK="x,y[@t][;x,y@t ...]": TEST hook — synthesizes
         * full clicks (press+release) at normalized [0..1] positions.
         * @t fires that click at t seconds since start (default: one every
         * 0.2 s starting at t=0.3 s) — delayed clicks are how the
         * click-after-calm regression is tested headlessly. CLICK-driven
         * interaction (ripple splash placement, particle bursts) is
         * regression-testable without a display server — the drag path had
         * REV_FAKE_POINTER, the click path had nothing. The real pointer
         * stays untouched; only the click events are injected. Never active
         * unless the env var is set. */
        enum { FAKE_CLICK_MAX = 8 };
        f64 fc_x[FAKE_CLICK_MAX], fc_y[FAKE_CLICK_MAX], fc_t[FAKE_CLICK_MAX];
        int fc_n = 0, fc_next = 0;
        {
                const char *spec = getenv("REV_FAKE_CLICK");
                if (spec && *spec) {
                        const char *p = spec;
                        while (fc_n < FAKE_CLICK_MAX) {
                                char *end = NULL;
                                double x = strtod(p, &end);
                                if (end == p)
                                        break;
                                p = end;
                                while (*p == ' ' || *p == ',')
                                        p++;
                                double y = strtod(p, &end);
                                if (end == p)
                                        break;
                                p = end;
                                fc_x[fc_n] = x;
                                fc_y[fc_n] = y;
                                fc_t[fc_n] = 0.3 + 0.2 * (f64)fc_n;   /* default cadence */
                                if (*p == '@') {
                                        double at = strtod(p + 1, &end);
                                        if (end != p + 1) {
                                                fc_t[fc_n] = at;
                                                p = end;
                                        }
                                }
                                fc_n++;
                                if (*p != ';')
                                        break;
                                p++;
                        }
                        if (fc_n > 0)
                                LOGI("fake click active (test hook): %d click(s)", fc_n);
                }
        }

        /* ---- output hotplug reconcile cadence --------------------------------
         * REV_HOTPLUG_S (default 2 s, 0 = off): how often the monitor list is
         * re-enumerated and windows reconciled. Disabled when a monitor was
         * pinned with -m (the user asked for exactly that output) or when the
         * platform lacks the primitives (no monitor_id / no remove_window).
         * The per-tick window-geometry poll below still handles plain resizes
         * between reconcile passes. */
        f64 hotplug_s = 2.0;
        {
                const char *hs = getenv("REV_HOTPLUG_S");
                if (hs && *hs) {
                        char *end = NULL;
                        f64 v = strtod(hs, &end);
                        if (end != hs && v >= 0.0 && v < 3600.0)
                                hotplug_s = v;
                }
        }
        const bool hotplug_on = hotplug_s > 0.0 && only_monitor < 0 &&
                                plat->monitor_id && plat->remove_window;
        if (hotplug_on)
                LOGD("hotplug: reconcile every %.0fs", hotplug_s);
        f64 hotplug_next = 0.0;
        bool zero_logged = false;

        while (g_run) {
                f64 now = rev_monotonic();
                f64 dt = now - last_frame;
                /* dt must span START-to-START (render + pacing wait): the
                 * old code set last_frame AFTER the pacing wait, so dt was
                 * the few microseconds between wait-end and loop-top — every
                 * simulation (fluid/ripple/particles) stepped with dt≈0 and
                 * effectively froze (only wall-clock-t animations moved). */
                last_frame = now;

                /* visibility (idle power-save): on Wayland the shim watches
                 * frame-callback starvation — a fullscreen app over the
                 * wallpaper or a sleeping display stops the callbacks.
                 * Hidden: skip the render entirely, freeze the scene clock,
                 * pace at the probe rate. NULL (X11, raw client) = assume
                 * visible, exactly the old behavior. */
                bool visible = !plat->surface_visible ||
                               plat->surface_visible(plat);

                /* idle watchdog audit bursts (see the comment at the state
                 * declarations) — never lets a wedged detector sleep us
                 * forever, on ANY platform. */
                if (!visible && idle_audit_s > 0) {
                        if (hidden_since == 0)
                                hidden_since = now;
                        if (now - hidden_since >= idle_audit_s) {
                                hidden_since = now;
                                audit_until = now + 1.0;
                                LOGI("idle watchdog: %.0fs hidden — 1s audit "
                                     "burst (guards against a wedged idle "
                                     "detector; REV_IDLE_AUDIT_S=0 disables)",
                                     idle_audit_s);
                        }
                } else {
                        hidden_since = 0;
                }
                const bool audit = now < audit_until;

                /* WSI idle probe: when the present path itself must carry
                 * the compositor probe (Vulkan swapchain on Wayland), keep
                 * rendering at the idle pace while hidden — each probe
                 * present arms a fresh frame request the compositor answers
                 * when it comes back. The scene clock stays frozen (dt=0
                 * below), so probe frames are pixel-identical. */
                const bool wsi_probe = !visible && !audit &&
                                       plat->hidden_render_probe &&
                                       plat->hidden_render_probe(plat);
                const bool render_now = visible || wsi_probe || audit;

                if (prof_on && visible && nframe_ms < PERF_RING)
                        frame_ms[nframe_ms++] = dt * 1000.0;
                if (!visible && !idle_note_shown) {
                        idle_note_shown = true;
                        if (wsi_probe)
                                LOGI("idle: no surface is being shown — "
                                     "rendering at probe rate only (WSI "
                                     "presents ~4 Hz carry the frame "
                                     "requests; scene clock frozen)");
                        else
                                LOGI("idle: no surface is being shown — rendering paused "
                                     "(compositor probe ~4 Hz, scene clock frozen)");
                } else if (visible && idle_note_shown) {
                        idle_note_shown = false;
                        LOGI("idle: surface visible again — rendering resumed");
                }
                if (visible) {
                        if (prev_visible) {
                                t_vis += dt;
                                /* clamp rare long stalls for the sims (they
                                 * clamp internally too — belt and braces) */
                                if (dt > 0.25)
                                        dt = 0.25;
                        } else {
                                dt = 0;   /* resume frame: the gap was idle, not scene time */
                        }
                } else {
                        dt = 0;           /* frozen while hidden */
                }
                f64 t = t_vis;

                plat->pump(plat);
                if (plat->should_close(plat))
                        break;

                /* output hotplug reconcile (gated cadence): runs even while
                 * hidden — idling with zero outputs must notice a replug.
                 * pump() above dispatched the platform's hotplug events
                 * (Wayland late-bound outputs / global_removes) first. */
                if (hotplug_on && now >= hotplug_next) {
                        hotplug_next = now + hotplug_s;
                        nmon = hotplug_reconcile(plat, rend, mons, fbs, fb_scale,
                                                  mon_hz, mon_id, winidx, nmon,
                                                  media, sf.sc, &zero_logged);
                }

                /* runtime IPC: bounded non-blocking pump — theme switches,
                 * playlist commands and QUIT arrive here (works while hidden
                 * too: the loop still ticks at the idle pace). Media mode
                 * serves its PING/STATUS/QUIT subset. */
                if (ipc) {
                        if (media)
                                ipc_serve_poll(ipc, ipc_handler_media, &mipc);
                        else
                                ipc_serve_poll(ipc, ipc_handler, &st);
                        fade_advance(&st, now);
                        if (!media && st.rotate_s > 0 && st.pl_n > 0 &&
                            now - st.last_advance >= st.rotate_s) {
                                char rerr[512] = { 0 };
                                int r = playlist_advance(&st, 1, now,
                                                         rerr, sizeof rerr);
                                if (r < 0)
                                        /* keep rotating: a broken entry is
                                         * skipped, not fatal */
                                        LOGW("ipc: rotation skipped an entry "
                                             "(%s)", rerr);
                                else
                                        LOGI("ipc: rotation -> %s", st.pl[r]);
                        }
                }
                /* current fade darkness for this frame (0 = none) */
                const f64 fade_black = fade_black_now(&st, now);

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
                                        /* renderer windows are indexed by the
                                         * PLATFORM window number (see begin()
                                         * below) — under hotplug that is not
                                         * always the slot number */
                                        rend->resize(rend, winidx[i], nw, nh);
                                        if (!media)
                                                rtscene_flush_caches(sf.sc);
                                }
                        }
                }

                /* pointer (consume-on-read: dx/dy and click edges are each
                 * delivered exactly once by the platform layer) — scenes
                 * only; media playback has no interaction */
                RPointerState ptr;
                if (!media && plat->pointer(plat, &ptr)) {
                        for (int i = 0; i < nmon; i++) {
                                f64 lx = (ptr.x - mons[i].x) * fb_scale[i];
                                f64 ly = (ptr.y - mons[i].y) * fb_scale[i];
                                f64 dx = ptr.dx * fb_scale[i];
                                f64 dy = ptr.dy * fb_scale[i];
                                rtscene_pointer(sf.sc, i, lx, ly, dx, dy, ptr.pressed);
                                /* every latched press+release between two
                                 * polls is a click — each splashes at its own
                                 * latched press position (a same-poll double
                                 * click used to collapse onto the last spot) */
                                for (u32 c = 0; c < ptr.click_qn && c < 4; c++)
                                        rtscene_click(sf.sc, i,
                                                      (ptr.click_q[c][0] - mons[i].x) * fb_scale[i],
                                                      (ptr.click_q[c][1] - mons[i].y) * fb_scale[i]);
                        }
                }

                /* synthetic clicks (test hook): each fires at its own @t (default
                 * 0.3 s + 0.2 s spacing) — same entry point a real click takes
                 * (rtscene_click), at the same device-pixel transform main.c
                 * applies to real clicks */
                while (!media && fc_next < fc_n && t >= fc_t[fc_next]) {
                        for (int i = 0; i < nmon; i++)
                                rtscene_click(sf.sc, i,
                                              fc_x[fc_next] * (f64)fbs[i].w,
                                              fc_y[fc_next] * (f64)fbs[i].h);
                        fc_next++;
                }

                /* synthetic pointer (test hook): overrides the real one —
                 * benchmarks need a deterministic drag, not a resting cursor */
                if (!media && fake_ptr_speed > 0) {
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

                /* render + present — skipped entirely while no surface is
                 * visible (idle power-save), EXCEPT when the present path
                 * itself must carry the compositor probe (wsi_probe — one
                 * frozen-clock frame per idle tick) or an audit burst is
                 * running. wlshim's idle probe re-draws when the compositor
                 * comes back, and the frozen clock resumes the scene or the
                 * video exactly where it stopped. */
                if (render_now) {
                        /* media mode: advance the playhead ONCE per frame
                         * (frozen while hidden — dt is 0 above), then the
                         * same per-monitor present machinery */
                        const RVideoFrame *vf = media
                                ? rev_video_display(vid, dt) : NULL;
                        for (int i = 0; i < nmon; i++) {
                                if (winidx[i] < 0 || !fbs[i].px)
                                        continue;
                                f64 r_t0 = rev_monotonic();
                                if (media) {
                                        render_media_frame(rend, vf, a.scale,
                                                           &fbs[i], winidx[i]);
                                } else if (gpu_layers) {
                                        fl.w = fbs[i].w;
                                        fl.h = fbs[i].h;
                                        fl.n = 0;
                                        fl.sim_offload = sim_offload;
                                        fl.point_offload = point_offload;
                                        fl.grad_offload = grad_offload;
                                        fl.video_offload = video_offload;
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
                                                cmd.rsim = fl.cmds[k].rsim;
                                                cmd.psim = fl.cmds[k].psim;
                                                cmd.grad = fl.cmds[k].grad;
                                                cmd.vsim = fl.cmds[k].vsim;
                                                rend->layer(rend, &cmd);
                                        }
                                        /* theme-switch fade on the GPU tiers:
                                         * one constant-black layer stretched
                                         * over the frame with opacity = the
                                         * current darkness — the reserved
                                         * content id means it uploads ONCE
                                         * (a 1x1 texture) for the whole
                                         * transition, on every backend */
                                        if (fade_black > 0.0) {
                                                static const u32 black_px[1] = {
                                                        0xFF000000u
                                                };
                                                RLayerCmd cmd;
                                                memset(&cmd, 0, sizeof cmd);
                                                cmd.px = black_px;
                                                cmd.w = 1;
                                                cmd.h = 1;
                                                cmd.place_w = fl.w;
                                                cmd.place_h = fl.h;
                                                cmd.blend = REV_BLEND_NORMAL;
                                                cmd.opacity = fade_black;
                                                cmd.linear = false;
                                                cmd.content_id = REV_FADE_CID;
                                                rend->layer(rend, &cmd);
                                        }
                                        rend->end(rend, NULL);
                                } else {
                                        /* CPU-composited mode still needs begin():
                                         * renderers target their PRESENT path per
                                         * window (cur_win) and size their viewports
                                         * there — without it, GL presents into a
                                         * 0x0 viewport (black screen) and multi-
                                         * monitor presents everything to window 0 */
                                        if (!rend->begin(rend, winidx[i], fbs[i].w, fbs[i].h))
                                                continue;
                                        memset(fbs[i].px, 0, (size_t)fbs[i].w * fbs[i].h * 4);
                                        rtscene_render(sf.sc, &fbs[i], i, t, dt);
                                        f64 s_ms, ra_ms;
                                        rtscene_profile(sf.sc, &s_ms, &ra_ms);
                                        prof_sim += s_ms;
                                        prof_raster += ra_ms;
                                        /* theme-switch fade: darken the finished
                                         * CPU frame before presenting */
                                        if (fade_black > 0.0)
                                                rev_comp_fade(&fbs[i], fade_black);
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
                }

                /* test hook: screenshot */
                if (shot_after >= 0 && t >= shot_after && !shot_done) {
                        shot_done = true;
                        /* naming follows the ACTIVE window count, not the
                         * high-water slot count: after a hotplug removal the
                         * surviving single window must get the plain FILE.png
                         * name (indexed names appear only when two or more
                         * windows are actually captured) */
                        int nwin = 0;
                        for (int i = 0; i < nmon; i++)
                                if (winidx[i] >= 0)
                                        nwin++;
                        for (int i = 0; i < nmon; i++) {
                                if (winidx[i] < 0)
                                        continue;
                                u32 sw = 0, sh = 0;
                                u32 *shot = plat->screenshot(plat, winidx[i], &sw, &sh);
                                if (shot) {
                                        char path[600];
                                        if (nwin > 1)
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

                /* hot reload — themes poll every file in the folder: a
                 * directory's own mtime never moves on an in-place edit,
                 * which silently killed theme hot-reload until now. Plain
                 * .rev scenes and .revpacks keep the single-file check.
                 * (Media mode has no hot reload — a changed video file is
                 * not re-opened mid-playback; documented limitation.) */
                if (!media && now - last_reload_check > 0.5) {
                        last_reload_check = now;
                        bool scene_changed;
                        if (sf.theme) {
                                scene_changed = theme_files_changed(sf.theme);
                        } else {
                                f64 mt = rev_file_mtime(sf.path);
                                scene_changed = mt > 0 && mt != sf.mtime;
                        }
                        if (scene_changed) {
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
                                                     "(renderer/driver lacks usable "
                                                     "timer queries — see the "
                                                     "renderer's init log)");
                                        else
                                                LOGI("perf: *gpu = async driver timer, "
                                                     "several frames behind — not "
                                                     "CPU wall time");
                                        LOGI("perf: frame= is CPU wall time per "
                                             "frame INCLUDING the pacing wait; "
                                             "when fps is capped to the display "
                                             "refresh, frame ~= refresh interval "
                                             "and the remainder is vsync idle, "
                                             "not render cost");
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
                 * while it rendered. Media mode paces at the SOURCE rate
                 * (uploads happen at video speed, not render speed). */
                int fps = fps_override > 0
                        ? fps_override
                        : (media ? (int)(rev_video_fps(vid) + 0.5)
                                 : rtscene_fps(sf.sc));
                if (fps_override <= 0 && plat->monitor_refresh) {
                        /* cap scene fps to the fastest connected display — the
                         * extra frames could never be shown (they would just
                         * pile up as compositor drops). An explicit --fps
                         * override is always honored as-is. Uses the
                         * per-SLOT refresh cache: after a hotplug the packed
                         * platform indices no longer align with slot numbers,
                         * so re-querying by slot index would read the wrong
                         * output (or none). */
                        int hz = 0;
                        for (int i = 0; i < nmon; i++) {
                                if (winidx[i] < 0)
                                        continue;
                                if (mon_hz[i] > hz)
                                        hz = mon_hz[i];
                        }
                        if (hz > 0 && fps > hz) {
                                LOGD("pacing: scene %d fps capped to display %d Hz", fps, hz);
                                fps = hz;
                        }
                }
                if (fps < 1) fps = 1;
                if (fps > 240) fps = 240;
                /* ABSOLUTE treadmill: the deadline advances by exactly one
                 * period from the PREVIOUS deadline. The old
                 * `deadline = now + frame_len` re-anchored on loop-top, so
                 * every frame inherited the pacing wait's wake-up slack
                 * (poll-slice rounding + dispatch, ~0.4 ms) — a steady creep
                 * that measured 58 fps against a 60 Hz display (frame avg
                 * 17.2 ms where p50 was 17.0). A frame that overruns a whole
                 * period resyncs (skips the missed slots) instead of letting
                 * the grid drift; so does a visibility flip (idle period ->
                 * frame period). While hidden, pace at the probe cadence. */
                f64 frame_len = (visible || audit) ? (1.0 / fps) : IDLE_PACE_S;
                {
                        const bool active = visible || audit;
                        if (active != prev_active || now >= next_deadline + frame_len)
                                next_deadline = now + frame_len;    /* resync */
                        else
                                next_deadline += frame_len;        /* keep the grid */
                }
                for (;;) {
                        f64 rem = next_deadline - rev_monotonic();
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
                prev_visible = visible;
                prev_active = visible || audit;
                if (visible || audit)
                        frames++;
        }

        LOGI("shutting down");
        if (ipc) {
                ipc_serve_stop(ipc);
                ipc = NULL;
        }
        renderer_destroy(rend);
        for (int i = 0; i < nmon; i++)
                free(fbs[i].px);
        platform_destroy(plat);
        if (vid)
                rev_video_close(vid);
        media_scratch_free();
        scene_file_free(&sf);
        scene_file_free(&st.next);
        free(base_dir);
        return 0;
}
