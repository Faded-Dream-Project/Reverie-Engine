/* ============================================================================
 * Reverie Engine — theme.h
 * Themes: a scene, its metadata, and every asset it references, organized
 * as either a FOLDER (development-friendly) or a single .revpack file
 * (distribution). One resource interface serves both — the scene loader
 * never learns which it is reading from.
 *
 * Folder layout (relocatable — all paths are theme-relative):
 *     <theme>/
 *       theme.json     metadata (this file is what makes a folder a theme)
 *       scene.rev      entry scene (or whatever "scene" says in theme.json)
 *       assets/        images referenced by the scene
 *       shaders/       RESERVED (not loaded — see docs/THEMES.md security)
 *       videos/        video resources for video layers
 *
 * Packed layout: the same files as entries of a .revpack v2 container,
 * accessed in place (no extraction).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_THEME_H
#define REV_THEME_H

#include "pack.h"
#include "rev_types.h"

#define REV_THEME_MAX_QUALITY 4

typedef struct {
        char id[64];
        char name[128];
        char description[256];
        char author[64];
        char version[32];
        char scene[256];            /* entry scene, theme-relative        */
        char engine_min[16];        /* minimum engine version, "0.0.0"    */
        char preview[256];          /* optional preview image, relative   */
        bool has_preview;
        int nquality;               /* optional named quality presets     */
        struct {
                char name[32];
                int fps;            /* target fps, 0 = scene default      */
                f64 res_scale;      /* resolution scale, 1.0 = native     */
        } quality[REV_THEME_MAX_QUALITY];
} RThemeMeta;

typedef struct RTheme RTheme;

/* Loads a theme from a DIRECTORY (must contain theme.json) or a packed
 * .revpack v2 theme. The scene text is read but NOT compiled — callers
 * pass theme_scene_text() to their scene pipeline. Returns NULL with a
 * message on any structural problem. */
RTheme *theme_load(const char *path, char *err, size_t errcap);
void theme_free(RTheme *th);

/* The entry scene text (NUL-terminated view owned by the theme). */
const char *theme_scene_text(const RTheme *th, size_t *len);

/* Reads a theme-relative resource ("assets/lake.png"). Folder themes
 * cache file contents on first read (bounded by REV_THEME_CACHE_MAX);
 * packed themes return direct views into the mapped container. Returns
 * NULL when the resource is absent. */
/* NOTE: not const — folder themes lazily cache file contents on first
 * read (bounded); packed themes are pure views. */
u8 *theme_asset(RTheme *th, const char *rel, size_t *len);

const RThemeMeta *theme_meta(const RTheme *th);

/* Base directory for scenes that resolve relative asset paths against
 * disk ("" for packed themes — they use the asset provider instead). */
const char *theme_base_dir(const RTheme *th);

/* Packs a folder theme into a .revpack v2 (all files, nested paths). */
bool theme_pack(const char *theme_dir, const char *out_path,
                char *err, size_t errcap);

/* Lists theme folders and .revpack themes under a directory; returns the
 * count, fills names[] (malloc'd copies, caller frees) up to max. */
int theme_list_dir(const char *dir, char **names, int max);

/* Resource path safety: no absolute paths, no '..' components, no
 * empty components, no backslashes, no control chars, sane length. */
bool rev_theme_path_safe(const char *rel);

/* Engine version gate: compares against REV_VERSION; false + message when
 * the theme requires a newer engine. */
bool theme_engine_ok(const RThemeMeta *m, char *err, size_t errcap);

#endif /* REV_THEME_H */
