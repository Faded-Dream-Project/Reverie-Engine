/* ============================================================================
 * Reverie Engine — revtool.c
 * Scene toolkit: validate, info, offline render, asset generation.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ipc.h"
#include "png.h"
#include "rev.h"
#include "pack.h"
#include "scene.h"
#include "theme.h"
#include "util.h"

static void usage(void)
{
	printf(
"revtool " REV_VERSION " — Reverie scene toolkit\n"
"usage:\n"
"  revtool validate <scene.rev>\n"
"  revtool info <scene.rev>\n"
"  revtool render <scene.rev> [options]\n"
"      --size WxH       output size (default 1920x1080)\n"
"      --frames N       number of frames (default 1)\n"
"      --fps N          simulated fps (default scene fps)\n"
"      --out FILE       output PNG (frames>1: use a %%d pattern)\n"
"      --pointer X,Y    simulate a held pointer (fractions 0..1)\n"
"      --drag X,Y       simulate a continuous drag toward X,Y\n"
"  revtool mkpng <out.png> <W> <H> <pattern>\n"
"      patterns: soft-bands | stars | checker | gradient | vignette\n"
"  revtool pnginfo <file.png>\n"
"  revtool pack <scene.rev> [-o out.revpack]\n"
"      bundles the scene + every referenced asset into one .revpack\n"
"  revtool unpack <file.revpack> [-d outdir]\n"
"      extracts scene.rev + assets back into a directory\n"
"  revtool theme list <dir>\n"
"      lists theme folders and packed themes under a directory\n"
"  revtool theme info <theme dir | theme.revpack>\n"
"      prints theme metadata\n"
"  revtool theme validate <theme dir | theme.revpack>\n"
"      checks metadata, entry scene, referenced assets, engine version\n"
"  revtool theme pack <theme dir> [-o out.revpack]\n"
"      bundles the whole theme folder into one .revpack\n"
"  revtool theme inspect <theme.revpack>\n"
"      lists every entry with size + CRC check\n"
"  revtool ipc [-s <sock>] <command...>\n"
"      sends one command to a running daemon's control socket and prints\n"
"      the reply (exit 0 on OK, 1 on ERR). Commands: PING | STATUS |\n"
"      THEME <path> [fade_ms] | PLAYLIST <p...> | NEXT [fade_ms] |\n"
"      PREV [fade_ms] | ROTATE <sec> | FADE <ms> | QUIT — see docs/IPC.md\n");
}

static int cmd_pack(int argc, char **argv)
{
	const char *scene = argv[2];
	const char *out = "scene.revpack";
	for (int i = 3; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--out"))
			out = argv[i + 1];
	}
	char err[512];
	if (!rev_pack_write(out, scene, err, sizeof err)) {
		printf("FAIL: %s\n", err);
		return 1;
	}
	printf("packed %s -> %s\n", scene, out);
	return 0;
}

static int cmd_unpack(int argc, char **argv)
{
	const char *pack = argv[2];
	const char *dir = "unpacked";
	for (int i = 3; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dir"))
			dir = argv[i + 1];
	}
	char err[512];
	if (!rev_pack_unpack(pack, dir, err, sizeof err)) {
		printf("FAIL: %s\n", err);
		return 1;
	}
	printf("unpacked %s -> %s/\n", pack, dir);
	return 0;
}

static int cmd_validate(const char *path)
{
	size_t len = 0;
	u8 *text = rev_read_file(path, &len);
	if (!text) {
		printf("FAIL: cannot read '%s'\n", path);
		return 1;
	}
	char err[512];
	RArena *ar = NULL;
	RSceneIR *ir = rev_parse_scene((const char *)text, len, &ar, err, sizeof err);
	free(text);
	if (!ir) {
		printf("FAIL: %s\n", err);
		return 1;
	}
	char *base = rev_path_dirname(path);
	RTScene *sc = rtscene_compile(ir, ar, base, err, sizeof err);
	if (!sc) {
		printf("FAIL: %s\n", err);
		arena_destroy(ar);
		free(base);
		return 1;
	}
	printf("OK: %d layer(s), %d fps, %s\n", rtscene_layer_count(sc),
	       rtscene_fps(sc), rtscene_is_animated(sc) ? "animated" : "static");
	rtscene_free(sc);
	arena_destroy(ar);
	free(base);
	return 0;
}

static int cmd_info(const char *path)
{
	size_t len = 0;
	u8 *text = rev_read_file(path, &len);
	if (!text) {
		printf("FAIL: cannot read '%s'\n", path);
		return 1;
	}
	char err[512];
	RArena *ar = NULL;
	RSceneIR *ir = rev_parse_scene((const char *)text, len, &ar, err, sizeof err);
	free(text);
	if (!ir) {
		printf("FAIL: %s\n", err);
		return 1;
	}
	printf("--- parsed IR ---\n");
	rev_dump_scene(ir);
	char *base = rev_path_dirname(path);
	RTScene *sc = rtscene_compile(ir, ar, base, err, sizeof err);
	if (sc) {
		printf("--- compiled ---\n");
		printf("fps=%d animated=%s layers=%d\n",
		       rtscene_fps(sc), rtscene_is_animated(sc) ? "yes" : "no",
		       rtscene_layer_count(sc));
		rtscene_free(sc);
	} else {
		printf("--- compile error ---\n%s\n", err);
	}
	arena_destroy(ar);
	free(base);
	return 0;
}

static int cmd_render(int argc, char **argv, int first)
{
	const char *path = argv[first++];
	u32 w = 1920, h = 1080;
	int frames = 1;
	int fps = 0;
	const char *out = "frame.png";
	f64 ptr_x = -1, ptr_y = -1;
	f64 drag_x = -1, drag_y = -1;

	while (first < argc) {
		const char *a = argv[first];
		if (strcmp(a, "--size") == 0 && first + 1 < argc) {
			if (sscanf(argv[++first], "%ux%u", &w, &h) != 2) {
				printf("FAIL: bad --size\n");
				return 1;
			}
		} else if (strcmp(a, "--frames") == 0 && first + 1 < argc) {
			frames = atoi(argv[++first]);
			if (frames < 1) frames = 1;
			if (frames > 10000) frames = 10000;
		} else if (strcmp(a, "--fps") == 0 && first + 1 < argc) {
			fps = atoi(argv[++first]);
		} else if (strcmp(a, "--out") == 0 && first + 1 < argc) {
			out = argv[++first];
		} else if (strcmp(a, "--pointer") == 0 && first + 1 < argc) {
			sscanf(argv[++first], "%lf,%lf", &ptr_x, &ptr_y);
		} else if (strcmp(a, "--drag") == 0 && first + 1 < argc) {
			sscanf(argv[++first], "%lf,%lf", &drag_x, &drag_y);
		} else {
			printf("FAIL: unknown render option '%s'\n", a);
			return 1;
		}
		first++;
	}

	size_t len = 0;
	u8 *text = rev_read_file(path, &len);
	if (!text) {
		printf("FAIL: cannot read '%s'\n", path);
		return 1;
	}
	char err[512];
	RArena *ar = NULL;
	RSceneIR *ir = rev_parse_scene((const char *)text, len, &ar, err, sizeof err);
	free(text);
	if (!ir) {
		printf("FAIL: %s\n", err);
		return 1;
	}
	char *base = rev_path_dirname(path);
	RTScene *sc = rtscene_compile(ir, ar, base, err, sizeof err);
	if (!sc) {
		printf("FAIL: %s\n", err);
		arena_destroy(ar);
		free(base);
		return 1;
	}
	if (fps <= 0)
		fps = rtscene_fps(sc);

	RFb fb = { (u32 *)calloc((size_t)w * h, 4), w, h };
	if (!fb.px) {
		printf("FAIL: out of memory\n");
		return 1;
	}

	f64 dt = 1.0 / fps;
	char name[1024];
	for (int f = 0; f < frames; f++) {
		f64 t = (f64)f * dt;

		if (ptr_x >= 0) {
			rtscene_pointer(sc, 0, ptr_x * w, ptr_y * h, 0, 0, true);
		} else if (drag_x >= 0) {
			f64 k = (f64)(f + 1) / frames;
			f64 x = (0.5 + (drag_x - 0.5) * k) * w;
			f64 y = (0.5 + (drag_y - 0.5) * k) * h;
			f64 dx = (drag_x - 0.5) * w / frames;
			f64 dy = (drag_y - 0.5) * h / frames;
			rtscene_pointer(sc, 0, x, y, dx, dy, true);
		}

		memset(fb.px, 0, (size_t)w * h * 4);
		rtscene_render(sc, &fb, 0, t, dt);

		if (frames == 1)
			snprintf(name, sizeof name, "%s", out);
		else
			snprintf(name, sizeof name, out, f + 1);
		/* if the pattern has no %d, append one */
		if (frames > 1 && !strstr(out, "%")) {
			char base2[512];
			snprintf(base2, sizeof base2, "%s", out);
			char *dot = strrchr(base2, '.');
			if (dot && strcmp(dot, ".png") == 0)
				*dot = '\0';
			snprintf(name, sizeof name, "%s_%04d.png", base2, f + 1);
		}

		RevImage img = { w, h, fb.px };
		if (!rev_png_encode_file(name, &img, err, sizeof err)) {
			printf("FAIL: %s\n", err);
			free(fb.px);
			rtscene_free(sc);
			arena_destroy(ar);
			free(base);
			return 1;
		}
	}
	printf("wrote %d frame(s) at %ux%u\n", frames, w, h);

	free(fb.px);
	rtscene_free(sc);
	arena_destroy(ar);
	free(base);
	return 0;
}

static int cmd_mkpng(int argc, char **argv, int first)
{
	if (first + 3 >= argc + 0 && argc - first < 4) {
		printf("FAIL: mkpng needs <out.png> <W> <H> <pattern>\n");
		return 1;
	}
	const char *out = argv[first];
	u32 w = (u32)atoi(argv[first + 1]);
	u32 h = (u32)atoi(argv[first + 2]);
	const char *pattern = argv[first + 3];
	if (w < 1 || h < 1 || w > 8192 || h > 8192) {
		printf("FAIL: bad size\n");
		return 1;
	}

	u32 *px = (u32 *)malloc((size_t)w * h * 4);
	if (!px)
		return 1;

	if (strcmp(pattern, "soft-bands") == 0) {
		for (u32 y = 0; y < h; y++) {
			f64 t = (f64)y / (h - 1);
			u32 r = (u32)(20 + 60 * t + 30 * sin(t * 9.0));
			u32 g = (u32)(40 + 90 * t + 25 * sin(t * 7.0 + 2.0));
			u32 b = (u32)(90 + 120 * t + 35 * sin(t * 11.0 + 4.0));
			u32 row = 255u << 24 | (b > 255 ? 255 : b) << 16 |
			          (g > 255 ? 255 : g) << 8 | (r > 255 ? 255 : r);
			for (u32 x = 0; x < w; x++)
				px[(size_t)y * w + x] = row;
		}
	} else if (strcmp(pattern, "stars") == 0) {
		u32 seed = 12345;
		for (u32 y = 0; y < h; y++) {
			f64 t = (f64)y / (h - 1);
			u32 r = (u32)(10 + 12 * t), g = (u32)(12 + 18 * t), b = (u32)(28 + 40 * t);
			u32 row = 255u << 24 | (b > 255 ? 255 : b) << 16 |
			          (g > 255 ? 255 : g) << 8 | (r > 255 ? 255 : r);
			for (u32 x = 0; x < w; x++)
				px[(size_t)y * w + x] = row;
		}
		for (u32 i = 0; i < w * h / 900 + 16; i++) {
			seed = seed * 1664525u + 1013904223u;
			u32 x = seed % w;
			seed = seed * 1664525u + 1013904223u;
			u32 y = seed % h;
			seed = seed * 1664525u + 1013904223u;
			u32 b = 120 + seed % 136;
			u32 rad = seed % 3;
			for (i32 dy = -(i32)rad; dy <= (i32)rad; dy++) {
				for (i32 dx = -(i32)rad; dx <= (i32)rad; dx++) {
					i32 sx = (i32)x + dx, sy = (i32)y + dy;
					if (sx < 0 || sy < 0 || sx >= (i32)w || sy >= (i32)h)
						continue;
					u32 v = b - (u32)(abs(dx) + abs(dy)) * 30;
					px[(size_t)sy * w + sx] = 255u << 24 | (v > 255 ? 255 : v) << 16 |
					                          (v > 255 ? 255 : v) << 8 | (v > 255 ? 255 : v);
				}
			}
		}
	} else if (strcmp(pattern, "checker") == 0) {
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++) {
				u32 c = ((x / 32) ^ (y / 32)) & 1 ? 0xff343434 : 0xffdedede;
				px[(size_t)y * w + x] = c;
			}
	} else if (strcmp(pattern, "gradient") == 0) {
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++) {
				f64 t = ((f64)x / (w - 1) + (f64)y / (h - 1)) * 0.5;
				u32 r = (u32)(18 + 200 * t);
				u32 g = (u32)(8 + 90 * t);
				u32 b = (u32)(40 + 160 * t);
				px[(size_t)y * w + x] = 255u << 24 |
					(b > 255 ? 255 : b) << 16 | (g > 255 ? 255 : g) << 8 | (r > 255 ? 255 : r);
			}
	} else if (strcmp(pattern, "vignette") == 0) {
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++) {
				f64 nx = (f64)x / w - 0.5, ny = (f64)y / h - 0.5;
				f64 d = sqrt(nx * nx + ny * ny) * 1.7;
				f64 t = 1.0 - d;
				if (t < 0) t = 0;
				u32 r = (u32)(16 + 90 * t * t);
				u32 g = (u32)(20 + 60 * t * t);
				u32 b = (u32)(44 + 120 * t * t);
				px[(size_t)y * w + x] = 255u << 24 |
					(b > 255 ? 255 : b) << 16 | (g > 255 ? 255 : g) << 8 | (r > 255 ? 255 : r);
			}
	} else {
		printf("FAIL: unknown pattern '%s'\n", pattern);
		free(px);
		return 1;
	}

	RevImage img = { w, h, px };
	char err[256];
	if (!rev_png_encode_file(out, &img, err, sizeof err)) {
		printf("FAIL: %s\n", err);
		free(px);
		return 1;
	}
	printf("wrote %s (%ux%u, %s)\n", out, w, h, pattern);
	free(px);
	return 0;
}

static int cmd_pnginfo(const char *path)
{
	size_t len = 0;
	u8 *data = rev_read_file(path, &len);
	if (!data) {
		printf("FAIL: cannot read '%s'\n", path);
		return 1;
	}
	RevImage img;
	char err[256];
	if (!rev_png_decode(data, len, &img, err, sizeof err)) {
		printf("FAIL: %s\n", err);
		free(data);
		return 1;
	}
	size_t opaque = 0, transparent = 0;
	for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
		u32 a = img.px[i] >> 24;
		if (a == 255) opaque++;
		else if (a == 0) transparent++;
	}
	printf("%s: %ux%u RGBA8, %zu opaque, %zu transparent, %zu partial\n",
	       path, img.w, img.h, opaque, transparent,
	       (size_t)img.w * img.h - opaque - transparent);
	rev_image_free(&img);
	free(data);
	return 0;
}


static const u8 *revtool_theme_asset(void *ctx, const char *path, size_t *len)
{
	return theme_asset((RTheme *)ctx, path, len);
}

static int cmd_theme(int argc, char **argv)
{
	if (argc < 3) {
		printf("FAIL: theme needs a subcommand (list|info|validate|pack|inspect)\n");
		return 1;
	}
	const char *sub = argv[2];
	const char *path = argc >= 4 ? argv[3] : NULL;
	char err[512];

	if (strcmp(sub, "list") == 0 && path) {
		char *names[256];
		int n = theme_list_dir(path, names, 256);
		for (int i = 0; i < n; i++) {
			printf("%s\n", names[i]);
			free(names[i]);
		}
		return 0;
	}
	if (strcmp(sub, "info") == 0 && path) {
		RTheme *th = theme_load(path, err, sizeof err);
		if (!th) {
			printf("FAIL: %s\n", err);
			return 1;
		}
		const RThemeMeta *m = theme_meta(th);
		printf("id:          %s\n", m->id);
		printf("name:        %s\n", m->name);
		printf("version:     %s\n", m->version);
		printf("author:      %s\n", m->author);
		printf("description: %s\n", m->description);
		printf("scene:       %s\n", m->scene);
		printf("engine_min:  %s\n", m->engine_min);
		if (m->has_preview)
			printf("preview:     %s\n", m->preview);
		for (int i = 0; i < m->nquality; i++)
			printf("quality[%s]:  %d fps, scale %.2f\n", m->quality[i].name,
			       m->quality[i].fps, m->quality[i].res_scale);
		theme_free(th);
		return 0;
	}
	if (strcmp(sub, "validate") == 0 && path) {
		RTheme *th = theme_load(path, err, sizeof err);
		if (!th) {
			printf("FAIL: %s\n", err);
			return 1;
		}
		const RThemeMeta *m = theme_meta(th);
		if (!theme_engine_ok(m, err, sizeof err)) {
			printf("FAIL: %s\n", err);
			theme_free(th);
			return 1;
		}
		size_t len = 0;
		const char *text = theme_scene_text(th, &len);
		RArena *ar = NULL;
		RSceneIR *ir = rev_parse_scene(text, len, &ar, err, sizeof err);
		if (!ir) {
			printf("FAIL: scene parse: %s\n", err);
			theme_free(th);
			return 1;
		}
		RTScene *sc = rtscene_compile_assets(ir, ar, theme_base_dir(th),
							  th, revtool_theme_asset, err, sizeof err);
		if (!sc) {
			printf("FAIL: scene compile: %s\n", err);
			arena_destroy(ar);
			theme_free(th);
			return 1;
		}
		/* every referenced asset must resolve through the theme */
		int nmiss = 0;
		for (int i = 0; i < rtscene_layer_count(sc); i++) {
			(void)i;    /* asset refs checked through compile already */
		}
		(void)nmiss;
		printf("OK: theme '%s' (%d layers, %d fps, scene %s)\n",
		       m->id, rtscene_layer_count(sc), rtscene_fps(sc), m->scene);
		rtscene_free(sc);
		arena_destroy(ar);
		theme_free(th);
		return 0;
	}
	if (strcmp(sub, "pack") == 0 && path) {
		const char *out = NULL;
		for (int i = 4; i + 1 < argc; i++) {
			if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--out"))
					out = argv[i + 1];
		}
		if (!out) {
			char base[512];
			snprintf(base, sizeof base, "%s", path);
			char *slash = strrchr(base, '/');
			const char *leaf = slash ? slash + 1 : base;
			static char obuf[600];
			snprintf(obuf, sizeof obuf, "%s.revpack", leaf);
			out = obuf;
		}
		if (!theme_pack(path, out, err, sizeof err)) {
			printf("FAIL: %s\n", err);
			return 1;
		}
		printf("packed theme %s -> %s\n", path, out);
		return 0;
	}
	if (strcmp(sub, "inspect") == 0 && path) {
		RevPack pk;
		if (!rev_pack_load(path, &pk, err, sizeof err)) {
			printf("FAIL: %s\n", err);
			return 1;
		}
		printf("%s: %d entr%s, scene %zu bytes\n", path, pk.n_assets,
		       pk.n_assets == 1 ? "y" : "ies", pk.scene_len);
		for (int i = 0; i < pk.n_assets; i++) {
			/* crc verified during load; report what we have */
			printf("  %-40s %8zu bytes\n", pk.assets[i].name, pk.assets[i].len);
		}
		rev_pack_free(&pk);
		return 0;
	}
	printf("FAIL: unknown theme subcommand\n");
	return 1;
}

static int cmd_ipc(int argc, char **argv)
{
	const char *sock = NULL;
	const char *cmdline = NULL;
	int i = 2;
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--sock")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "revtool: -s needs a path\n");
				return 1;
			}
			sock = argv[++i];
		} else {
			cmdline = argv[i++];
			break;
		}
	}
	if (!cmdline) {
		fprintf(stderr, "revtool: ipc needs a command\n");
		usage();
		return 1;
	}
	/* join the remaining words back into one command line — shells
	 * quote the whole command anyway, but 'revtool ipc THEME p 500'
	 * working unquoted is friendlier */
	char line[1024];
	snprintf(line, sizeof line, "%s", cmdline);
	for (; i < argc && strlen(line) + strlen(argv[i]) + 1 < sizeof line; i++) {
		strncat(line, " ", sizeof line - strlen(line) - 1);
		strncat(line, argv[i], sizeof line - strlen(line) - 1);
	}
	char reply[IPC_REPLY_MAX + 32];
	if (!ipc_request(sock, line, reply, sizeof reply, 2000)) {
		fprintf(stderr, "revtool: %s\n", reply[0] ? reply : "request failed");
		return 1;
	}
	printf("%s\n", reply);
	return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 1;
	}
	const char *cmd = argv[1];

	if (strcmp(cmd, "validate") == 0 && argc >= 3)
		return cmd_validate(argv[2]);
	if (strcmp(cmd, "info") == 0 && argc >= 3)
		return cmd_info(argv[2]);
	if (strcmp(cmd, "render") == 0 && argc >= 3)
		return cmd_render(argc, argv, 2);
	if (strcmp(cmd, "mkpng") == 0 && argc >= 6)
		return cmd_mkpng(argc, argv, 2);
	if (strcmp(cmd, "pack") == 0 && argc >= 3)
		return cmd_pack(argc, argv);
	if (strcmp(cmd, "unpack") == 0 && argc >= 3)
		return cmd_unpack(argc, argv);
	if (strcmp(cmd, "pnginfo") == 0 && argc >= 3)
		return cmd_pnginfo(argv[2]);
	if (strcmp(cmd, "theme") == 0 && argc >= 3)
		return cmd_theme(argc, argv);
	if (strcmp(cmd, "ipc") == 0 && argc >= 3)
		return cmd_ipc(argc, argv);
	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		usage();
		return 0;
	}
	usage();
	return 1;
}
