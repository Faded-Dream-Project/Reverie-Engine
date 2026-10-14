/* ============================================================================
 * Reverie Engine — scene.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "scene.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fluid.h"
#include "log.h"
#include "particles.h"
#include "png.h"
#include "raster.h"
#include "ripple.h"
#include "svg.h"
#include "util.h"

/* --------------------------------------------------------------------------
 * types
 * ------------------------------------------------------------------------ */

enum {
        AT_OPACITY = 0,
        AT_BLUR,
        AT_PARALLAX,
        AT_FORCE,
        AT_ANGLE,
        AT_STRENGTH,
        AT_RADIUS,
        AT_COUNT
};

typedef struct {
        int target;
        f64 from, to;
        f64 dur, delay;
        int ease, mode;      /* mode: 0 once, 1 loop, 2 pingpong */
} RTAnim;

typedef struct RTLayerInst RTLayerInst;

typedef struct RTLayer {
        char *name;
        int type;
        int monitor;          /* -1 all */

        f64 opacity, blur, parallax;
        int blend;

        char *src;            /* resolved asset path */
        int fit;
        bool smooth;

        int gshape;           /* 0 linear, 1 radial */
        f64 gangle;
        f64 gcx, gcy, gradius;
        RColor gstops[16];
        f64 goffs[16];
        int ngstops;

        RColor color;

        f64 f_res, f_force, f_radius, f_dissip, f_ambient;
        RColor f_dye, f_dye2;

        f64 r_strength, r_damping, r_radius;

        int p_preset, p_spawn;
        f64 p_rate, p_size, p_grav, p_drift;
        RColor p_c0, p_c1;

        RTAnim anims[16];
        int nanims;

        RTLayerInst *inst[RT_MAX_MONITORS];
} RTLayer;

struct RTLayerInst {
        u32 w, h;
        RFb raster;           /* static content at (w,h)            */
        RFb sim_fb;           /* persistent sim render target        */
        bool raster_ok;
        RFb blurred;
        f64 blur_amt;
        bool blurred_ok;
        RevFluid *fluid;
        RevRipple *ripple;
        RevParticles *parts;
        /* GPU fluid offload descriptor (address-stable: the GL renderer
         * keys its per-layer GPU sim state on &simdesc) */
        RSimFluid simdesc;
        /* GPU ripple offload descriptor (same identity contract) */
        RSimRipple rdesc;
        /* GPU particle offload: descriptor + packed float bands (grown on
         * demand; one malloc total per layer instance at steady state) */
        RSimParticles pdesc;
        f32 *pband;
        u32 pband_slots;
        /* GPU gradient offload descriptor (address-stable) */
        RGradDesc gdesc;
        /* raster content serial — bumped on every (re)build; renderers
         * cache textures against it and skip redundant re-uploads */
        u64 content_id;
        /* image source cache */
        bool img_loaded;
        bool img_is_svg;
        RevImage img;
        RevSvg *svg;
        f64 src_w, src_h;
};

/* Process-global raster serial: strictly monotonic, never reused — a
 * content_id identifies one raster build uniquely even across hot
 * reloads (new RTScene instances reset layer insts, not this counter). */
static u64 g_content_serial;

static inline u64 content_serial_next(void)
{
        return ++g_content_serial;
}

struct RTScene {
        char *title;
        int fps;
        bool animated;
        RTLayer *layers;
        int n;
        /* per-monitor pointer state (consume-on-read fields are drained by
         * render_core each frame) */
        struct {
                f64 x, y, dx, dy;
                bool pressed;
                bool seen;
                f64 click_x, click_y;   /* last click position (device px) */
                u32 clicks;             /* pending clicks since last render  */
        } ptr[RT_MAX_MONITORS];
        char *base_dir;
        RArena *ir_arena;     /* borrowed: IR must outlive the scene */
        void *asset_ctx;
        RTAssetFn asset_fn;
        /* stage timings accumulated by the current/last render pass (ms) */
        f64 prof_sim_ms, prof_raster_ms;
};

/* --------------------------------------------------------------------------
 * IR helpers
 * ------------------------------------------------------------------------ */

static const RVal *getp(const RProp *props, int n, const char *key)
{
        for (int i = 0; i < n; i++)
                if (strcmp(props[i].key, key) == 0)
                        return props[i].val;
        return NULL;
}

static const RProp *get_prop_meta(const RProp *props, int n, const char *key)
{
        for (int i = 0; i < n; i++)
                if (strcmp(props[i].key, key) == 0)
                        return &props[i];
        return NULL;
}

static bool ident_is(const RVal *v, const char *s)
{
        return v && v->kind == RV_IDENT && v->str && strcmp(v->str, s) == 0;
}

static bool val_num(const RVal *v, f64 *out)
{
        return v && v->kind == RV_NUM && out && (*out = v->num, true);
}

static bool val_str(const RVal *v, const char **out)
{
        return v && v->kind == RV_STR && out && (*out = v->str, true);
}

static bool val_color(const RVal *v, RColor *out)
{
        return v && v->kind == RV_COLOR && out && (*out = v->color, true);
}

static int map_ident(const RVal *v, const char *const *names, int n)
{
        if (!v || v->kind != RV_IDENT || !v->str)
                return -1;
        for (int i = 0; i < n; i++)
                if (strcmp(v->str, names[i]) == 0)
                        return i;
        return -1;
}

static bool fail(char *err, size_t cap, const char *fmt, ...)
{
        if (!cap)
                return false;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
        return false;
}

/* --------------------------------------------------------------------------
 * compile
 * ------------------------------------------------------------------------ */

RTScene *rtscene_compile(const RSceneIR *ir, RArena *ir_arena, const char *base_dir,
                         char *err, size_t errcap)
{
        return rtscene_compile_assets(ir, ir_arena, base_dir, NULL, NULL, err, errcap);
}

RTScene *rtscene_compile_assets(const RSceneIR *ir, RArena *ir_arena,
                                const char *base_dir, void *asset_ctx, RTAssetFn asset_fn,
                                char *err, size_t errcap)
{
        if (!ir)
                return NULL;

        RTScene *sc = (RTScene *)calloc(1, sizeof(RTScene));
        sc->fps = 30;
        sc->ir_arena = ir_arena;
        sc->base_dir = base_dir ? strdup(base_dir) : strdup(".");
        sc->asset_ctx = asset_ctx;
        sc->asset_fn = asset_fn;

        const RVal *v;
        if ((v = getp(ir->props, ir->nprops, "fps"))) {
                f64 f;
                if (!val_num(v, &f) || f < 1 || f > 240)
                        { fail(err, errcap, "scene: fps must be 1..240"); goto bad; }
                sc->fps = (int)f;
        }

        sc->layers = (RTLayer *)calloc((size_t)(ir->nlayers ? ir->nlayers : 1), sizeof(RTLayer));
        sc->n = ir->nlayers;

        static const char *type_names[] = { "image", "gradient", "solid", "fluid", "ripple", "particles" };
        static const char *blend_names[] = { "normal", "add", "screen", "multiply", "overlay" };
        static const char *fit_names[] = { "fill", "fit", "stretch", "tile" };
        static const char *shape_names[] = { "linear", "radial" };
        static const char *spawn_names[] = { "ambient", "pointer", "both" };
        static const char *preset_names[] = { "custom", "rain", "snow", "embers", "petals", "fireflies" };
        static const char *ease_names[] = { "linear", "sine", "quad", "cubic", "quart", "expo", "back", "elastic", "bounce" };
        static const char *mode_names[] = { "once", "loop", "pingpong" };
        static const char *anim_targets[] = { "opacity", "blur", "parallax", "force", "angle", "strength", "radius" };

        for (int li = 0; li < ir->nlayers; li++) {
                const RLayerIR *L = ir->layers[li];
                RTLayer *rt = &sc->layers[li];
                rt->name = L->name ? strdup(L->name) : strdup("(unnamed)");
                rt->monitor = -1;
                rt->opacity = 1.0;
                rt->blend = REV_BLEND_NORMAL;
                rt->fit = FIT_FILL;
                rt->smooth = true;
                rt->gshape = 0;
                rt->gangle = 90;
                rt->gcx = 0.5;
                rt->gcy = 0.5;
                rt->gradius = 0.8;
                rt->f_res = 0.3;
                rt->f_force = 8.0;
                rt->f_radius = 0.015;
                rt->f_dissip = 0.12;
                rt->f_ambient = 0.0;
                rt->f_dye = rev_rgb(255, 94, 196);
                rt->f_dye2 = rev_rgb(76, 184, 255);
                rt->r_strength = 1.0;
                rt->r_damping = 0.12;
                rt->r_radius = 0.01;
                rt->p_preset = RPPRESET_CUSTOM;
                rt->p_spawn = RPSPAWN_AMBIENT;
                rt->p_rate = 60;
                rt->p_size = 3;
                rt->p_grav = 60;
                rt->p_drift = 20;
                rt->p_c0 = rev_rgb(255, 200, 120);
                rt->p_c1 = rev_rgb(255, 90, 60);

                /* type (required) */
                const RVal *tv = getp(L->props, L->nprops, "type");
                if (!tv || tv->kind != RV_IDENT)
                        { fail(err, errcap, "layer '%s' (line %d): missing or invalid 'type'",
                                    rt->name, L->line); goto bad; }
                rt->type = map_ident(tv, type_names, 6);
                if (rt->type < 0)
                        { fail(err, errcap, "layer '%s' (line %d): unknown type '%s'",
                                    rt->name, L->line, tv->str); goto bad; }

                /* common props */
                if ((v = getp(L->props, L->nprops, "opacity"))) {
                        f64 x;
                        if (!val_num(v, &x))
                                { fail(err, errcap, "layer '%s': opacity must be a number", rt->name); goto bad; }
                        rt->opacity = rev_clampf(x, 0, 1);
                }
                if ((v = getp(L->props, L->nprops, "blur"))) {
                        f64 x;
                        if (!val_num(v, &x))
                                { fail(err, errcap, "layer '%s': blur must be a number (px)", rt->name); goto bad; }
                        rt->blur = rev_clampf(x, 0, 512);
                }
                if ((v = getp(L->props, L->nprops, "parallax"))) {
                        f64 x;
                        if (!val_num(v, &x))
                                { fail(err, errcap, "layer '%s': parallax must be a number", rt->name); goto bad; }
                        rt->parallax = rev_clampf(x, 0, 1);
                }
                if ((v = getp(L->props, L->nprops, "blend"))) {
                        int b = map_ident(v, blend_names, 5);
                        if (b < 0)
                                { fail(err, errcap, "layer '%s': unknown blend '%s' (normal|add|screen|multiply|overlay)",
                                            rt->name, v->str); goto bad; }
                        rt->blend = b;
                }
                if ((v = getp(L->props, L->nprops, "monitor"))) {
                        if (ident_is(v, "all")) {
                                rt->monitor = -1;
                        } else if (v->kind == RV_NUM) {
                                rt->monitor = (int)v->num;
                                if (rt->monitor < 0 || rt->monitor >= RT_MAX_MONITORS)
                                        { fail(err, errcap, "layer '%s': monitor index out of range", rt->name); goto bad; }
                        } else {
                                { fail(err, errcap, "layer '%s': monitor must be 'all' or an index", rt->name); goto bad; }
                        }
                }
                if ((v = getp(L->props, L->nprops, "smooth"))) {
                        if (v->kind == RV_BOOL) rt->smooth = v->b;
                }

                /* type-specific */
                if (rt->type == LT_IMAGE) {
                        const char *s;
                        if (!val_str(getp(L->props, L->nprops, "src"), &s) || !*s)
                                { fail(err, errcap, "layer '%s': image layers need src = \"file\"", rt->name); goto bad; }
                        char *path = rev_path_join(base_dir, s);
                        /* existence: on disk next to the scene, OR through
                         * the asset provider (theme folder cache / packed
                         * container) — packed themes must load without
                         * extraction */
                        bool have = rev_file_exists(path);
                        if (!have && asset_fn && s[0] && s[0] != '/') {
                                char rel[512];
                                snprintf(rel, sizeof rel, "%s", s);
                                if (asset_fn(asset_ctx, rel, NULL))
                                        have = true;
                        }
                        if (!have)
                                { fail(err, errcap, "layer '%s': asset not found: %s", rt->name, path); goto bad; }
                        rt->src = path;
                        if ((v = getp(L->props, L->nprops, "fit"))) {
                                int f = map_ident(v, fit_names, 4);
                                if (f < 0)
                                        { fail(err, errcap, "layer '%s': unknown fit '%s' (fill|fit|stretch|tile)",
                                                    rt->name, v->str); goto bad; }
                                rt->fit = f;
                        }
                } else if (rt->type == LT_GRADIENT) {
                        if ((v = getp(L->props, L->nprops, "shape"))) {
                                int s = map_ident(v, shape_names, 2);
                                if (s < 0)
                                        { fail(err, errcap, "layer '%s': shape must be linear|radial", rt->name); goto bad; }
                                rt->gshape = s;
                        }
                        if ((v = getp(L->props, L->nprops, "angle")))
                                val_num(v, &rt->gangle);
                        if ((v = getp(L->props, L->nprops, "center")) && v->kind == RV_TUPLE && v->n == 2) {
                                val_num(v->items[0], &rt->gcx);
                                val_num(v->items[1], &rt->gcy);
                        }
                        if ((v = getp(L->props, L->nprops, "radius")))
                                val_num(v, &rt->gradius);
                        const RVal *stops = getp(L->props, L->nprops, "stops");
                        if (!stops || stops->kind != RV_LIST || stops->n < 2)
                                { fail(err, errcap, "layer '%s': gradient needs stops = [color, ...] (min 2)",
                                            rt->name); goto bad; }
                        rt->ngstops = 0;
                        for (int i = 0; i < stops->n && rt->ngstops < 16; i++) {
                                const RVal *it = stops->items[i];
                                f64 off = -1;
                                RColor c;
                                if (val_color(it, &c)) {
                                        off = (f64)i / (stops->n - 1);   /* evenly spaced */
                                } else if (it->kind == RV_TUPLE && it->n == 2 &&
                                           val_color(it->items[0], &c) && val_num(it->items[1], &off)) {
                                        /* (color, offset) */
                                } else {
                                        { fail(err, errcap, "layer '%s': gradient stop %d must be a color or (color, offset)",
                                                    rt->name, i); goto bad; }
                                }
                                rt->gstops[rt->ngstops] = c;
                                rt->goffs[rt->ngstops] = rev_clampf(off, 0, 1);
                                rt->ngstops++;
                        }
                } else if (rt->type == LT_SOLID) {
                        if (!val_color(getp(L->props, L->nprops, "color"), &rt->color))
                                { fail(err, errcap, "layer '%s': solid layers need color = #rrggbb[aa]", rt->name); goto bad; }
                } else if (rt->type == LT_FLUID) {
                        if ((v = getp(L->props, L->nprops, "res")))      val_num(v, &rt->f_res);
                        if ((v = getp(L->props, L->nprops, "force")))    val_num(v, &rt->f_force);
                        if ((v = getp(L->props, L->nprops, "radius")))   val_num(v, &rt->f_radius);
                        if ((v = getp(L->props, L->nprops, "dissipation"))) val_num(v, &rt->f_dissip);
                        if ((v = getp(L->props, L->nprops, "ambient")))  val_num(v, &rt->f_ambient);
                        val_color(getp(L->props, L->nprops, "dye"), &rt->f_dye);
                        val_color(getp(L->props, L->nprops, "dye2"), &rt->f_dye2);
                        rt->f_res = rev_clampf(rt->f_res, 0.05, 1.0);
                        sc->animated = true;
                } else if (rt->type == LT_RIPPLE) {
                        if ((v = getp(L->props, L->nprops, "strength"))) val_num(v, &rt->r_strength);
                        if ((v = getp(L->props, L->nprops, "damping")))  val_num(v, &rt->r_damping);
                        if ((v = getp(L->props, L->nprops, "radius")))   val_num(v, &rt->r_radius);
                        sc->animated = true;
                } else if (rt->type == LT_PARTICLES) {
                        if ((v = getp(L->props, L->nprops, "preset"))) {
                                int p = map_ident(v, preset_names, 6);
                                if (p < 0)
                                        { fail(err, errcap, "layer '%s': unknown preset '%s'", rt->name, v->str); goto bad; }
                                rt->p_preset = p;
                        }
                        if ((v = getp(L->props, L->nprops, "rate")))   val_num(v, &rt->p_rate);
                        if ((v = getp(L->props, L->nprops, "size")))   val_num(v, &rt->p_size);
                        if ((v = getp(L->props, L->nprops, "gravity"))) val_num(v, &rt->p_grav);
                        if ((v = getp(L->props, L->nprops, "drift")))  val_num(v, &rt->p_drift);
                        if ((v = getp(L->props, L->nprops, "spawn"))) {
                                int s = map_ident(v, spawn_names, 3);
                                if (s < 0)
                                        { fail(err, errcap, "layer '%s': spawn must be ambient|pointer|both", rt->name); goto bad; }
                                rt->p_spawn = s;
                        }
                        val_color(getp(L->props, L->nprops, "color"), &rt->p_c0);
                        val_color(getp(L->props, L->nprops, "color2"), &rt->p_c1);
                        sc->animated = true;
                }

                /* anims */
                for (int ai = 0; ai < L->nanims && rt->nanims < 16; ai++) {
                        const RAnim *a = &L->anims[ai];
                        RVal tmp = { .kind = RV_IDENT, .str = a->target };
                        int target = map_ident(&tmp, anim_targets, AT_COUNT);
                        if (target < 0)
                                { fail(err, errcap, "layer '%s' (line %d): anim target '%s' not animatable "
                                            "(opacity|blur|parallax|force|angle|strength|radius)",
                                            rt->name, a->line, a->target); goto bad; }
                        RTAnim *ra = &rt->anims[rt->nanims++];
                        memset(ra, 0, sizeof *ra);
                        ra->target = target;
                        ra->dur = 3;
                        ra->ease = REV_EASE_SINE;
                        ra->mode = 1;
                        if (!val_num(getp(a->props, a->nprops, "from"), &ra->from) ||
                            !val_num(getp(a->props, a->nprops, "to"), &ra->to))
                                { fail(err, errcap, "layer '%s': anim '%s' needs numeric from/to",
                                            rt->name, a->target); goto bad; }
                        val_num(getp(a->props, a->nprops, "dur"), &ra->dur);
                        val_num(getp(a->props, a->nprops, "delay"), &ra->delay);
                        if ((v = getp(a->props, a->nprops, "ease"))) {
                                int e = map_ident(v, ease_names, 9);
                                if (e >= 0) ra->ease = e;
                        }
                        if ((v = getp(a->props, a->nprops, "mode"))) {
                                int m = map_ident(v, mode_names, 3);
                                if (m >= 0) ra->mode = m;
                        }
                        if (ra->dur <= 0)
                                { fail(err, errcap, "layer '%s': anim dur must be > 0", rt->name); goto bad; }
                }
                if (rt->nanims > 0)
                        sc->animated = true;
                (void)get_prop_meta;
        }

        return sc;

bad:
        for (int i = 0; i < sc->n; i++)
                free(sc->layers[i].name);
        free(sc->layers);
        free(sc->title);
        free(sc->base_dir);
        free(sc);
        return NULL;
}

void rtscene_free(RTScene *sc)
{
        if (!sc)
                return;
        for (int i = 0; i < sc->n; i++) {
                RTLayer *L = &sc->layers[i];
                free(L->name);
                free(L->src);
                for (int m = 0; m < RT_MAX_MONITORS; m++) {
                        RTLayerInst *in = L->inst[m];
                        if (!in)
                                continue;
                        free(in->raster.px);
                                        free(in->sim_fb.px);
                                        in->sim_fb.px = NULL;
                        free(in->pband);
                        free(in->blurred.px);
                        fluid_free(in->fluid);
                        ripple_free(in->ripple);
                        particles_free(in->parts);
                        if (in->img_is_svg)
                                rev_svg_destroy(in->svg);
                        else
                                rev_image_free(&in->img);
                        free(in);
                }
        }
        free(sc->layers);
        free(sc->title);
        free(sc->base_dir);
        free(sc);
}

void rtscene_set_asset_provider(RTScene *sc, void *ctx, RTAssetFn fn)
{
        if (!sc)
                return;
        sc->asset_ctx = ctx;
        sc->asset_fn = fn;
        /* force reload of any cached sources */
        for (int i = 0; i < sc->n; i++)
                for (int m = 0; m < RT_MAX_MONITORS; m++) {
                        RTLayerInst *in = sc->layers[i].inst[m];
                        if (in) {
                                in->img_loaded = false;
                                in->raster_ok = false;
                        }
                }
}

const char *rtscene_title(const RTScene *sc) { return sc ? (sc->title ? sc->title : "") : ""; }
int  rtscene_fps(const RTScene *sc)          { return sc ? sc->fps : 30; }
int  rtscene_layer_count(const RTScene *sc)  { return sc ? sc->n : 0; }
bool rtscene_is_animated(const RTScene *sc)  { return sc ? sc->animated : false; }

void rtscene_pointer(RTScene *sc, int mon, f64 x, f64 y, f64 dx, f64 dy, bool pressed)
{
        if (!sc || mon < 0 || mon >= RT_MAX_MONITORS)
                return;
        sc->ptr[mon].x = x;
        sc->ptr[mon].y = y;
        sc->ptr[mon].dx += dx;
        sc->ptr[mon].dy += dy;
        sc->ptr[mon].pressed = pressed;
        sc->ptr[mon].seen = true;
}

void rtscene_click(RTScene *sc, int mon, f64 x, f64 y)
{
        if (!sc || mon < 0 || mon >= RT_MAX_MONITORS)
                return;
        /* latched click edge -> pending splash/burst, consumed by the next
         * render (works even when it lands before the sims exist) */
        sc->ptr[mon].click_x = x;
        sc->ptr[mon].click_y = y;
        sc->ptr[mon].clicks++;
}

void rtscene_flush_caches(RTScene *sc)
{
        if (!sc)
                return;
        for (int i = 0; i < sc->n; i++) {
                for (int m = 0; m < RT_MAX_MONITORS; m++) {
                        RTLayerInst *in = sc->layers[i].inst[m];
                        if (in) {
                                in->raster_ok = false;
                                in->blurred_ok = false;
                        }
                }
        }
}

/* --------------------------------------------------------------------------
 * animation evaluation
 * ------------------------------------------------------------------------ */

static f64 anim_eval(const RTAnim *a, f64 t)
{
        f64 u = (t - a->delay) / a->dur;
        if (u < 0)
                u = 0;
        switch (a->mode) {
        case 0:  /* once */
                if (u >= 1) u = 1;
                break;
        case 1:  /* loop */
                u = fmod(u, 1.0);
                if (u < 0) u += 1.0;
                break;
        default: /* pingpong */
                u = fmod(u, 2.0);
                if (u < 0) u += 2.0;
                if (u > 1) u = 2.0 - u;
                break;
        }
        f64 e = rev_ease(a->ease, u);
        return a->from + (a->to - a->from) * e;
}

typedef struct {
        f64 opacity, blur, parallax, force, angle, strength, radius;
} Eff;

static void eval_props(const RTLayer *L, f64 t, Eff *e)
{
        e->opacity = L->opacity;
        e->blur = L->blur;
        e->parallax = L->parallax;
        e->force = L->f_force;
        e->angle = L->gangle;
        e->strength = L->r_strength;
        e->radius = L->f_radius;
        for (int i = 0; i < L->nanims; i++) {
                f64 v = anim_eval(&L->anims[i], t);
                switch (L->anims[i].target) {
                case AT_OPACITY:  e->opacity = v;  break;
                case AT_BLUR:     e->blur = v;     break;
                case AT_PARALLAX: e->parallax = v; break;
                case AT_FORCE:    e->force = v;    break;
                case AT_ANGLE:    e->angle = v;    break;
                case AT_STRENGTH: e->strength = v; break;
                case AT_RADIUS:   e->radius = v;   break;
                default: break;
                }
        }
        e->opacity = rev_clampf(e->opacity, 0, 1);
        e->blur = rev_clampf(e->blur, 0, 512);
        e->parallax = rev_clampf(e->parallax, 0, 1);
}

/* --------------------------------------------------------------------------
 * static raster builders
 * ------------------------------------------------------------------------ */

static void build_gradient_raster(RFb *fb, const RTLayer *L, Eff *e)
{
        RPaint paint;
        memset(&paint, 0, sizeof paint);
        RGradStop stops[16];
        for (int i = 0; i < L->ngstops; i++) {
                stops[i].off = L->goffs[i];
                stops[i].c = L->gstops[i];
        }
        paint.stops = stops;
        paint.nstops = L->ngstops;
        paint.spread = RSPREAD_PAD;

        f64 w = fb->w, h = fb->h;
        if (L->gshape == 0) {
                paint.kind = RPAINT_LINEAR;
                f64 rad = e->angle * M_PI / 180.0;
                Vec2 dir = { cos(rad), sin(rad) };
                Vec2 c = { w / 2, h / 2 };
                f64 hx = fabs(dir.x) * w / 2 + fabs(dir.y) * h / 2;
                f64 hy = hx;
                paint.lp0 = (Vec2){ c.x - dir.x * hx, c.y - dir.y * hy };
                paint.lp1 = (Vec2){ c.x + dir.x * hx, c.y + dir.y * hy };
                (void)hy;
        } else {
                paint.kind = RPAINT_RADIAL;
                paint.rc = (Vec2){ L->gcx * w, L->gcy * h };
                paint.rf = paint.rc;
                paint.rr = L->gradius * (w > h ? w : h) * 0.5;
        }
        /* full-rect direct fill: same LUT/spread/geometry as the scanline
         * rasterizer (which this replaced) minus the polygon machinery —
         * and it WRITES every pixel, so animated rebuilds no longer blend
         * over the previous frame's raster through their transparent stops */
        rev_paint_fill_rect(fb->px, fb->w, fb->h, &paint);
}

static void build_solid_raster(RFb *fb, RColor c)
{
        u32 v = rev_pack_rgba(c);
        size_t n = (size_t)fb->w * fb->h;
        for (size_t i = 0; i < n; i++)
                fb->px[i] = v;
}

static bool load_source(RTScene *sc, RTLayer *L, RTLayerInst *in, char *err, size_t errcap)
{
        if (in->img_loaded)
                return in->img_is_svg ? in->svg != NULL : in->img.px != NULL;

        in->img_loaded = true;
        bool is_svg = rev_str_ends_with_ci(L->src, ".svg");
        in->img_is_svg = is_svg;

        /* packed asset? decode from memory first */
        if (sc->asset_fn) {
                size_t mlen = 0;
                const u8 *mdata = sc->asset_fn(sc->asset_ctx, L->src, &mlen);
                if (mdata && mlen) {
                        if (is_svg) {
                                in->svg = rev_svg_parse((const char *)mdata, mlen, err, errcap);
                                if (!in->svg)
                                        return false;
                                f64 w = 0, h = 0;
                                rev_svg_intrinsic_size(in->svg, &w, &h);
                                in->src_w = w > 0 ? w : 16;
                                in->src_h = h > 0 ? h : 9;
                        } else {
                                if (!rev_png_decode(mdata, mlen, &in->img, err, errcap))
                                        return false;
                                in->src_w = in->img.w;
                                in->src_h = in->img.h;
                        }
                        return true;
                }
                /* fall through to disk */
        }

        if (is_svg) {
                char e2[256];
                in->svg = rev_svg_load(L->src, e2, sizeof e2);
                if (!in->svg) {
                        snprintf(err, errcap, "%s", e2);
                        return false;
                }
                f64 w = 0, h = 0;
                rev_svg_intrinsic_size(in->svg, &w, &h);
                in->src_w = w > 0 ? w : 16;
                in->src_h = h > 0 ? h : 9;
        } else {
                char e2[256];
                if (!rev_png_decode_file(L->src, &in->img, e2, sizeof e2)) {
                        snprintf(err, errcap, "%s", e2);
                        return false;
                }
                in->src_w = in->img.w;
                in->src_h = in->img.h;
        }
        return true;
}

static void build_image_raster(RFb *fb, RTLayer *L, RTLayerInst *in)
{
        if (in->img_is_svg) {
                memset(fb->px, 0, (size_t)fb->w * fb->h * 4);
                rev_svg_render(in->svg, fb->px, fb->w, fb->h);
                return;
        }
        /* PNG: scale to fit */
        f64 iw = in->src_w, ih = in->src_h;
        f64 dw = fb->w, dh = fb->h;
        f64 s;
        switch (L->fit) {
        case FIT_STRETCH: s = -1; break;
        case FIT_CONTAIN: s = dw / iw < dh / ih ? dw / iw : dh / ih; break;
        case FIT_TILE:    s = 1; break;
        default:          s = dw / iw > dh / ih ? dw / iw : dh / ih; break;
        }
        if (L->fit == FIT_TILE) {
                /* draw at native size, tiled from the origin */
                u32 tw = in->img.w, th = in->img.h;
                for (u32 y = 0; y < fb->h; y += th) {
                        for (u32 x = 0; x < fb->w; x += tw) {
                                u32 cw = tw < fb->w - x ? tw : fb->w - x;
                                u32 ch = th < fb->h - y ? th : fb->h - y;
                                for (u32 r = 0; r < ch; r++)
                                        memcpy(fb->px + (size_t)(y + r) * fb->w + x,
                                               in->img.px + (size_t)r * tw, (size_t)cw * 4);
                        }
                }
                return;
        }
        if (s < 0) {
                /* stretch to fb exactly */
                if (L->smooth)
                        rev_resample_bilinear(in->img.px, in->img.w, in->img.h, fb->px, fb->w, fb->h);
                else
                        rev_resample_nearest(in->img.px, in->img.w, in->img.h, fb->px, fb->w, fb->h);
                return;
        }
        u32 nw = (u32)(iw * s + 0.5), nh = (u32)(ih * s + 0.5);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        u32 *tmp = (u32 *)malloc((size_t)nw * nh * 4);
        if (!tmp)
                return;
        if (L->smooth)
                rev_resample_bilinear(in->img.px, in->img.w, in->img.h, tmp, nw, nh);
        else
                rev_resample_nearest(in->img.px, in->img.w, in->img.h, tmp, nw, nh);
        memset(fb->px, 0, (size_t)fb->w * fb->h * 4);
        /* center */
        int ox = (int)((f64)fb->w / 2 - (f64)nw / 2);
        int oy = (int)((f64)fb->h / 2 - (f64)nh / 2);
        RFb src = { tmp, nw, nh };
        rev_blit(fb, &src, ox, oy, 255);
        free(tmp);
}

/* --------------------------------------------------------------------------
 * render
 * ------------------------------------------------------------------------ */

static RTLayerInst *get_inst(RTLayer *L, int mon)
{
        if (!L->inst[mon]) {
                RTLayerInst *in = (RTLayerInst *)calloc(1, sizeof(RTLayerInst));
                L->inst[mon] = in;
        }
        return L->inst[mon];
}

/* --------------------------------------------------------------------------
 * render core: composites into `fb` (CPU mode, fl == NULL) or emits layer
 * commands into `fl` (GPU offload mode, fb == NULL). Emission mode skips
 * CPU blur (the renderer blurs on the GPU) and requires ripple-free scenes.
 * ------------------------------------------------------------------------ */

static bool ensure_sim_fb(RTLayerInst *in, u32 w, u32 h)
{
        if (in->sim_fb.px && in->sim_fb.w == w && in->sim_fb.h == h)
                return true;
        free(in->sim_fb.px);
        in->sim_fb.px = (u32 *)malloc((size_t)w * h * 4);
        in->sim_fb.w = w;
        in->sim_fb.h = h;
        return in->sim_fb.px != NULL;
}

bool rtscene_has_particles(const RTScene *sc)
{
        if (!sc)
                return false;
        for (int i = 0; i < sc->n; i++)
                if (sc->layers[i].type == LT_PARTICLES)
                        return true;
        return false;
}

bool rtscene_has_gradients(const RTScene *sc)
{
        if (!sc)
                return false;
        for (int i = 0; i < sc->n; i++)
                if (sc->layers[i].type == LT_GRADIENT)
                        return true;
        return false;
}

bool rtscene_gpu_capable(const RTScene *sc, bool sim_offload)
{
        if (!sc)
                return false;
        for (int i = 0; i < sc->n; i++) {
                if (sc->layers[i].type == LT_RIPPLE) {
                        /* ripple refraction needs the below-composite AND a
                         * GPU height field: without sim offload the whole
                         * scene must composite on the CPU (the old behavior
                         * for every ripple scene, now only the fallback). */
                        if (!sim_offload)
                                return false;
                }
        }
        return true;
}

static void render_core(RTScene *sc, RFb *fb, RTFrameList *fl, int mon, f64 t, f64 dt)
{
        if (!sc || (!fb && !fl))
                return;
        if (fl && fb)
                return;    /* mutually exclusive modes */
        if (fb && (!fb->px || fb->w == 0 || fb->h == 0))
                return;
        if (mon < 0 || mon >= RT_MAX_MONITORS)
                mon = 0;
        if (dt <= 0) dt = 1.0 / sc->fps;
        if (dt > 0.1) dt = 0.1;
        sc->prof_sim_ms = 0;
        sc->prof_raster_ms = 0;

        f64 W = fb ? fb->w : fl->w, H = fb ? fb->h : fl->h;
        /* normalized pointer for this monitor (default center) */
        f64 px = 0.5, py = 0.5, pdx = 0, pdy = 0;
        bool pressed = false;
        f64 click_x = 0.5, click_y = 0.5;
        u32 clicks = 0;
        if (sc->ptr[mon].seen) {
                px = sc->ptr[mon].x / W;
                py = sc->ptr[mon].y / H;
                pdx = sc->ptr[mon].dx / W;
                pdy = sc->ptr[mon].dy / H;
                pressed = sc->ptr[mon].pressed;
                click_x = sc->ptr[mon].click_x / W;
                click_y = sc->ptr[mon].click_y / H;
                clicks = sc->ptr[mon].clicks;
                sc->ptr[mon].clicks = 0;    /* consume */
        }

        RFb scratch = { 0 };
        u32 *below_snap = NULL;

        for (int i = 0; i < sc->n; i++) {
                RTLayer *L = &sc->layers[i];
                if (L->monitor != -1 && L->monitor != mon)
                        continue;

                Eff e;
                eval_props(L, t, &e);
                if (e.opacity <= 0.001)
                        continue;

                RTLayerInst *in = get_inst(L, mon);

                /* parallax oversize */
                f64 pf = 1.0 + e.parallax * 0.08;
                u32 rw = (u32)(W * pf + 0.5), rh = (u32)(H * pf + 0.5);

                const RFb *srcfb = NULL;
                f64 emit_blur = e.blur;    /* GPU mode: sigma passed through  */
                bool emit_linear = false;  /* sub-frame-res sim: linear filter */
                bool emit_stretch = false; /* sim texture stretches over frame */

                if (L->type == LT_RIPPLE) {
                        if (fl && fl->sim_offload) {
                                /* The renderer owns the simulation: emit a
                         * descriptor instead of stepping the CPU wave
                         * equation — the height field lives in an RGBA16F
                         * ping-pong pair on the GPU and the refraction
                         * happens in the composite shader reading the
                         * accumulation target (the below-composite).
                         * Grid sizing mirrors ripple_new() exactly. */
                                u32 gw = (u32)W / 2, gh = (u32)H / 2;
                                if (gw < 32) gw = 32;
                                if (gh < 32) gh = 32;
                                if (gw > 512) gw = 512;
                                if (gh > 512) gh = 512;
                                RSimRipple *rd = &in->rdesc;
                                rd->dt = dt;
                                rd->px = px;
                                rd->py = py;
                                rd->pressed = pressed;
                                rd->strength = e.strength;
                                rd->damping = L->r_damping;
                                rd->radius = L->r_radius;
                                rd->nclicks = clicks > 4 ? 4 : clicks;
                                for (u32 c = 0; c < rd->nclicks; c++) {
                                        rd->click_x[c] = click_x;
                                        rd->click_y[c] = click_y;
                                }
                                rd->frame_w = (u32)W;
                                rd->frame_h = (u32)H;
                                rd->gw = gw;
                                rd->gh = gh;
                                sc->ptr[mon].clicks = 0;    /* consumed */
                                if (fl->n >= RT_FRAME_MAX_LAYERS)
                                        break;
                                RTLayerCmd *cmd = &fl->cmds[fl->n++];
                                memset(cmd, 0, sizeof *cmd);
                                cmd->rsim = rd;
                                cmd->w = gw;
                                cmd->h = gh;
                                cmd->place_w = (u32)W;
                                cmd->place_h = (u32)H;
                                cmd->dx = 0;
                                cmd->dy = 0;
                                cmd->blend = L->blend;
                                cmd->opacity = e.opacity;
                                cmd->blur = 0.0;
                                cmd->linear = true;
                                continue;
                        }
                        if (fl) {
                                /* GPU layers without sim offload cannot do
                                 * refraction (no below-composite exists in
                                 * emission mode) — main.c never asks for this
                                 * combination; guard regardless. */
                                static bool warned_ripple;
                                if (!warned_ripple) {
                                        warned_ripple = true;
                                        LOGW("layer '%s': ripple requires CPU compositing",
                                             L->name);
                                }
                                continue;
                        }
                        if (!in->ripple) {
                                in->ripple = ripple_new(fb->w, fb->h);
                                if (in->ripple)
                                        ripple_configure(in->ripple, L->r_strength, L->r_damping, L->r_radius);
                        }
                        if (in->ripple) {
                                ripple_configure(in->ripple, e.strength, L->r_damping, L->r_radius);
                                ripple_pointer(in->ripple, px, py, pressed);
                                /* latched clicks: a full press+release between two
                                 * polls still makes a splash */
                                for (u32 c = 0; c < clicks; c++)
                                        ripple_click(in->ripple, click_x, click_y);
                                f64 ms = rev_monotonic();
                                ripple_step(in->ripple, dt);
                                if (!below_snap)
                                        below_snap = (u32 *)malloc((size_t)fb->w * fb->h * 4);
                                if (below_snap) {
                                        memcpy(below_snap, fb->px, (size_t)fb->w * fb->h * 4);
                                        if (!scratch.px || scratch.w != fb->w || scratch.h != fb->h) {
                                                free(scratch.px);
                                                scratch.px = (u32 *)malloc((size_t)fb->w * fb->h * 4);
                                                scratch.w = fb->w;
                                                scratch.h = fb->h;
                                        }
                                        ripple_apply(in->ripple, below_snap, scratch.px);
                                        srcfb = &scratch;
                                }
                                sc->prof_sim_ms += (rev_monotonic() - ms) * 1000.0;
                        }
                } else if (L->type == LT_FLUID) {
                        if (fl && fl->sim_offload) {
                                /* The renderer owns the simulation — emit a
                                 * descriptor instead of stepping the CPU
                                 * solver. The dye grid mirrors fluid_new's
                                 * sizing exactly (fluid_grid_size) so both
                                 * paths render the same field shape. */
                                u32 gw, gh;
                                fluid_grid_size((u32)W, (u32)H, L->f_res, &gw, &gh);
                                RSimFluid *sd = &in->simdesc;
                                sd->dt = dt;
                                sd->px = px;
                                sd->py = py;
                                sd->pdx = pdx;
                                sd->pdy = pdy;
                                sd->pressed = pressed;
                                sd->force = e.force;
                                sd->radius = e.radius;
                                sd->dissip = L->f_dissip;
                                sd->ambient = L->f_ambient;
                                sd->dye_a = L->f_dye;
                                sd->dye_b = L->f_dye2;
                                sd->frame_w = (u32)W;
                                sd->frame_h = (u32)H;
                                sd->dye_w = gw * 2;
                                sd->dye_h = gh * 2;
                                if (fl->n >= RT_FRAME_MAX_LAYERS)
                                        break;
                                RTLayerCmd *cmd = &fl->cmds[fl->n++];
                                memset(cmd, 0, sizeof *cmd);
                                cmd->sim = sd;
                                cmd->w = sd->dye_w;
                                cmd->h = sd->dye_h;
                                cmd->place_w = (u32)W;
                                cmd->place_h = (u32)H;
                                cmd->dx = 0;
                                cmd->dy = 0;
                                cmd->blend = L->blend;
                                cmd->opacity = e.opacity;
                                cmd->blur = 0.0;
                                cmd->linear = true;
                                continue;
                        }
                        if (!in->fluid) {
                                in->fluid = fluid_new((u32)W, (u32)H, L->f_res);
                                if (in->fluid)
                                        fluid_configure(in->fluid, L->f_dye, L->f_dye2, L->f_force,
                                                        L->f_radius, L->f_dissip, L->f_ambient);
                        }
                        if (in->fluid) {
                                fluid_configure(in->fluid, L->f_dye, L->f_dye2, e.force,
                                                e.radius, L->f_dissip, L->f_ambient);
                                fluid_pointer(in->fluid, px, py, pdx, pdy, pressed);
                                f64 ms = rev_monotonic();
                                fluid_step(in->fluid, dt);
                                sc->prof_sim_ms += (rev_monotonic() - ms) * 1000.0;
                                RFb *target;
                                u32 tw = (u32)W, th = (u32)H;
                                if (fl) {
                                        /* GPU offload: render the dye at the
                                         * resolution it actually exists at and
                                         * stretch it over the frame with linear
                                         * texture sampling — the old path did
                                         * the SAME bilinear upscale per output
                                         * pixel on the CPU (plus an exp()
                                         * tonemap) every frame. */
                                        u32 dw = 0, dh = 0;
                                        fluid_dye_size(in->fluid, &dw, &dh);
                                        tw = dw;
                                        th = dh;
                                        if (!ensure_sim_fb(in, dw, dh))
                                                continue;
                                        target = &in->sim_fb;
                                        emit_linear = true;
                                        emit_stretch = (dw != (u32)W || dh != (u32)H);
                                } else {
                                        if (!scratch.px || scratch.w != (u32)W || scratch.h != (u32)H) {
                                                free(scratch.px);
                                                scratch.px = (u32 *)malloc((size_t)W * H * 4);
                                                scratch.w = (u32)W;
                                                scratch.h = (u32)H;
                                        }
                                        target = &scratch;
                                }
                                if (target->px) {
                                        ms = rev_monotonic();
                                        fluid_render(in->fluid, target->px, tw, th);
                                        sc->prof_sim_ms += (rev_monotonic() - ms) * 1000.0;
                                        srcfb = target;
                                }
                        }
                } else if (L->type == LT_PARTICLES) {
                        if (!in->parts) {
                                in->parts = particles_new((u32)W, (u32)H);
                                if (in->parts)
                                        particles_configure(in->parts, L->p_preset, 0, L->p_rate, L->p_size,
                                                            L->p_grav, L->p_drift, L->p_spawn, L->p_c0, L->p_c1);
                        }
                        if (in->parts) {
                                particles_pointer(in->parts, px, py, pressed);
                                if (clicks)
                                        particles_burst(in->parts, click_x, click_y,
                                                        (int)(clicks > 4 ? 4 : clicks) * 24);
                                f64 ms = rev_monotonic();
                                particles_step(in->parts, dt);
                                sc->prof_sim_ms += (rev_monotonic() - ms) * 1000.0;
                                /* GPU point-sprite offload: the CPU keeps the
                                 * cheap per-particle state update and packs
                                 * live particles into ~128KB of floats; the
                                 * renderer draws them as point sprites (the
                                 * old path rasterized a full-frame sprite layer
                                 * on the CPU and uploaded 8MB RGBA every
                                 * frame). Rain's vertical streaks are not
                                 * point-sprite-shaped — that preset keeps the
                                 * CPU raster. */
                                if (fl && fl->point_offload && L->p_preset != RPPRESET_RAIN) {
                                        u32 live = 0;
                                        /* one-time band buffer sized to the
                                         * particle capacity (4096 slots) */
                                        u32 slots = 4096;
                                        if (!in->pband) {
                                                in->pband = (f32 *)malloc((size_t)slots * 4 * 2 * sizeof(f32));
                                                in->pband_slots = in->pband ? slots : 0;
                                        }
                                        if (in->pband) {
                                                memset(in->pband, 0, (size_t)in->pband_slots * 4 * 2 * sizeof(f32));
                                                live = particles_pack(in->parts, in->pband,
                                                                      in->pband + (size_t)in->pband_slots * 4,
                                                                      in->pband_slots);
                                                RSimParticles *pd = &in->pdesc;
                                                pd->n = live;
                                                pd->slots = in->pband_slots;
                                                pd->band0 = in->pband;
                                                pd->band1 = in->pband + (size_t)in->pband_slots * 4;
                                                pd->frame_w = (u32)W;
                                                pd->frame_h = (u32)H;
                                                if (fl->n >= RT_FRAME_MAX_LAYERS)
                                                        break;
                                                RTLayerCmd *cmd = &fl->cmds[fl->n++];
                                                memset(cmd, 0, sizeof *cmd);
                                                cmd->psim = pd;
                                                cmd->w = (u32)W;
                                                cmd->h = (u32)H;
                                                cmd->place_w = 0;
                                                cmd->place_h = 0;
                                                cmd->dx = 0;
                                                cmd->dy = 0;
                                                cmd->blend = L->blend;
                                                cmd->opacity = e.opacity;
                                                cmd->blur = 0.0;
                                                cmd->linear = false;
                                                cmd->content_id = 0;
                                                continue;
                                        }
                                }
                                RFb *target;
                                if (fl) {
                                        if (!ensure_sim_fb(in, (u32)W, (u32)H))
                                                continue;
                                        target = &in->sim_fb;
                                } else {
                                        if (!scratch.px || scratch.w != (u32)W || scratch.h != (u32)H) {
                                                free(scratch.px);
                                                scratch.px = (u32 *)malloc((size_t)W * H * 4);
                                                scratch.w = (u32)W;
                                                scratch.h = (u32)H;
                                        }
                                        target = &scratch;
                                }
                                if (target->px) {
                                        f64 ms2 = rev_monotonic();
                                        memset(target->px, 0, (size_t)W * H * 4);
                                        particles_render(in->parts, target->px, (u32)W, (u32)H);
                                        sc->prof_sim_ms += (rev_monotonic() - ms2) * 1000.0;
                                        srcfb = target;
                                }
                        }
                } else {
                        /* static content with parallax oversize */
                        if (L->type == LT_GRADIENT && fl && fl->grad_offload) {
                                /* GPU gradient evaluation: emit the descriptor
                                 * instead of rasterizing — the CPU LUT fill
                                 * (~0.2ms at 1080p) and the full-frame upload
                                 * (8MB) disappear; the renderer evaluates the
                                 * gradient in a shader into a cached texture.
                                 * Geometry mirrors build_gradient_raster() and
                                 * stops mirror build_lut() (sorted + capped). */
                                RGradDesc *gd = &in->gdesc;
                                memset(gd, 0, sizeof *gd);
                                gd->shape = L->gshape;
                                gd->nstops = L->ngstops;
                                for (int s = 0; s < L->ngstops; s++) {
                                        gd->stops[s] = L->gstops[s];
                                        gd->offs[s] = L->goffs[s];
                                }
                                /* sort stops by offset (insertion, <=16) */
                                for (int s = 1; s < gd->nstops; s++) {
                                        RColor c = gd->stops[s];
                                        f64 o = gd->offs[s];
                                        int j = s - 1;
                                        while (j >= 0 && gd->offs[j] > o) {
                                                gd->stops[j + 1] = gd->stops[j];
                                                gd->offs[j + 1] = gd->offs[j];
                                                j--;
                                        }
                                        gd->stops[j + 1] = c;
                                        gd->offs[j + 1] = o;
                                }
                                f64 rw_g = (f64)W * (1.0 + e.parallax * 0.08);
                                f64 rh_g = (f64)H * (1.0 + e.parallax * 0.08);
                                if (gd->shape == 0) {
                                        f64 rad = e.angle * M_PI / 180.0;
                                        f64 dirx = cos(rad), diry = sin(rad);
                                        f64 hx = fabs(dirx) * rw_g / 2 + fabs(diry) * rh_g / 2;
                                        gd->p0x = rw_g / 2 - dirx * hx;
                                        gd->p0y = rh_g / 2 - diry * hx;
                                        gd->p1x = rw_g / 2 + dirx * hx;
                                        gd->p1y = rh_g / 2 + diry * hx;
                                } else {
                                        gd->cx = L->gcx * rw_g;
                                        gd->cy = L->gcy * rh_g;
                                        gd->radius = L->gradius * (rw_g > rh_g ? rw_g : rh_g) * 0.5;
                                }
                                gd->frame_w = (u32)rw_g;
                                gd->frame_h = (u32)rh_g;
                                /* static gradients keep one stable serial (the
                                 * renderer renders once); animated angles get
                                 * a fresh serial per frame */
                                bool angle_anim = false;
                                for (int a = 0; a < L->nanims && !angle_anim; a++)
                                        angle_anim = (L->anims[a].target == AT_ANGLE);
                                u64 gid = angle_anim ? content_serial_next() : in->content_id;
                                if (!angle_anim && !gid)
                                        gid = in->content_id = content_serial_next();
                                if (fl->n >= RT_FRAME_MAX_LAYERS)
                                        break;
                                RTLayerCmd *cmd = &fl->cmds[fl->n++];
                                memset(cmd, 0, sizeof *cmd);
                                cmd->grad = gd;
                                cmd->w = gd->frame_w;
                                cmd->h = gd->frame_h;
                                cmd->place_w = 0;
                                cmd->place_h = 0;
                                /* parallax placement like the raster path */
                                f64 ox = (W - gd->frame_w) / 2.0 + (px - 0.5) * e.parallax * W * 0.04;
                                f64 oy = (H - gd->frame_h) / 2.0 + (py - 0.5) * e.parallax * H * 0.04;
                                cmd->dx = (i32)ox;
                                cmd->dy = (i32)oy;
                                cmd->blend = L->blend;
                                cmd->opacity = e.opacity;
                                cmd->blur = e.blur;
                                cmd->linear = false;
                                cmd->content_id = gid;
                                cmd->sim = NULL;
                                cmd->rsim = NULL;
                                cmd->psim = NULL;
                                continue;
                        }
                        if (!in->raster_ok || in->raster.w != rw || in->raster.h != rh) {
                                free(in->raster.px);
                                        free(in->sim_fb.px);
                                        in->sim_fb.px = NULL;
                                        free(in->pband);
                                        in->pband = NULL;
                                        in->pband_slots = 0;
                                in->raster.px = (u32 *)malloc((size_t)rw * rh * 4);
                                in->raster.w = rw;
                                in->raster.h = rh;
                                if (!in->raster.px)
                                        continue;
                                f64 ms = rev_monotonic();
                                if (L->type == LT_GRADIENT) {
                                        build_gradient_raster(&in->raster, L, &e);
                                } else if (L->type == LT_SOLID) {
                                        build_solid_raster(&in->raster, L->color);
                                } else {
                                        char err[256] = { 0 };
                                        if (!load_source(sc, L, in, err, sizeof err)) {
                                                LOGW("layer '%s': %s", L->name, err);
                                                continue;
                                        }
                                        build_image_raster(&in->raster, L, in);
                                }
                                sc->prof_raster_ms += (rev_monotonic() - ms) * 1000.0;
                                in->raster_ok = true;
                                in->blurred_ok = false;
                                in->content_id = content_serial_next();
                        }

                        /* gradient angle is the ONLY raster-affecting animated
                         * property (opacity/blur/parallax are applied at composite
                         * time) — rebuild only when the angle actually animates,
                         * and write it cleanly instead of blending over the
                         * previous frame's raster */
                        if (L->type == LT_GRADIENT) {
                                bool angle_anim = false;
                                for (int a = 0; a < L->nanims && !angle_anim; a++)
                                        angle_anim = (L->anims[a].target == AT_ANGLE);
                                if (angle_anim) {
                                        f64 ms = rev_monotonic();
                                        build_gradient_raster(&in->raster, L, &e);
                                        sc->prof_raster_ms += (rev_monotonic() - ms) * 1000.0;
                                        in->content_id = content_serial_next();
                                }
                        }

                        if (e.blur >= 0.5 && !fl) {
                                if (!in->blurred_ok || in->blur_amt != e.blur ||
                                    in->blurred.w != rw || in->blurred.h != rh) {
                                        free(in->blurred.px);
                                        in->blurred.px = (u32 *)malloc((size_t)rw * rh * 4);
                                        if (!in->blurred.px)
                                                continue;
                                        in->blurred.w = rw;
                                        in->blurred.h = rh;
                                        memcpy(in->blurred.px, in->raster.px, (size_t)rw * rh * 4);
                                        RFb tmp = { in->blurred.px, rw, rh };
                                        rev_blur(&tmp, e.blur);
                                        in->blur_amt = e.blur;
                                        in->blurred_ok = true;
                                }
                                srcfb = &in->blurred;
                        } else {
                                srcfb = &in->raster;
                        }
                }

                if (!srcfb || !srcfb->px)
                        continue;

                /* placement + composite/emit */
                if (fl) {
                        if (fl->n >= RT_FRAME_MAX_LAYERS)
                                break;
                        RTLayerCmd *cmd = &fl->cmds[fl->n++];
                        cmd->px = srcfb->px;
                        cmd->w = srcfb->w;
                        cmd->h = srcfb->h;
                        cmd->place_w = 0;
                        cmd->place_h = 0;
                        cmd->blend = L->blend;
                        cmd->opacity = e.opacity;
                        cmd->blur = (L->type == LT_IMAGE || L->type == LT_GRADIENT ||
                                     L->type == LT_SOLID) ? emit_blur : 0.0;
                        cmd->linear = emit_linear;
                        /* static rasters carry their content serial (renderers
                         * skip re-uploads while it is unchanged); sims and
                         * animated rebuilds upload every frame */
                        cmd->content_id = (L->type == LT_IMAGE || L->type == LT_GRADIENT ||
                                           L->type == LT_SOLID) ? in->content_id : 0;
                        cmd->sim = NULL;
                        if (emit_stretch) {
                                /* sub-frame-res sim stretched over the whole frame */
                                cmd->place_w = (u32)W;
                                cmd->place_h = (u32)H;
                                cmd->dx = 0;
                                cmd->dy = 0;
                        } else if ((u32)srcfb->w == (u32)W && (u32)srcfb->h == (u32)H) {
                                cmd->dx = 0;
                                cmd->dy = 0;
                        } else {
                                f64 ox = (W - srcfb->w) / 2.0 + (px - 0.5) * e.parallax * W * 0.04;
                                f64 oy = (H - srcfb->h) / 2.0 + (py - 0.5) * e.parallax * H * 0.04;
                                cmd->dx = (i32)ox;
                                cmd->dy = (i32)oy;
                        }
                } else if ((u32)srcfb->w == fb->w && (u32)srcfb->h == fb->h) {
                        rev_comp_blend(fb, srcfb, L->blend, e.opacity);
                } else {
                        f64 ox = (W - srcfb->w) / 2.0 + (px - 0.5) * e.parallax * W * 0.04;
                        f64 oy = (H - srcfb->h) / 2.0 + (py - 0.5) * e.parallax * H * 0.04;
                        rev_comp_blend_at(fb, srcfb, (int)ox, (int)oy, L->blend, e.opacity);
                }
        }

        free(scratch.px);
        free(below_snap);
        sc->ptr[mon].dx = 0;
        sc->ptr[mon].dy = 0;
}

void rtscene_render(RTScene *sc, RFb *fb, int mon, f64 t, f64 dt)
{
        render_core(sc, fb, NULL, mon, t, dt);
}

void rtscene_profile(RTScene *sc, f64 *sim_ms, f64 *raster_ms)
{
        if (sim_ms)
                *sim_ms = sc ? sc->prof_sim_ms : 0.0;
        if (raster_ms)
                *raster_ms = sc ? sc->prof_raster_ms : 0.0;
        if (sc) {
                /* reading resets — counters accumulate per render pass */
                sc->prof_sim_ms = 0;
                sc->prof_raster_ms = 0;
        }
}

void rtscene_render_layers(RTScene *sc, RTFrameList *fl, int mon, f64 t, f64 dt)
{
        if (!sc || !fl)
                return;
        fl->n = 0;
        render_core(sc, NULL, fl, mon, t, dt);
}
