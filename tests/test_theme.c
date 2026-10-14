/* ============================================================================
 * Reverie Engine — test_theme.c
 * Theme system: JSON parser, folder themes, packed themes, security.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "png.h"
#include "rev.h"
#include "scene.h"
#include "theme.h"
#include "util.h"

static int fails = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fails++; \
		printf("FAIL: " __VA_ARGS__); \
		printf(" (%s:%d)\n", __FILE__, __LINE__); \
	} else { \
		printf("  ok: " __VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static const u8 *test_theme_asset(void *ctx, const char *path, size_t *len)
{
	return theme_asset((RTheme *)ctx, path, len);
}

static const u8 *memfind(const u8 *hay, size_t hn, const u8 *needle, size_t nn)
{
	if (nn == 0 || hn < nn)
		return NULL;
	for (size_t i = 0; i + nn <= hn; i++)
		if (memcmp(hay + i, needle, nn) == 0)
			return hay + i;
	return NULL;
}

static void wr(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(text, 1, strlen(text), f);
	fclose(f);
}

/* a tiny valid PNG (1x1 opaque) produced with the engine's own encoder */
static bool make_png(const char *path)
{
	u32 *px = (u32 *)malloc(4);
	if (!px)
		return false;
	px[0] = 0xff336699u;
	RevImage img = { 1, 1, px };
	char err[128];
	bool ok = rev_png_encode_file(path, &img, err, sizeof err);
	free(px);
	return ok;
}

int main(void)
{
	char err[512];
	char dir[256];
	snprintf(dir, sizeof dir, "/tmp/revtheme_%d", (int)getpid());
	mkdir(dir, 0755);
	char sub[512], suba[600];
	snprintf(sub, sizeof sub, "%s/demo", dir);
	snprintf(suba, sizeof suba, "%s/assets", sub);
	mkdir(sub, 0755);
	mkdir(suba, 0755);

	/* ---- 1. JSON parser ---- */
	{
		const char *j =
			"{ \"id\": \"demo\", \"n\": 3.5, \"flag\": true, \"nil\": null, "
			"\"arr\": [1, \"two\", false], "
			"\"esc\": \"a\\nb\\u0041\", \"obj\": { \"deep\": \"yes\" } }";
		RArena *ar = NULL;
		RJVal *v = rev_json_parse(j, strlen(j), &ar, err, sizeof err);
		CHECK(v != NULL, "json: parses (%s)", v ? "" : err);
		if (v) {
			CHECK(strcmp(rev_json_str(rev_json_get(v, "id"), "") , "demo") == 0,
			      "json: string member");
			CHECK(rev_json_num(rev_json_get(v, "n"), 0) == 3.5, "json: number member");
			CHECK(rev_json_bool(rev_json_get(v, "flag"), false), "json: bool member");
			CHECK(rev_json_get(v, "nil") != NULL &&
			      rev_json_get(v, "nil")->kind == RJ_NULL, "json: null member");
			CHECK(rev_json_len(rev_json_get(v, "arr")) == 3, "json: array length");
			CHECK(strcmp(rev_json_str(rev_json_get(v, "esc"), ""), "a\nbA") == 0,
			      "json: escapes (\\n + \\u0041)");
			CHECK(strcmp(rev_json_str(rev_json_get(rev_json_get(v, "obj"), "deep"), ""),
			             "yes") == 0, "json: nested object");
			arena_destroy(ar);
		}
		/* rejects garbage */
		RArena *ar2 = NULL;
		RJVal *bad = rev_json_parse("{\"a\": 1} trailing", 20, &ar2, err, sizeof err);
		CHECK(bad == NULL, "json: rejects trailing garbage");
		RArena *ar3 = NULL;
		bad = rev_json_parse("{\"a\": },}", 10, &ar3, err, sizeof err);
		CHECK(bad == NULL, "json: rejects malformed value");
	}

	/* ---- 2. theme folder ---- */
	{
		char jpath[600];
		snprintf(jpath, sizeof jpath, "%s/theme.json", sub);
		wr(jpath,
		   "{ \"id\": \"demo\", \"name\": \"Demo Theme\", "
		   "\"description\": \"unit\", \"author\": \"test\", \"version\": \"1.2.3\", "
		   "\"scene\": \"scene.rev\", \"engine_min\": \"0.0.1\", "
		   "\"quality\": [ { \"name\": \"low\", \"fps\": 30, \"resolution_scale\": 0.5 } ] }");
		char spath[600];
		snprintf(spath, sizeof spath, "%s/scene.rev", sub);
		wr(spath,
		   "scene { layer img { type = image src = \"assets/dot.png\" fit = fill } }");
		char apath[700];
		snprintf(apath, sizeof apath, "%s/dot.png", suba);
		CHECK(make_png(apath), "theme: test asset created");

		RTheme *th = theme_load(sub, err, sizeof err);
		CHECK(th != NULL, "theme folder loads (%s)", th ? "" : err);
		if (th) {
			const RThemeMeta *m = theme_meta(th);
			CHECK(strcmp(m->id, "demo") == 0, "meta: id");
			CHECK(strcmp(m->name, "Demo Theme") == 0, "meta: name");
			CHECK(strcmp(m->version, "1.2.3") == 0, "meta: version");
			CHECK(strcmp(m->scene, "scene.rev") == 0, "meta: scene");
			CHECK(m->nquality == 1 && strcmp(m->quality[0].name, "low") == 0,
			      "meta: quality preset");
			CHECK(theme_engine_ok(m, err, sizeof err), "engine gate passes");

			size_t alen = 0;
			const u8 *a = theme_asset(th, "assets/dot.png", &alen);
			CHECK(a != NULL && alen > 8, "asset read through theme (%zu bytes)", alen);
			CHECK(theme_asset(th, "../evil", NULL) == NULL, "traversal rejected");
			CHECK(theme_asset(th, "/etc/passwd", NULL) == NULL, "absolute rejected");

			/* scene compiles with the provider */
			size_t slen = 0;
			const char *text = theme_scene_text(th, &slen);
			RArena *ar = NULL;
			RSceneIR *ir = rev_parse_scene(text, slen, &ar, err, sizeof err);
			CHECK(ir != NULL, "theme scene parses");
			if (ir) {
				RTScene *sc = rtscene_compile_assets(ir, ar, theme_base_dir(th),
				                                     th, test_theme_asset,
				                                     err, sizeof err);
				CHECK(sc != NULL, "theme scene compiles via provider (%s)",
				      sc ? "" : err);
				if (sc)
					rtscene_free(sc);
				arena_destroy(ar);
			}
			theme_free(th);
		}

		/* folder without theme.json is not a theme */
		char notj[256];
		snprintf(notj, sizeof notj, "/tmp/revnottheme_%d", (int)getpid());
		mkdir(notj, 0755);
		RTheme *bad = theme_load(notj, err, sizeof err);
		CHECK(bad == NULL, "folder without theme.json rejected");
		rmdir(notj);
	}

	/* ---- 3. packed theme roundtrip ---- */
	{
		char packpath[600];
		snprintf(packpath, sizeof packpath, "%s/demo.revpack", dir);
		bool ok = theme_pack(sub, packpath, err, sizeof err);
		CHECK(ok, "theme_pack writes (%s)", ok ? "" : err);
		if (ok) {
			RTheme *th = theme_load(packpath, err, sizeof err);
			CHECK(th != NULL, "packed theme loads without extraction (%s)",
			      th ? "" : err);
			if (th) {
				const RThemeMeta *m = theme_meta(th);
				CHECK(strcmp(m->id, "demo") == 0, "packed meta: id");
				size_t alen = 0;
				const u8 *a = theme_asset(th, "assets/dot.png", &alen);
				CHECK(a != NULL && alen > 8, "packed asset served in place");
				CHECK(theme_asset(th, "nope.bin", NULL) == NULL,
				      "missing packed asset returns NULL");
				theme_free(th);
			}
			/* scene.json not required to be named scene.rev: entry follows meta */
		}

		/* v1 packs (scene + flat assets) still load */
		char v1[600];
		snprintf(v1, sizeof v1, "%s/v1.revpack", dir);
		wr("/tmp/rev_v1_scene.rev", "scene { layer s { type = solid color = #112233 } }");
		if (rev_pack_write(v1, "/tmp/rev_v1_scene.rev", err, sizeof err)) {
			/* v1 pack has no theme.json -> plain scene pack path in the daemon;
			 * verify the pack itself loads and its scene text parses */
			RevPack pk;
			CHECK(rev_pack_load(v1, &pk, err, sizeof err), "v1 pack still loads");
			CHECK(pk.scene_len > 10, "v1 scene text present");
			rev_pack_free(&pk);
		} else {
			CHECK(false, "v1 pack write failed: %s", err);
		}
	}

	/* ---- 4. security: hostile packs are rejected at load ---- */
	{
		/* craft a pack entry table with a traversal name by hand: take the
		 * good pack and rewrite one entry's name bytes to "../evil.png" */
		char packpath[600];
		snprintf(packpath, sizeof packpath, "%s/demo.revpack", dir);
		size_t plen = 0;
		u8 *pbytes = rev_read_file(packpath, &plen);
		CHECK(pbytes != NULL, "security: pack readable");
		if (pbytes) {
			u8 *mut = (u8 *)malloc(plen);
			memcpy(mut, pbytes, plen);
			/* name blob lives right after the entry table; find "assets/dot.png"
			 * and corrupt it into a traversal path (same length: "../dot.png\0" +
			 * padding to keep CRC irrelevant — name blob is not CRC'd, only data) */
			u8 *at = (u8 *)memfind(mut, plen, (const u8 *)"assets/dot.png", 14);
			CHECK(at != NULL, "security: name blob located");
			if (at) {
				memcpy(at, "../dot.png..\0", 13);
				char evil[600];
				snprintf(evil, sizeof evil, "%s/evil.revpack", dir);
				CHECK(rev_write_file(evil, mut, plen), "security: evil pack written");
				RevPack pk;
				bool loaded = rev_pack_load(evil, &pk, err, sizeof err);
				CHECK(!loaded, "security: traversal entry rejected (%s)",
				      loaded ? "ACCEPTED" : err);
				if (loaded)
					rev_pack_free(&pk);
			}
			free(mut);
			free(pbytes);
		}

		/* engine gate: a theme demanding a future engine is refused */
		char jpath[600];
		snprintf(jpath, sizeof jpath, "%s/theme.json", sub);
		wr(jpath, "{ \"id\": \"demo\", \"engine_min\": \"99.0.0\", \"scene\": \"scene.rev\" }");
		RTheme *th = theme_load(sub, err, sizeof err);
		CHECK(th != NULL, "future-engine theme loads structurally");
		if (th) {
			CHECK(!theme_engine_ok(theme_meta(th), err, sizeof err),
			      "engine gate refuses newer-engine requirement");
			theme_free(th);
		}
		/* restore the metadata for any later runs */
		wr(jpath,
		   "{ \"id\": \"demo\", \"name\": \"Demo Theme\", \"scene\": \"scene.rev\" }");
	}

	/* ---- 5. path safety ---- */
	{
		CHECK(rev_theme_path_safe("assets/lake.png"), "safe: nested relative");
		CHECK(rev_theme_path_safe("scene.rev"), "safe: flat");
		CHECK(rev_theme_path_safe("a/b/c/d.e"), "safe: deep");
		CHECK(!rev_theme_path_safe("/abs.png"), "unsafe: absolute");
		CHECK(!rev_theme_path_safe("a/../b"), "unsafe: parent");
		CHECK(!rev_theme_path_safe("a//b"), "unsafe: empty component");
		CHECK(!rev_theme_path_safe("a\\b"), "unsafe: backslash");
		CHECK(!rev_theme_path_safe(""), "unsafe: empty");
		char ctl[] = "a\tb";
		CHECK(!rev_theme_path_safe(ctl), "unsafe: control char");
	}

	/* cleanup */
	{
		char cmd[700];
		snprintf(cmd, sizeof cmd, "rm -rf '%s' /tmp/rev_v1_scene.rev", dir);
		if (system(cmd) != 0)
			fprintf(stderr, "cleanup failed\n");
	}

	if (fails) {
		printf("theme: %d failure(s)\n", fails);
		return 1;
	}
	printf("theme: OK\n");
	return 0;
}
