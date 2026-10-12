/* ============================================================================
 * Reverie Engine — main.c
 * The reverie daemon: loads a .rev scene, creates one desktop window per
 * monitor, and runs the interactive frame loop.
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
#include "png.h"
#include "rev.h"
#include "scene.h"
#include "util.h"
#include "x11.h"

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
"usage: reverie [options] <scene.rev>\n"
"\n"
"options:\n"
"  -f, --fps N        override the scene's target fps\n"
"  -m, --monitor N    render only on monitor N\n"
"  -o, --once         render one frame to PNG and exit (no X11 needed)\n"
"  -s, --size WxH     output size for --once (default 1920x1080)\n"
"  -O, --out FILE     output PNG path for --once (default reverie.png)\n"
"  -l, --list         list monitors and exit\n"
"  -v, --verbose      debug logging\n"
"  -q, --quiet        errors only\n"
"  -h, --help         this help\n");
}

typedef struct {
	RArena *ir_arena;
	RSceneIR *ir;
	RTScene *sc;
	f64 mtime;
	char path[512];
} SceneFile;

static bool scene_load(SceneFile *sf, const char *path, const char *base_dir,
                       char *err, size_t errcap)
{
	size_t len = 0;
	u8 *text = rev_read_file(path, &len);
	if (!text) {
		snprintf(err, errcap, "cannot read '%s'", path);
		return false;
	}
	RArena *ar = NULL;
	RSceneIR *ir = rev_parse_scene((const char *)text, len, &ar, err, errcap);
	free(text);
	if (!ir)
		return false;

	RTScene *sc = rtscene_compile(ir, ar, base_dir, err, errcap);
	if (!sc) {
		arena_destroy(ar);
		return false;
	}

	/* free the previous scene, if any */
	if (sf->sc)
		rtscene_free(sf->sc);
	if (sf->ir_arena)
		arena_destroy(sf->ir_arena);

	sf->ir_arena = ar;
	sf->ir = ir;
	sf->sc = sc;
	sf->mtime = rev_file_mtime(path);
	snprintf(sf->path, sizeof sf->path, "%s", path);
	return true;
}

int main(int argc, char **argv)
{
	const char *scene_path = NULL;
	int fps_override = 0;
	int only_monitor = -1;
	bool once = false, list = false;
	u32 out_w = 1920, out_h = 1080;
	const char *out_png = "reverie.png";

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-f") == 0 || strcmp(a, "--fps") == 0) {
			if (++i >= argc) { usage(); return 1; }
			fps_override = atoi(argv[i]);
		} else if (strcmp(a, "-m") == 0 || strcmp(a, "--monitor") == 0) {
			if (++i >= argc) { usage(); return 1; }
			only_monitor = atoi(argv[i]);
		} else if (strcmp(a, "-o") == 0 || strcmp(a, "--once") == 0) {
			once = true;
		} else if (strcmp(a, "-s") == 0 || strcmp(a, "--size") == 0) {
			if (++i >= argc) { usage(); return 1; }
			if (sscanf(argv[i], "%ux%u", &out_w, &out_h) != 2) {
				fprintf(stderr, "reverie: bad --size '%s'\n", argv[i]);
				return 1;
			}
		} else if (strcmp(a, "-O") == 0 || strcmp(a, "--out") == 0) {
			if (++i >= argc) { usage(); return 1; }
			out_png = argv[i];
		} else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
			list = true;
		} else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
			rev_log_set_level(REV_LOG_DEBUG);
		} else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
			rev_log_set_level(REV_LOG_ERROR);
		} else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			usage();
			return 0;
		} else if (a[0] == '-') {
			fprintf(stderr, "reverie: unknown option '%s'\n", a);
			usage();
			return 1;
		} else {
			scene_path = a;
		}
	}

	if (list) {
		X11 *x = x11_connect(NULL, 0);
		if (!x) {
			fprintf(stderr, "reverie: cannot connect to X\n");
			return 1;
		}
		RRect mons[8];
		int n = x11_monitors(x, mons, 8);
		printf("%d monitor(s):\n", n);
		for (int i = 0; i < n; i++)
			printf("  [%d] %dx%d at %d,%d\n", i, mons[i].w, mons[i].h, mons[i].x, mons[i].y);
		x11_disconnect(x);
		return 0;
	}

	if (!scene_path) {
		usage();
		return 1;
	}

	char *base_dir = rev_path_dirname(scene_path);
	char err[512];
	SceneFile sf;
	memset(&sf, 0, sizeof sf);
	if (!scene_load(&sf, scene_path, base_dir, err, sizeof err)) {
		fprintf(stderr, "reverie: %s\n", err);
		free(base_dir);
		return 1;
	}
	LOGI("scene '%s': %d layer(s), %d fps, %s",
	     scene_path, rtscene_layer_count(sf.sc), rtscene_fps(sf.sc),
	     rtscene_is_animated(sf.sc) ? "animated" : "static");
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

	/* ---- X11 session ---- */
	X11 *x = x11_connect(err, sizeof err);
	if (!x) {
		fprintf(stderr, "reverie: %s\n", err);
		return 1;
	}

	RRect mons[8];
	int nmon = x11_monitors(x, mons, 8);
	if (only_monitor >= 0) {
		if (only_monitor >= nmon) {
			fprintf(stderr, "reverie: monitor %d does not exist (%d found)\n",
			        only_monitor, nmon);
			x11_disconnect(x);
			return 1;
		}
		mons[0] = mons[only_monitor];
		nmon = 1;
	}
	LOGI("%d monitor(s)", nmon);

	/* framebuffers + windows */
	RFb fbs[8];
	int winidx[8];
	for (int i = 0; i < nmon; i++) {
		fbs[i].w = (u32)mons[i].w;
		fbs[i].h = (u32)mons[i].h;
		fbs[i].px = (u32 *)calloc((size_t)fbs[i].w * fbs[i].h, 4);
		winidx[i] = x11_add_window(x, mons[i]);
		if (winidx[i] < 0)
			LOGW("monitor %d: window creation failed", i);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	f64 t0 = rev_monotonic();
	f64 last_frame = t0;
	f64 last_reload_check = t0;
	i32 last_px = -1, last_py = -1;

	while (g_run) {
		f64 now = rev_monotonic();
		f64 dt = now - last_frame;
		f64 t = now - t0;

		x11_pump_events(x);

		/* window resize handling: recreate on size change */
		for (int i = 0; i < nmon; i++) {
			RRect g;
			if (winidx[i] >= 0 && x11_win_geometry(x, winidx[i], &g)) {
				if ((u32)g.w != fbs[i].w || (u32)g.h != fbs[i].h) {
					LOGI("monitor %d: geometry now %ux%u, re-creating", i, g.w, g.h);
					free(fbs[i].px);
					fbs[i].w = (u32)g.w;
					fbs[i].h = (u32)g.h;
					fbs[i].px = (u32 *)calloc((size_t)fbs[i].w * fbs[i].h, 4);
					/* re-create the window (SHM segments sized to old geometry) */
					winidx[i] = x11_add_window(x, g);
					rtscene_flush_caches(sf.sc);
				}
			}
		}

		/* pointer */
		i32 rx = 0, ry = 0;
		u32 mask = 0;
		bool have_ptr = x11_query_pointer(x, &rx, &ry, &mask);
		if (have_ptr) {
			bool pressed = (mask & ((1u << 8) | (1u << 9) | (1u << 10))) != 0;
			for (int i = 0; i < nmon; i++) {
				f64 lx = (f64)(rx - mons[i].x);
				f64 ly = (f64)(ry - mons[i].y);
				f64 dx = (last_px >= 0) ? (f64)(rx - last_px) : 0;
				f64 dy = (last_py >= 0) ? (f64)(ry - last_py) : 0;
				rtscene_pointer(sf.sc, i, lx, ly, dx, dy, pressed);
			}
			last_px = rx;
			last_py = ry;
		}

		/* render + present */
		for (int i = 0; i < nmon; i++) {
			if (winidx[i] < 0 || !fbs[i].px)
				continue;
			memset(fbs[i].px, 0, (size_t)fbs[i].w * fbs[i].h * 4);
			rtscene_render(sf.sc, &fbs[i], i, t, dt);
			x11_present(x, winidx[i], fbs[i].px, fbs[i].w, fbs[i].h);
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

		/* frame pacing */
		int fps = fps_override > 0 ? fps_override : rtscene_fps(sf.sc);
		if (fps < 1) fps = 1;
		if (fps > 240) fps = 240;
		f64 frame_len = 1.0 / fps;
		f64 spent = rev_monotonic() - now;
		f64 sleep = frame_len - spent;
		if (sleep > 0.0005) {
			struct timespec ts;
			ts.tv_sec = (time_t)sleep;
			ts.tv_nsec = (long)((sleep - (f64)ts.tv_sec) * 1e9);
			nanosleep(&ts, NULL);
		}
		last_frame = rev_monotonic();
	}

	LOGI("shutting down");
	for (int i = 0; i < nmon; i++)
		free(fbs[i].px);
	x11_disconnect(x);
	rtscene_free(sf.sc);
	arena_destroy(sf.ir_arena);
	free(base_dir);
	return 0;
}
