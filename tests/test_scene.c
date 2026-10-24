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

static int litpx(u32 p) { return (p & 0x00ffffff) != 0; }

static RArena *g_arenas[64];
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
        if (g_narenas < 64)
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

        /* 3b. gradient ordered dither: quantized gradients used to leave
         * staircase banding — contiguous same-code BLOCKS (measured 12x12 on
         * this ramp before the fix). The 4x4 Bayer offset must collapse the
         * flat blocks (banding contours) without biasing the mean, and must
         * leave EXACT flat regions untouched (|D| < 0.5 rounds back). The
         * 2D-block metric is the perceptual one: adjacent rows carry different
         * Bayer phases, so contours break across rows even where a single
         * scanline still shows short runs. */
        {
                const char *txt =
                        "scene {"
                        "  layer g {"
                        "    type = gradient"
                        "    shape = linear"
                        "    angle = 0"
                        "    stops = [#050505, #101010]"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile: %s", err);
                if (sc) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        rtscene_render(sc, &fb, 0, 0, 1.0 / 30.0);
                        int max_blk = 1;
                        for (u32 y = 0; y < H; y++) {
                                for (u32 x = 0; x < W; x++) {
                                        int k = 1;
                                        while (x + (u32)k < W && y + (u32)k < H) {
                                                bool eq = true;
                                                for (u32 j = 0; j <= (u32)k && eq; j++)
                                                        for (u32 i = 0; i <= (u32)k; i++)
                                                                if ((fb.px[(y + j) * W + (x + i)] & 0xff) !=
                                                                    (fb.px[y * W + x] & 0xff)) {
                                                                        eq = false;
                                                                        break;
                                                                }
                                                if (!eq)
                                                        break;
                                                k++;
                                        }
                                        if (k > max_blk)
                                                max_blk = k;
                                }
                        }
                        double sum = 0;
                        for (u32 x = 0; x < W; x++)
                                sum += (double)(fb.px[(H / 2) * W + x] & 0xff);
                        double mean = sum / W;
                        CHECK(max_blk <= 3, "dither collapses banding blocks (max block %d)",
                              max_blk);
                        CHECK(fabs(mean - 10.5) < 1.0,
                              "dither does not bias the ramp (mean %.2f)", mean);
                        free(fb.px);
                        rtscene_free(sc);
                }
        }

        /* 3c. equal-color stops: the flat half must stay EXACT — the dither
         * offset is smaller than the rounding step (|D| < 0.5), so an exactly
         * integral color can never cross a code boundary */
        {
                const char *txt =
                        "scene {"
                        "  layer g {"
                        "    type = gradient"
                        "    shape = linear"
                        "    angle = 0"
                        "    stops = [#0a0a0a, (#0a0a0a 0.5), #3a3a3a]"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile: %s", err);
                if (sc) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        rtscene_render(sc, &fb, 0, 0, 1.0 / 30.0);
                        const u32 *row = fb.px + (H / 2) * W;
                        bool flat = true;
                        for (u32 x = 0; x < W / 2 - 2; x++)
                                if ((row[x] & 0xff) != 0x0a)
                                        flat = false;
                        CHECK(flat, "flat stop span stays exact under dither");
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

        /* 4b. fluid GPU emission: dye-resolution texture stretched over the
         *     frame with linear filtering â not a full-res CPU upscale */
        {
                const char *txt =
                        "scene {"
                        "  layer f { type = fluid  res = 0.25 }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile: %s", err);
                if (sc) {
                        RTFrameList fl;
                        memset(&fl, 0, sizeof fl);
                        fl.w = 400;
                        fl.h = 300;
                        rtscene_render_layers(sc, &fl, 0, 0.1, 1.0 / 30.0);
                        CHECK(fl.n == 1, "one layer emitted (n=%d)", fl.n);
                        if (fl.n == 1) {
                                RTLayerCmd *c = &fl.cmds[0];
                                CHECK(c->w == 200 && c->h == 150,
                                      "fluid texture at dye res 200x150 (got %ux%u)",
                                      c->w, c->h);
                                CHECK(c->place_w == 400 && c->place_h == 300,
                                      "placement covers the frame (got %ux%u)",
                                      c->place_w, c->place_h);
                                CHECK(c->linear, "linear filtering requested");
                                CHECK(c->dx == 0 && c->dy == 0,
                                      "stretched from the origin (%d,%d)", c->dx, c->dy);
                                CHECK(c->px != NULL, "pixels present");
                        }
                        rtscene_free(sc);
                }
        }


                /* 4b-2. GPU sim offload: sim_offload=true emits a descriptor (no
                 *     CPU solver runs, no pixels, dye grid mirrors fluid_grid_size)
                 *     with stretch placement over the frame. */
                {
                        const char *txt =
                                "scene {"
                                "  layer f { type = fluid  res = 0.25 dye = #ff0000 dye2 = #00ff00 }"
                                "}";
                        RTScene *sc = compile(txt, err, sizeof err);
                        CHECK(sc != NULL, "compile: %s", err);
                        if (sc) {
                                RTFrameList fl;
                                memset(&fl, 0, sizeof fl);
                                fl.w = 400;
                                fl.h = 300;
                                fl.sim_offload = true;
                                rtscene_render_layers(sc, &fl, 0, 0.1, 1.0 / 30.0);
                                CHECK(fl.n == 1, "one layer emitted (n=%d)", fl.n);
                                if (fl.n == 1) {
                                        RTLayerCmd *c = &fl.cmds[0];
                                        CHECK(c->sim != NULL, "sim descriptor present");
                                        CHECK(c->px == NULL, "no CPU pixels emitted");
                                        CHECK(c->w == 200 && c->h == 150,
                                              "dye-grid size 200x150 (got %ux%u)", c->w, c->h);
                                        CHECK(c->place_w == 400 && c->place_h == 300,
                                              "stretch placement (got %ux%u)", c->place_w, c->place_h);
                                        CHECK(c->linear, "linear filtering requested");
                                        if (c->sim) {
                                                CHECK(c->sim->dye_w == 200 && c->sim->dye_h == 150,
                                                      "descriptor dye grid (got %ux%u)",
                                                      c->sim->dye_w, c->sim->dye_h);
                                                CHECK(c->sim->frame_w == 400 && c->sim->frame_h == 300,
                                                      "descriptor frame size");
                                                CHECK(c->sim->dt > 0 && c->sim->dt <= 0.1,
                                                      "descriptor dt sane (%.3f)", c->sim->dt);
                                                CHECK(c->sim->dye_a.r == 255 && c->sim->dye_b.g == 255,
                                                      "descriptor dye palette");
                                        }
                                }
                                rtscene_free(sc);
                        }
                }

        /* 4b-3. GPU ripple offload: sim_offload=true emits an RSimRipple
         *     descriptor (grid mirrors ripple_new: half res, clamped 32..512),
         *     no CPU wave equation runs, no pixels; clicks consume-on-read. */
        {
                const char *txt =
                        "scene {"
                        "  layer w { type = ripple  strength = 1.6 }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile: %s", err);
                if (sc) {
                        CHECK(rtscene_gpu_capable(sc, true),
                              "ripple scene is GPU-compositable with sim offload");
                        CHECK(!rtscene_gpu_capable(sc, false),
                              "ripple scene needs CPU compositing without sim offload");
                        RTFrameList fl;
                        memset(&fl, 0, sizeof fl);
                        fl.w = 1000;
                        fl.h = 400;
                        fl.sim_offload = true;
                        rtscene_pointer(sc, 0, 300, 100, 2, 1, true);
                        rtscene_click(sc, 0, 500, 200);
                        rtscene_render_layers(sc, &fl, 0, 0.5, 1.0 / 60.0);
                        CHECK(fl.n == 1, "one layer emitted (n=%d)", fl.n);
                        if (fl.n == 1) {
                                RTLayerCmd *c = &fl.cmds[0];
                                CHECK(c->rsim != NULL, "ripple descriptor present");
                                CHECK(c->px == NULL && c->sim == NULL,
                                      "no pixels, no fluid sim");
                                CHECK(c->w == 500 && c->h == 200,
                                      "heightfield at half res 500x200 (got %ux%u)",
                                      c->w, c->h);
                                CHECK(c->place_w == 1000 && c->place_h == 400,
                                      "stretched over the frame");
                                if (c->rsim) {
                                        CHECK(c->rsim->gw == 500 && c->rsim->gh == 200,
                                              "grid mirrors ripple_new");
                                        CHECK(c->rsim->pressed, "pointer state relayed");
                                        CHECK(c->rsim->nclicks == 1,
                                              "click queued (got %u)", c->rsim->nclicks);
                                        CHECK(c->rsim->strength > 1.5 && c->rsim->strength < 1.7,
                                              "strength passthrough");
                                }
                        }
                        /* clicks are consume-on-read */
                        RTFrameList fl2;
                        memset(&fl2, 0, sizeof fl2);
                        fl2.w = 1000;
                        fl2.h = 400;
                        fl2.sim_offload = true;
                        rtscene_render_layers(sc, &fl2, 0, 0.6, 1.0 / 60.0);
                        CHECK(fl2.n == 1 && fl2.cmds[0].rsim && fl2.cmds[0].rsim->nclicks == 0,
                              "clicks consumed by the first render");
                        rtscene_free(sc);
                }
        }

        /* 4b-4. GPU particle offload: sim_offload=true packs live particles
         *     into float bands (no full-res CPU raster, no pixels emitted);
         *     rain keeps the CPU raster (streaks are not point sprites). */
        {
                const char *txt =
                        "scene {"
                        "  layer s { type = particles  preset = snow }"
                        "  layer r { type = particles  preset = rain }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile: %s", err);
                if (sc) {
                        RTFrameList fl;
                        memset(&fl, 0, sizeof fl);
                        fl.w = 400;
                        fl.h = 300;
                        fl.sim_offload = true;
                        fl.point_offload = true;
                        rtscene_render_layers(sc, &fl, 0, 0.1, 2.0);
                        CHECK(fl.n == 2, "two layers emitted (n=%d)", fl.n);
                        if (fl.n == 2) {
                                RTLayerCmd *c = &fl.cmds[0];
                                CHECK(c->psim != NULL, "snow: particle descriptor present");
                                CHECK(c->px == NULL, "snow: no CPU pixels");
                                if (c->psim) {
                                        CHECK(c->psim->frame_w == 400 && c->psim->frame_h == 300,
                                              "frame size relayed");
                                        CHECK(c->psim->slots == 4096, "band capacity");
                                        CHECK(c->psim->band0 != NULL && c->psim->band1 != NULL,
                                              "bands packed");
                                        CHECK(c->psim->n > 0 && c->psim->n <= 4096,
                                              "live particles packed (n=%u)", c->psim->n);
                                }
                                RTLayerCmd *r = &fl.cmds[1];
                                CHECK(r->psim == NULL, "rain: no descriptor (streaks) ");
                                CHECK(r->px != NULL, "rain: CPU raster fallback emits pixels");
                        }
                        rtscene_free(sc);
                }
        }

        /* 4c. animated gradient rebuilds WRITE cleanly â the old path
         *     re-rasterized into the same buffer every frame and blended
         *     over the previous frame's raster, so once a region had been
         *     opaque it could never become transparent again (the sweep
         *     left a permanent smear). angle 0 -> 90 -> 0 over 2s. */
        {
                const char *txt =
                        "scene {"
                        "  layer g {"
                        "    type = gradient"
                        "    shape = linear"
                        "    angle = 0"
                        "    stops = [#00000000, (#ffffffff 0.35), #00000000]"
                        "    anim angle { from = 0; to = 90; dur = 1s; mode = pingpong }"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile: %s", err);
                if (sc) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        /* the daemon clears the frame before every composite
                         * (main.c memsets fb) â mirror that here so this test
                         * measures the LAYER RASTER, not accumulation in fb */
                        memset(fb.px, 0, (size_t)W * H * 4);
                        rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
                        u32 left = fb.px[(H / 2) * W + 1];
                        CHECK((left >> 24) < 16, "left transparent at angle 0 (a=%u)",
                              left >> 24);
                        memset(fb.px, 0, (size_t)W * H * 4);
                        rtscene_render(sc, &fb, 0, 1.0, 1.0 / 30.0);
                        u32 mid = fb.px[(H / 2) * W + 1];
                        CHECK((mid >> 24) > 150, "left near-opaque at angle 90 (a=%u)",
                              mid >> 24);
                        /* t=2s: angle back at 0 â the left edge must go
                         * transparent AGAIN. The old rebuild blended the new gradient
                         * over the previous frame's RASTER, so the raster (and thus
                         * the layer) never recovered. */
                        memset(fb.px, 0, (size_t)W * H * 4);
                        rtscene_render(sc, &fb, 0, 2.0, 1.0 / 30.0);
                        u32 back = fb.px[(H / 2) * W + 1];
                        CHECK((back >> 24) < 16, "left returns to transparent (a=%u)",
                              back >> 24);
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

        /* 5b. the particle authoring vocabulary: a fully custom emitter
         *     compiles, renders, and packs (the shape id rides band1.w);
         *     fade_in gates visibility; the validation surface rejects
         *     garbage with the right words. */
        {
                const char *txt =
                        "scene {"
                        "  layer p {"
                        "    type = particles"
                        "    emit_from = bottom"
                        "    direction = -90"
                        "    spread = 25"
                        "    speed = (40, 120)"
                        "    lifetime = (2, 4)"
                        "    rate = 300"
                        "    size = (1.5, 3.5)"
                        "    size_out = 0.25"
                        "    fade_in = 0.4"
                        "    fade_out = 0.9"
                        "    shape = glow"
                        "    colors = [#35e0ff, (#8f6bff 0.5), #ff6bd6]"
                        "    attract = 40"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile: %s", err);
                if (sc) {
                        RTFrameList fl;
                        memset(&fl, 0, sizeof fl);
                        fl.w = W;
                        fl.h = H;
                        fl.sim_offload = true;
                        fl.point_offload = true;
                        rtscene_render_layers(sc, &fl, 0, 0.5, 0.5);
                        CHECK(fl.n == 1, "one layer emitted (n=%d)", fl.n);
                        if (fl.n == 1) {
                                CHECK(fl.cmds[0].psim != NULL, "particle descriptor present");
                                if (fl.cmds[0].psim && fl.cmds[0].psim->n > 0) {
                                        /* shape id 2 (glow) rides band1.w on
                                         * every packed particle */
                                        u32 shaped = 0;
                                        const RSimParticles *pd = fl.cmds[0].psim;
                                        for (u32 i = 0; i < pd->n; i++)
                                                if (pd->band1[i * 4 + 3] == 2.0f)
                                                        shaped++;
                                        CHECK(shaped == pd->n,
                                              "shape rides band1.w (n=%u shaped=%u)",
                                              pd->n, shaped);
                                }
                        }
                        rtscene_free(sc);
                }

                /* fade_in gate: a 60s ramp keeps everything under the
                 * cov <= 0.02 visibility floor for the first second */
                {
                        const char *t2 =
                                "scene {"
                                "  layer p {"
                                "    type = particles"
                                "    rate = 400"
                                "    emit_from = everywhere"
                                "    fade_in = 60"
                                "    lifetime = (10, 10)"
                                "  }"
                                "}";
                        RTScene *sc2 = compile(t2, err, sizeof err);
                        CHECK(sc2 != NULL, "compile fade gate: %s", err);
                        if (sc2) {
                                RFb fb = { calloc((size_t)W * H, 4), W, H };
                                for (int i = 0; i < 30; i++)
                                        rtscene_render(sc2, &fb, 0, i / 30.0, 1.0 / 30.0);
                                u64 lit = 0;
                                for (size_t i = 0; i < (size_t)W * H; i++)
                                        if ((fb.px[i] >> 24) > 0)
                                                lit++;
                                CHECK(lit == 0, "fade_in gates visibility (%llu px)",
                                      (unsigned long long)lit);
                                free(fb.px);
                                rtscene_free(sc2);
                        }
                }

                /* validation surface */
                CHECK(compile("scene { layer p { type = particles  shape = blob } }",
                              err, sizeof err) == NULL && strstr(err, "shape must be"),
                      "bad shape rejected: %s", err);
                CHECK(compile("scene { layer p { type = particles  emit_from = middle } }",
                              err, sizeof err) == NULL && strstr(err, "emit_from must be"),
                      "bad emit_from rejected: %s", err);
                CHECK(compile("scene { layer p { type = particles  lifetime = (1) } }",
                              err, sizeof err) == NULL && strstr(err, "lifetime"),
                      "bad lifetime rejected: %s", err);
                CHECK(compile("scene { layer p { type = particles  emit_area = (0.1, 0.2) } }",
                              err, sizeof err) == NULL && strstr(err, "emit_area"),
                      "bad emit_area rejected: %s", err);
                CHECK(compile("scene { layer p { type = particles  colors = [#fff] } }",
                              err, sizeof err) == NULL && strstr(err, "colors"),
                      "short ramp rejected: %s", err);
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

                /* dev: click latch, isolated on a FRESH scene so every changed
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

                        /* ripple-lake regression (2026-10): the GPU ripple solver computed its
                         * Laplacian from UN-injected neighbors while subtracting 4x the
                         * INJECTED center height, cancelling the wave launch (nh ~= 1.04h
                         * instead of 2h) — clicks produced a weak static smear instead of an
                         * expanding ring. These checks pin the CPU solver's click PLACEMENT
                         * (the GPU path is verified against this by scripts/e2e.sh's click
                         * tests): center, all four corners, multiple simultaneous clicks,
                         * and repeated interactions without cumulative drift. */
                        {
                                /* a NON-uniform background (linear gradient) — on a flat color a
                                 * click is only visible through the upper-left specular glint,
                                 * whose centroid is intentionally biased toward the light; with
                                 * texture the centered refraction dominates, like the real theme */
                                const char *ctxt =
                                        "scene {"
                                        "  layer lake { type = gradient  shape = linear  angle = 0"
                                        "                stops = [#101820, #f0e0c0] }"
                                        "  layer water { type = ripple  strength = 3"
                                        "                 radius = 0.06 }"
                                        "}";
                                struct ClickProbe {
                                        double x, y;      /* click position, normalized */
                                        const char *name;
                                } probes[] = {
                                        { 0.50, 0.50, "center" },
                                        { 0.15, 0.15, "top-left corner" },
                                        { 0.85, 0.15, "top-right corner" },
                                        { 0.15, 0.85, "bottom-left corner" },
                                        { 0.85, 0.85, "bottom-right corner" },
                                };
                                for (unsigned pi = 0; pi < sizeof probes / sizeof probes[0]; pi++) {
                                        RTScene *scp = compile(ctxt, err, sizeof err);
                                        CHECK(scp != NULL, "recompile %s: %s", probes[pi].name, err);
                                        if (!scp)
                                                continue;
                                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                                        rtscene_render(scp, &fb, 0, 0.0, 1.0 / 30.0);   /* flat water */
                                        u32 *base = malloc((size_t)W * H * 4);
                                        memcpy(base, fb.px, (size_t)W * H * 4);
                                        /* clicks are consumed only when pointer state exists (render_core
                                         * gates on ptr[mon].seen — main.c provides it every frame) */
                                        rtscene_pointer(scp, 0, W * 0.5, H * 0.5, 0, 0, false);
                                        rtscene_click(scp, 0, probes[pi].x * W, probes[pi].y * H);
                                        for (int i = 0; i < 6; i++)
                                                rtscene_render(scp, &fb, 0, (i + 1) / 30.0, 1.0 / 30.0);
                                        long n = 0;
                                        double sx = 0, sy = 0;
                                        for (u32 y = 0; y < H; y++)
                                                for (u32 x = 0; x < W; x++) {
                                                        size_t i = (size_t)y * W + x;
                                                        if (fb.px[i] != base[i]) {
                                                                n++;
                                                                sx += x;
                                                                sy += y;
                                                        }
                                                }
                                        double cx = n ? sx / n : -1, cy = n ? sy / n : -1;
                                        CHECK(n > 60, "%s click changes >=60 px (got %ld)",
                                              probes[pi].name, n);
                                        CHECK(fabs(cx - probes[pi].x * W) <= W * 0.15 &&
                                              fabs(cy - probes[pi].y * H) <= H * 0.15,
                                              "%s click centroid on target (got %.1f,%.1f want %.1f,%.1f)",
                                              probes[pi].name, cx, cy,
                                              probes[pi].x * W, probes[pi].y * H);
                                        free(base);
                                        free(fb.px);
                                        rtscene_free(scp);
                                }

                                /* multiple clicks in one frame: two splashes, each side of the
                                 * frame keeps its own energy */
                                {
                                        RTScene *scm = compile(ctxt, err, sizeof err);
                                        CHECK(scm != NULL, "recompile multi-click: %s", err);
                                        if (scm) {
                                                RFb fb = { calloc((size_t)W * H, 4), W, H };
                                                rtscene_render(scm, &fb, 0, 0.0, 1.0 / 30.0);
                                                u32 *base = malloc((size_t)W * H * 4);
                                                memcpy(base, fb.px, (size_t)W * H * 4);
                                                rtscene_pointer(scm, 0, W * 0.5, H * 0.5, 0, 0, false);
                                                rtscene_click(scm, 0, W * 0.25, H * 0.5);
                                                rtscene_click(scm, 0, W * 0.75, H * 0.5);
                                                for (int i = 0; i < 6; i++)
                                                        rtscene_render(scm, &fb, 0, (i + 1) / 30.0, 1.0 / 30.0);
                                                long left = 0, right = 0;
                                                for (size_t i = 0; i < (size_t)W * H; i++)
                                                        if (fb.px[i] != base[i]) {
                                                                if (i % W < W / 2)
                                                                        left++;
                                                                else
                                                                        right++;
                                                        }
                                                CHECK(left > 40 && right > 40,
                                                      "two clicks splash on both sides (L=%ld R=%ld)",
                                                      left, right);
                                                free(base);
                                                free(fb.px);
                                                rtscene_free(scm);
                                        }
                                }

                                /* repeated interactions must not drift: render the SAME click
                                 * twice on fresh scenes and require the two disturbance
                                 * centroids to agree closely */
                                {
                                        double cx[2], cy[2];
                                        for (int rep = 0; rep < 2; rep++) {
                                                RTScene *scd = compile(ctxt, err, sizeof err);
                                                CHECK(scd != NULL, "recompile drift %d: %s", rep, err);
                                                cx[rep] = cy[rep] = -1;
                                                if (scd) {
                                                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                                                        rtscene_render(scd, &fb, 0, 0.0, 1.0 / 30.0);
                                                        u32 *base = malloc((size_t)W * H * 4);
                                                        memcpy(base, fb.px, (size_t)W * H * 4);
                                                        rtscene_pointer(scd, 0, W * 0.5, H * 0.5, 0, 0, false);
                                                        rtscene_click(scd, 0, W * 0.6, H * 0.4);
                                                        for (int i = 0; i < 6; i++)
                                                                rtscene_render(scd, &fb, 0, (i + 1) / 30.0, 1.0 / 30.0);
                                                        long n = 0;
                                                        double sx = 0, sy = 0;
                                                        for (u32 y = 0; y < H; y++)
                                                                for (u32 x = 0; x < W; x++) {
                                                                        size_t i = (size_t)y * W + x;
                                                                        if (fb.px[i] != base[i]) {
                                                                                n++;
                                                                                sx += x;
                                                                                sy += y;
                                                                        }
                                                                }
                                                        if (n) {
                                                                cx[rep] = sx / n;
                                                                cy[rep] = sy / n;
                                                        }
                                                        free(base);
                                                        free(fb.px);
                                                        rtscene_free(scd);
                                                }
                                        }
                                        CHECK(fabs(cx[0] - cx[1]) <= 3.0 && fabs(cy[0] - cy[1]) <= 3.0,
                                              "repeated clicks land identically (d=%.1f,%.1f)",
                                              fabs(cx[0] - cx[1]), fabs(cy[0] - cy[1]));
                                }
                                /* ripple-lake regression #2 (2026-10, the "expanding glitch
                                 * + dead clicks" field report): the wave update was
                                 * ANTI-damped - the recurrence's smooth-mode pole sat at
                                 * 1 + sqrt(damping*0.05) ~ 1.06, so every click's energy
                                 * grew ~x38/s until the field hit inf/NaN (f32 ~27 s,
                                 * RGBA16F ~3 s). A NaN field refracts garbage and later
                                 * clicks do nothing (adding -2.4 to NaN stays NaN). The
                                 * corrected solver must (a) show live waves right after
                                 * the click, (b) return to an EXACT flat pass-through
                                 * once the water calms - a NaN field never recovers -
                                 * and (c) still splash when clicked again afterwards. */
                                {
                                        const char *ctxt2 =
                                                "scene {"
                                                "  layer lake { type = gradient  shape = linear  angle = 0"
                                                "                stops = [#101820, #f0e0c0] }"
                                                "  layer water { type = ripple  strength = 3"
                                                "                 radius = 0.06 }"
                                                "}";
                                        RTScene *scq = compile(ctxt2, err, sizeof err);
                                        CHECK(scq != NULL, "recompile calm: %s", err);
                                        if (scq) {
                                                RFb fb = { calloc((size_t)W * H, 4), W, H };
                                                rtscene_render(scq, &fb, 0, 0.0, 1.0 / 30.0);
                                                u32 *base = malloc((size_t)W * H * 4);
                                                memcpy(base, fb.px, (size_t)W * H * 4);
                                                rtscene_pointer(scq, 0, W * 0.5, H * 0.5, 0, 0, false);
                                                rtscene_click(scq, 0, W * 0.5, H * 0.5);
                                                /* (a) waves alive 0.3 s after the click */
                                                for (int i = 0; i < 10; i++)
                                                        rtscene_render(scq, &fb, 0, (i + 1) / 30.0, 1.0 / 30.0);
                                                long live = 0;
                                                for (size_t i = 0; i < (size_t)W * H; i++)
                                                        if (fb.px[i] != base[i])
                                                                live++;
                                                CHECK(live > 60, "waves visible 0.3 s after click (%ld px)", live);
                                                /* (b) ...then the water calms back to the
                                                 * EXACT flat frame (measured: quiescent by
                                                 * render ~114 on this 64x48 grid; 300 gives
                                                 * 2.6x margin) */
                                                for (int i = 10; i < 300; i++)
                                                        rtscene_render(scq, &fb, 0, (i + 1) / 30.0, 1.0 / 30.0);
                                                long diff = 0;
                                                for (size_t i = 0; i < (size_t)W * H; i++)
                                                        if (fb.px[i] != base[i])
                                                                diff++;
                                                CHECK(diff == 0,
                                                              "calm lake is an exact pass-through (%ld px differ)",
                                                              diff);
                                                /* (c) a click AFTER the field fully calms
                                                 * must still splash, on target */
                                                rtscene_click(scq, 0, W * 0.4, H * 0.6);
                                                for (int i = 0; i < 6; i++)
                                                        rtscene_render(scq, &fb, 0,
                                                                       10.0 + (i + 1) / 30.0, 1.0 / 30.0);
                                                long n2 = 0;
                                                double sx = 0, sy = 0;
                                                for (u32 y = 0; y < H; y++)
                                                        for (u32 x = 0; x < W; x++) {
                                                                size_t i = (size_t)y * W + x;
                                                                if (fb.px[i] != base[i]) {
                                                                        n2++;
                                                                        sx += x;
                                                                        sy += y;
                                                                }
                                                        }
                                                CHECK(n2 > 60, "click after calm still splashes (%ld px)", n2);
                                                CHECK(n2 && fabs(sx / n2 - W * 0.4) <= W * 0.15 &&
                                                              fabs(sy / n2 - H * 0.6) <= H * 0.15,
                                                              "post-calm splash lands on target (%.1f,%.1f)",
                                                              n2 ? sx / n2 : -1, n2 ? sy / n2 : -1);
                                                free(base);
                                                free(fb.px);
                                                rtscene_free(scq);
                                        }
                                }
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

        /* 9. sprite placement: a smaller image at an explicit rect
         * (pos + anchor + scale) — the "images composited into a bigger
         * image" vocabulary. Fixture: 64x48 RGBA. */
        {
                const char *txt =
                        "scene {"
                        "  layer bg { type = solid  color = #000000 }"
                        "  layer corner {"
                        "    type = image"
                        "    src = \"fixtures/png_rgba8_i0.png\""
                        "    pos = (0.25, 0.75)"
                        "    anchor = topleft"
                        "    scale = 0.5"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile placed sprite: %s", err);
                if (sc) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
                        /* rect: 32x24 at (32, 72) */
                        size_t lit = 0;
                        for (u32 y = 72; y < 96; y++)
                                for (u32 x = 32; x < 64; x++)
                                        if (litpx(fb.px[y * W + x]))
                                                lit++;
                        CHECK(lit > (size_t)32 * 24 * 3 / 4,
                              "sprite fills its rect (%zu of 768)", lit);
                        CHECK(litpx(fb.px[80 * W + 40]), "inside rect lit");
                        CHECK(!litpx(fb.px[80 * W + 16]), "left of rect black");
                        CHECK(!litpx(fb.px[80 * W + 70]), "right of rect black");
                        CHECK(!litpx(fb.px[68 * W + 40]), "above rect black");
                        free(fb.px);
                        rtscene_free(sc);
                }
        }

        /* 10. default anchor (center): sprite centroid lands on pos */
        {
                const char *txt =
                        "scene {"
                        "  layer bg { type = solid  color = #000000 }"
                        "  layer c {"
                        "    type = image"
                        "    src = \"fixtures/png_rgba8_i0.png\""
                        "    pos = (0.5, 0.5)"
                        "    scale = 0.5"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile centered sprite: %s", err);
                if (sc) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
                        double sx = 0, sy = 0;
                        size_t n = 0;
                        for (u32 y = 0; y < H; y++)
                                for (u32 x = 0; x < W; x++)
                                        if (litpx(fb.px[y * W + x])) {
                                                sx += x;
                                                sy += y;
                                                n++;
                                        }
                        CHECK(n > 400, "sprite rendered (%zu px)", n);
                        if (n) {
                                CHECK(abs((int)(sx / n) - 64) <= 2,
                                      "centroid x ~ 64 (got %.1f)", sx / n);
                                CHECK(abs((int)(sy / n) - 48) <= 2,
                                      "centroid y ~ 48 (got %.1f)", sy / n);
                        }
                        free(fb.px);
                        rtscene_free(sc);
                }
        }

        /* 11. animated pos: the sprite drifts (anim pos_x, linear) */
        {
                const char *txt =
                        "scene {"
                        "  layer bg { type = solid  color = #000000 }"
                        "  layer d {"
                        "    type = image"
                        "    src = \"fixtures/png_rgba8_i0.png\""
                        "    scale = 0.5"
                        "    anim pos_x { from = 0.2; to = 0.8; dur = 2; ease = linear; mode = once }"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile drifting sprite: %s", err);
                if (sc) {
                        for (int k = 0; k < 2; k++) {
                                f64 t = k == 0 ? 0.5 : 1.0;   /* pos_x 0.35 / 0.5 */
                                RFb fb = { calloc((size_t)W * H, 4), W, H };
                                rtscene_render(sc, &fb, 0, t, 1.0 / 30.0);
                                double sx = 0;
                                size_t n = 0;
                                for (u32 y = 0; y < H; y++)
                                        for (u32 x = 0; x < W; x++)
                                                if (litpx(fb.px[y * W + x])) {
                                                        sx += x;
                                                        n++;
                                                }
                                int want = k == 0 ? 44 : 64;   /* 0.35*128 / 0.5*128 */
                                CHECK(n > 400, "drift render at t=%g (%zu px)", t, n);
                                if (n)
                                        CHECK(abs((int)(sx / n) - want) <= 3,
                                              "drift centroid at t=%g ~ %d (got %.1f)",
                                              t, want, sx / n);
                                free(fb.px);
                        }
                        rtscene_free(sc);
                }
        }

        /* 12. explicit size rects: stretch fills, fit letterboxes inside,
         * tile patterns — all bounded to the rect */
        {
                const char *txt =
                        "scene {"
                        "  layer bg { type = solid  color = #000000 }"
                        "  layer s {"
                        "    type = image"
                        "    src = \"fixtures/png_rgba8_i0.png\""
                        "    pos = (0.5, 0.5)"
                        "    size = (0.5, 0.25)"
                        "  }"
                        "}";
                RTScene *sc = compile(txt, err, sizeof err);
                CHECK(sc != NULL, "compile size sprite: %s", err);
                if (sc) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        rtscene_render(sc, &fb, 0, 0.0, 1.0 / 30.0);
                        /* rect 64x24 centered: x in [32,96), y in [36,60).
                         * Default fit for an explicit size is stretch -> the
                         * whole rect is covered. */
                        size_t lit = 0;
                        for (u32 y = 36; y < 60; y++)
                                for (u32 x = 32; x < 96; x++)
                                        if (litpx(fb.px[y * W + x]))
                                                lit++;
                        CHECK(lit > (size_t)64 * 24 * 9 / 10,
                              "stretch fills the size rect (%zu of 1536)", lit);
                        CHECK(!litpx(fb.px[70 * W + 64]), "below rect black");
                        free(fb.px);
                        rtscene_free(sc);
                }
                /* fit = fit letterboxes INSIDE the rect */
                const char *txt2 =
                        "scene {"
                        "  layer bg { type = solid  color = #000000 }"
                        "  layer s {"
                        "    type = image"
                        "    src = \"fixtures/png_rgba8_i0.png\""
                        "    pos = (0.5, 0.5)"
                        "    size = (0.5, 0.25)"
                        "    fit = fit"
                        "  }"
                        "}";
                RTScene *sc2 = compile(txt2, err, sizeof err);
                CHECK(sc2 != NULL, "compile fit-in-rect sprite: %s", err);
                if (sc2) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        rtscene_render(sc2, &fb, 0, 0.0, 1.0 / 30.0);
                        /* 64x48 source contained in 64x24 rect -> 32x24 at
                         * the rect center: columns [48,80) lit, [32,48) not */
                        u32 lit_left = 0, lit_mid = 0;
                        for (u32 y = 36; y < 60; y++) {
                                for (u32 x = 33; x < 46; x++)
                                        if (litpx(fb.px[y * W + x]))
                                                lit_left++;
                                for (u32 x = 50; x < 78; x++)
                                        if (litpx(fb.px[y * W + x]))
                                                lit_mid++;
                        }
                        CHECK(lit_left == 0, "letterbox bar inside rect is empty (%u)", lit_left);
                        CHECK(lit_mid > (size_t)24 * 24 * 3 / 4,
                              "contained sprite in rect center (%u)", lit_mid);
                        free(fb.px);
                        rtscene_free(sc2);
                }
                /* tile: native tiles bounded to the rect — MULTI-row grid
                 * (the first cut read the source at +y*iw and ran off the
                 * image buffer on the second tile row; ASan caught it) */
                const char *txt3 =
                        "scene {"
                        "  layer bg { type = solid  color = #000000 }"
                        "  layer s {"
                        "    type = image"
                        "    src = \"fixtures/png_rgba8_i0.png\""
                        "    pos = (0.5, 0.5)"
                        "    size = (0.75, 0.75)"
                        "    fit = tile"
                        "  }"
                        "}";
                RTScene *sc3 = compile(txt3, err, sizeof err);
                CHECK(sc3 != NULL, "compile tile sprite: %s", err);
                if (sc3) {
                        RFb fb = { calloc((size_t)W * H, 4), W, H };
                        rtscene_render(sc3, &fb, 0, 0.0, 1.0 / 30.0);
                        /* rect 96x72 centered: x in [16,112), y in [12,84);
                         * native 64x48 tiles -> two tile rows, whole rect
                         * patterned */
                        size_t lit = 0;
                        for (u32 y = 12; y < 84; y++)
                                for (u32 x = 16; x < 112; x++)
                                        if (litpx(fb.px[y * W + x]))
                                                lit++;
                        CHECK(lit > (size_t)96 * 72 * 9 / 10,
                              "tile fills the size rect (%zu of 6912)", lit);
                        free(fb.px);
                        rtscene_free(sc3);
                }
        }

        /* 13. placement validation: size+scale, bad anchor, bad pos shape */
        {
                RTScene *sc = compile("scene { layer x { type = image "
                                      "src = \"fixtures/png_rgba8_i0.png\" "
                                      "size = (0.5, 0.5)  scale = 2 } }",
                                      err, sizeof err);
                CHECK(sc == NULL, "size + scale rejected");
                CHECK(strstr(err, "mutually exclusive"), "error names the conflict: %s", err);
        }
        {
                RTScene *sc = compile("scene { layer x { type = image "
                                      "src = \"fixtures/png_rgba8_i0.png\" "
                                      "pos = (0.5, 0.5)  anchor = diagonal } }",
                                      err, sizeof err);
                CHECK(sc == NULL, "unknown anchor rejected");
                CHECK(strstr(err, "anchor"), "error names anchor: %s", err);
        }
        {
                RTScene *sc = compile("scene { layer x { type = image "
                                      "src = \"fixtures/png_rgba8_i0.png\" "
                                      "pos = 7 } }", err, sizeof err);
                CHECK(sc == NULL, "non-tuple pos rejected");
                CHECK(strstr(err, "pos must be"), "error names pos: %s", err);
        }
        {
                RTScene *sc = compile("scene { layer x { type = image "
                                      "src = \"fixtures/png_rgba8_i0.png\" "
                                      "pos = (0.5, 0.5)  anim pos { from = 0; to = 1; dur = 1 } } }",
                                      err, sizeof err);
                CHECK(sc == NULL, "anim target 'pos' rejected");
                CHECK(strstr(err, "pos_x"), "error suggests pos_x: %s", err);
        }
        {
                /* percent tuple + named anchor parse */
                RTScene *sc = compile("scene { layer x { type = image "
                                      "src = \"fixtures/png_rgba8_i0.png\" "
                                      "pos = (50%, 25%)  anchor = bottomleft  scale = 1 } }",
                                      err, sizeof err);
                CHECK(sc != NULL, "percent pos + named anchor compile: %s", err);
                if (sc)
                        rtscene_free(sc);
        }

        drain_arenas();
        if (g_fails == 0)
                printf("  scene: OK\n");
        return g_fails != 0;
}
