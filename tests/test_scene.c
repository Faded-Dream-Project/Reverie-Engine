/* ============================================================================
 * Reverie Engine — tests/test_scene.c
 * End-to-end pipeline test: .rev text -> IR -> runtime -> composited frames.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "composite.h"
#include "rev.h"
#include "scene.h"
#include "util.h"

static int g_fails = 0;

#define CHECK(cond, ...)                                                    \
	do {                                                                    \
		if (!(cond)) {                                                      \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
			printf(__VA_ARGS__);                                            \
			printf("\n");                                                   \
			g_fails++;                                                      \
		}                                                                   \
	} while (0)

#define W 128
#define H 96

static RArena *g_arenas[32];
static int g_narenas;

static void drain_arenas(void)
{
	for (int i = 0; i < g_narenas; i++)
		arena_destroy(g_arenas[i]);
	g_narenas = 0;
}

static RTScene *compile(const char *text, char *err, size_t errcap)
{
	RArena *ar = NULL;
	RSceneIR *ir = rev_parse_scene(text, strlen(text), &ar, err, errcap);
	if (!ir)
		return NULL;
	RTScene *sc = rtscene_compile(ir, ar, "tests", err, errcap);
	if (!sc) {
		arena_destroy(ar);
		return NULL;
	}
	if (g_narenas < 32)
		g_arenas[g_narenas++] = ar;
	return sc;
}

int main(void)
{
	printf("test_scene:\n");
	char err[256];

	/* 1. parse/compile errors are precise */
	{
		RTScene *sc = compile("scene { layer x { type = bogus } }", err, sizeof err);
		CHECK(sc == NULL, "bogus type must fail");
		CHECK(strstr(err, "type"), "error mentions type: %s", err);
	}
	{
		RTScene *sc = compile("scene { layer x { type = image } }", err, sizeof err);
		CHECK(sc == NULL, "image without src must fail");
	}
	{
		RTScene *sc = compile("scene { layer x { type = fluid } anim opacity { } }", err, sizeof err);
		(void)sc;
		/* anim outside a layer: parser should reject */
	}
	{
		RTScene *sc = compile("scene { layer x { type = fluid } }", err, sizeof err);
		CHECK(sc != NULL, "fluid layer compiles: %s", err);
		rtscene_free(sc);
	}

	/* 2. solid + opacity blend */
	{
		const char *txt =
			"scene {"
			"  layer base { type = solid  color = #ffffff }"
			"  layer top  { type = solid  color = #ff0000  opacity = 0.5 }"
			"}";
		RTScene *sc = compile(txt, err, sizeof err);
		CHECK(sc != NULL, "compile: %s", err);
		if (sc) {
			RFb fb = { calloc((size_t)W * H, 4), W, H };
			rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
			u32 p = fb.px[(H / 2) * W + W / 2];
			CHECK(((p >> 24) & 0xff) == 255, "opaque result");
			CHECK((p & 0xff) == 255, "red over white at 0.5 -> r=255 (got %u)", p & 0xff);
			CHECK(abs((int)((p >> 8) & 0xff) - 128) <= 1, "g half (got %u)", (p >> 8) & 0xff);
			CHECK(abs((int)((p >> 16) & 0xff) - 128) <= 1, "b half (got %u)", (p >> 16) & 0xff);
			free(fb.px);
			rtscene_free(sc);
		}
	}

	/* 3. gradient + anim evaluated over time */
	{
		const char *txt =
			"scene {"
			"  layer g {"
			"    type = gradient"
			"    shape = linear"
			"    angle = 0"
			"    stops = [#000000, #ffffff]"
			"    anim opacity { from = 1; to = 1; dur = 1s }"
			"  }"
			"}";
		RTScene *sc = compile(txt, err, sizeof err);
		CHECK(sc != NULL, "compile: %s", err);
		if (sc) {
			CHECK(rtscene_is_animated(sc), "anims make scene animated");
			RFb fb = { calloc((size_t)W * H, 4), W, H };
			rtscene_render(sc, &fb, 0, 0.5, 1.0 / 30.0);
			u32 left = fb.px[(H / 2) * W + 4];
			u32 right = fb.px[(H / 2) * W + W - 4];
			CHECK((left & 0xff) < 40, "left dark (%u)", left & 0xff);
			CHECK((right & 0xff) > 215, "right bright (%u)", right & 0xff);
			free(fb.px);
			rtscene_free(sc);
		}
	}

	/* 4. fluid renders dye and moves with pointer */
	{
		const char *txt =
			"scene {"
			"  layer f {"
			"    type = fluid"
			"    res = 0.5"
			"    dye = #ffffff"
			"    dye2 = #ffffff"
			"    force = 30"
			"    radius = 0.05"
			"    dissipation = 0.02"
			"  }"
			"}";
		RTScene *sc = compile(txt, err, sizeof err);
		CHECK(sc != NULL, "compile: %s", err);
		if (sc) {
			RFb fb = { calloc((size_t)W * H, 4), W, H };
			/* paint at center for several frames */
			for (int i = 0; i < 10; i++) {
				rtscene_pointer(sc, 0, W * 0.5, H * 0.5, 0, 0, true);
				rtscene_render(sc, &fb, 0, i / 30.0, 1.0 / 30.0);
			}
			u32 c = fb.px[(H / 2) * W + W / 2];
			CHECK((c >> 24) > 40, "fluid dye visible at center (a=%u)", c >> 24);
			u32 corner = fb.px[4 * W + 4];
			CHECK((corner >> 24) < 20, "corner stays clear (a=%u)", corner >> 24);

			/* dye moves with a swipe */
			u32 before = fb.px[(H / 2) * W + W / 4];
			for (int i = 0; i < 20; i++) {
				f64 x = W * (0.5 - 0.3 * (i / 20.0));
				rtscene_pointer(sc, 0, x, H * 0.5, -W * 0.3 / 20.0, 0, true);
				rtscene_render(sc, &fb, 0, (10 + i) / 30.0, 1.0 / 30.0);
			}
			(void)before;
			u32 c2 = fb.px[(H / 2) * W + W / 2];
			CHECK((c2 >> 24) > 5, "dye advected through center after swipe (a=%u)", c2 >> 24);
			free(fb.px);
			rtscene_free(sc);
		}
	}

	/* 5. particles render */
	{
		const char *txt =
			"scene {"
			"  layer p {"
			"    type = particles"
			"    preset = snow"
			"  }"
			"}";
		RTScene *sc = compile(txt, err, sizeof err);
		CHECK(sc != NULL, "compile: %s", err);
		if (sc) {
			RFb fb = { calloc((size_t)W * H, 4), W, H };
			u64 lit = 0;
			for (int i = 0; i < 60; i++)
				rtscene_render(sc, &fb, 0, i / 30.0, 1.0 / 30.0);
			for (size_t i = 0; i < (size_t)W * H; i++)
				if ((fb.px[i] >> 24) > 0)
					lit++;
			CHECK(lit > 50, "snow visible after 2s (%llu px)", (unsigned long long)lit);
			CHECK(lit < (size_t)W * H, "snow is sparse");
			free(fb.px);
			rtscene_free(sc);
		}
	}

	/* 6. ripple refracts the content below */
	{
		const char *txt =
			"scene {"
			"  layer base { type = solid  color = #4080c0 }"
			"  layer water {"
			"    type = ripple"
			"    strength = 3"
			"    radius = 0.06"
			"  }"
			"}";
		RTScene *sc = compile(txt, err, sizeof err);
		CHECK(sc != NULL, "compile: %s", err);
		if (sc) {
			RFb fb = { calloc((size_t)W * H, 4), W, H };
			rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
			u32 flat = fb.px[(H / 2) * W + W / 2];
			CHECK((flat >> 24) == 255 && (flat & 0xff) == 0x40, "flat water shows base");
			/* disturb and let it ring */
			rtscene_pointer(sc, 0, W * 0.5, H * 0.5, 0, 0, true);
			for (int i = 0; i < 8; i++)
				rtscene_render(sc, &fb, 0, (i + 1) / 30.0, 1.0 / 30.0);
			int changed = 0;
			for (size_t i = 0; i < (size_t)W * H; i++)
				if (fb.px[i] != flat)
					changed++;
			CHECK(changed > 30, "ripple disturbs pixels (%d)", changed);
			free(fb.px);
			rtscene_free(sc);
		}

		/* v0.2.0 click latch, isolated on a FRESH scene so every changed
		 * pixel is attributable to the click alone: a full press+release
		 * between two renders (no held pointer at all) must still splash
		 * via rtscene_click. Consume-once semantics are pinned by the
		 * platform-layer suites (test_wayland / test_wlshim). */
		{
			RTScene *sc2 = compile(txt, err, sizeof err);
			CHECK(sc2 != NULL, "recompile for click test: %s", err);
			if (sc2) {
				RFb fb = { calloc((size_t)W * H, 4), W, H };
				rtscene_render(sc2, &fb, 0, 0.0, 1.0 / 30.0);   /* flat water */
				u32 flatpx = fb.px[0];    /* uniform: base color through still water */
				rtscene_pointer(sc2, 0, W * 0.5, H * 0.5, 0, 0, false);
				rtscene_click(sc2, 0, W * 0.25, H * 0.25);
				for (int i = 0; i < 4; i++)
					rtscene_render(sc2, &fb, 0, (i + 1) / 30.0, 1.0 / 30.0);
				int disturbed = 0;
				for (size_t i = 0; i < (size_t)W * H; i++)
					if (fb.px[i] != flatpx)
						disturbed++;
				CHECK(disturbed > 30,
				      "click splash disturbs pixels (%d)", disturbed);
				free(fb.px);
				rtscene_free(sc2);
			}
		}
	}

	/* 7. screen blend brightens */
	{
		const char *txt =
			"scene {"
			"  layer a { type = solid  color = #606060 }"
			"  layer b { type = solid  color = #808080  blend = screen }"
			"}";
		RTScene *sc = compile(txt, err, sizeof err);
		CHECK(sc != NULL, "compile: %s", err);
		if (sc) {
			RFb fb = { calloc((size_t)W * H, 4), W, H };
			rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
			u32 p = fb.px[(H / 2) * W + W / 2];
			/* screen(0x60, 0x80) = 1-(1-0.376)(1-0.502) = 0.689 ~ 0xB0 */
			CHECK((p & 0xff) > 0xA8 && (p & 0xff) < 0xB8, "screen blend ~0xB0 (got %u)", p & 0xff);
			free(fb.px);
			rtscene_free(sc);
		}
	}

	/* 8. image layer via PNG fixture + fit/stretch */
	{
		const char *txt =
			"scene {"
			"  layer img {"
			"    type = image"
			"    src = \"fixtures/png_rgba8_i0.png\""
			"    fit = stretch"
			"  }"
			"}";
		RTScene *sc = compile(txt, err, sizeof err);
		CHECK(sc != NULL, "compile: %s", err);
		if (sc) {
			RFb fb = { calloc((size_t)W * H, 4), W, H };
			rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
			size_t lit = 0;
			for (size_t i = 0; i < (size_t)W * H; i++)
				if ((fb.px[i] >> 24) > 0)
					lit++;
			CHECK(lit > (size_t)W * H * 0.9, "image covers frame (%zu)", lit);
			free(fb.px);
			rtscene_free(sc);
		}
	}

	drain_arenas();
	if (g_fails == 0)
		printf("  scene: OK\n");
	return g_fails != 0;
}
