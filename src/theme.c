/* ============================================================================
 * Reverie Engine — theme.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "theme.h"

#include <dirent.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "log.h"
#include "png.h"
#include "rev.h"
#include "util.h"

/* folder-theme asset cache cap: images are a few MB each; 256MB is far past
 * any sane theme and stops a hostile one from ballooning the process */
#define REV_THEME_CACHE_MAX (256u * 1024u * 1024u)

/* ---- hot-reload watch ------------------------------------------------------- */

/* Hot reload polls a snapshot of (path, mtime, size) for every file under
 * the theme. Watching the theme FOLDER's own mtime is useless on Linux:
 * a directory's mtime only moves when entries are added/removed/renamed —
 * editing scene.rev or replacing an asset leaves it untouched, so the old
 * check never fired on the common case. Themes are small (a scene, a json,
 * a handful of assets), so a bounded recursive stat() walk every 0.5s is
 * cheaper and more portable than inotify and needs no teardown. */
#define REV_THEME_WATCH_MAX 256
#define REV_THEME_WATCH_DEPTH 5

typedef struct {
        char rel[192];          /* theme-relative path ("assets/lake.png")  */
        f64 mtime;              /* seconds, ns resolution when the fs gives it */
        long long size;
} TWatchEnt;

enum { THEME_DIR, THEME_PACK };

typedef struct {
        char *name;
        u8 *data;
        size_t len;
} TCacheEnt;

struct RTheme {
        int mode;
        char root[512];           /* folder root or the .revpack path      */
        char base_dir[512];       /* for scene asset resolution on disk    */
        RThemeMeta meta;
        char *scene_text;         /* NUL-terminated (owned)                */
        size_t scene_len;
        RevPack pack;             /* THEME_PACK                            */
        TCacheEnt cache[64];      /* THEME_DIR                             */
        int ncache;
        size_t cache_bytes;
        TWatchEnt watch[REV_THEME_WATCH_MAX];   /* reload poll snapshot   */
        int nwatch;
        bool watch_capped;        /* >REV_THEME_WATCH_MAX files: partial   */
};

static void theme_watch_snapshot(RTheme *th);   /* defined with the pack walker */

/* ---- path safety ----------------------------------------------------------- */

bool rev_theme_path_safe(const char *rel)
{
        if (!rel || !*rel)
                return false;
        size_t n = strlen(rel);
        if (n > 512)
                return false;
        if (rel[0] == '/')
                return false;
        const char *p = rel;
        while (*p) {
                if ((unsigned char)*p < 0x20)
                        return false;
                if (*p == '\\')
                        return false;
                p++;
        }
        /* component walk: reject empty ("//") and ".." (and "." components) */
        p = rel;
        while (*p) {
                const char *e = strchr(p, '/');
                size_t cl = e ? (size_t)(e - p) : strlen(p);
                if (cl == 0)
                        return false;           /* "//" */
                if (cl == 2 && p[0] == '.' && p[1] == '.')
                        return false;           /* traversal */
                if (cl == 1 && p[0] == '.')
                        return false;           /* "./" noise */
                if (!e)
                        break;
                p = e + 1;
        }
        return true;
}

/* ---- engine version gate ---------------------------------------------------- */

static int parse_ver(const char *s, int *maj, int *min, int *pat)
{
        *maj = *min = *pat = 0;
        if (!s || !*s)
                return 0;
        return sscanf(s, "%d.%d.%d", maj, min, pat) >= 1 ? 1 : 0;
}

bool theme_engine_ok(const RThemeMeta *m, char *err, size_t errcap)
{
        if (!m || !m->engine_min[0])
                return true;
        int rmaj, rmin, rpat, cmaj, cmin, cpat;
        if (!parse_ver(m->engine_min, &rmaj, &rmin, &rpat)) {
                snprintf(err, errcap, "theme: bad engine_min '%s'", m->engine_min);
                return false;
        }
        parse_ver(REV_VERSION, &cmaj, &cmin, &cpat);
        if (cmaj > rmaj || (cmaj == rmaj && cmin > rmin) ||
            (cmaj == rmaj && cmin == rmin && cpat >= rpat))
                return true;
        snprintf(err, errcap,
                 "theme '%s' needs engine >= %s, this build is %s",
                 m->id[0] ? m->id : "?", m->engine_min, REV_VERSION);
        return false;
}

/* ---- metadata ---------------------------------------------------------------- */

static void meta_from_json(RThemeMeta *m, const RJVal *root)
{
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%s", rev_json_str(rev_json_get(root, "id"), ""));
        snprintf(m->name, sizeof m->name, "%s", rev_json_str(rev_json_get(root, "name"), ""));
        snprintf(m->description, sizeof m->description, "%s",
                 rev_json_str(rev_json_get(root, "description"), ""));
        snprintf(m->author, sizeof m->author, "%s", rev_json_str(rev_json_get(root, "author"), ""));
        snprintf(m->version, sizeof m->version, "%s", rev_json_str(rev_json_get(root, "version"), ""));
        snprintf(m->scene, sizeof m->scene, "%s", rev_json_str(rev_json_get(root, "scene"), "scene.rev"));
        snprintf(m->engine_min, sizeof m->engine_min, "%s",
                 rev_json_str(rev_json_get(root, "engine_min"), ""));
        const RJVal *prev = rev_json_get(root, "preview");
        if (prev && prev->kind == RJ_STR && prev->str && *prev->str) {
                snprintf(m->preview, sizeof m->preview, "%s", prev->str);
                m->has_preview = true;
        }
        const RJVal *q = rev_json_get(root, "quality");
        if (q && q->kind == RJ_ARR) {
                for (int i = 0; i < q->n && m->nquality < REV_THEME_MAX_QUALITY; i++) {
                        const RJVal *it = q->items[i];
                        const char *qn = rev_json_str(rev_json_get(it, "name"), "");
                        if (!*qn)
                                continue;
                        snprintf(m->quality[m->nquality].name,
                                 sizeof m->quality[0].name, "%s", qn);
                        m->quality[m->nquality].fps =
                                (int)rev_json_num(rev_json_get(it, "fps"), 0);
                        f64 rs = rev_json_num(rev_json_get(it, "resolution_scale"), 1.0);
                        m->quality[m->nquality].res_scale =
                                rs < 0.25 ? 0.25 : (rs > 2.0 ? 2.0 : rs);
                        m->nquality++;
                }
        }
}

static bool meta_from_data(RThemeMeta *m, const u8 *data, size_t len,
                           char *err, size_t errcap)
{
        RArena *ar = NULL;
        RJVal *root = rev_json_parse((const char *)data, len, &ar, err, errcap);
        if (!root)
                return false;
        if (root->kind != RJ_OBJ) {
                snprintf(err, errcap, "theme.json root must be an object");
                arena_destroy(ar);
                return false;
        }
        meta_from_json(m, root);
        arena_destroy(ar);
        if (!m->id[0]) {
                snprintf(err, errcap, "theme.json: missing required field 'id'");
                return false;
        }
        if (!m->name[0])
                snprintf(m->name, sizeof m->name, "%s", m->id);
        if (!rev_theme_path_safe(m->scene)) {
                snprintf(err, errcap, "theme.json: unsafe scene path '%s'", m->scene);
                return false;
        }
        if (m->has_preview && !rev_theme_path_safe(m->preview)) {
                snprintf(err, errcap, "theme.json: unsafe preview path");
                return false;
        }
        return true;
}

/* ---- folder themes ----------------------------------------------------------- */

static bool file_exists_at(const char *dir, const char *rel)
{
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, rel);
        return rev_file_exists(full);
}

static RTheme *theme_load_dir(const char *path, char *err, size_t errcap)
{
        char jpath[1024];
        snprintf(jpath, sizeof jpath, "%s/theme.json", path);
        size_t jlen = 0;
        u8 *jdata = rev_read_file(jpath, &jlen);
        if (!jdata) {
                snprintf(err, errcap, "'%s' is not a theme (no theme.json)", path);
                return NULL;
        }
        RThemeMeta meta;
        if (!meta_from_data(&meta, jdata, jlen, err, errcap)) {
                free(jdata);
                return NULL;
        }
        free(jdata);

        RTheme *th = (RTheme *)calloc(1, sizeof(RTheme));
        th->mode = THEME_DIR;
        snprintf(th->root, sizeof th->root, "%s", path);
        snprintf(th->base_dir, sizeof th->base_dir, "%s", path);
        th->meta = meta;

        char spath[1024];
        snprintf(spath, sizeof spath, "%s/%s", path, meta.scene);
        size_t slen = 0;
        u8 *stext = rev_read_file(spath, &slen);
        if (!stext) {
                snprintf(err, errcap, "theme '%s': entry scene '%s' not found",
                         meta.id, meta.scene);
                theme_free(th);
                return NULL;
        }
        th->scene_text = (char *)stext;
        th->scene_len = slen;
        (void)file_exists_at;
        theme_watch_snapshot(th);
        return th;
}

/* ---- packed themes ------------------------------------------------------------ */

static RTheme *theme_load_pack(const char *path, char *err, size_t errcap)
{
        RevPack pk;
        if (!rev_pack_load(path, &pk, err, errcap))
                return NULL;

        size_t jlen = 0;
        const u8 *jdata = rev_pack_asset(&pk, "theme.json", &jlen);
        if (!jdata) {
                snprintf(err, errcap,
                         "'%s' is a .revpack but not a theme (no theme.json entry)", path);
                rev_pack_free(&pk);
                return NULL;
        }
        RThemeMeta meta;
        if (!meta_from_data(&meta, jdata, jlen, err, errcap)) {
                rev_pack_free(&pk);
                return NULL;
        }

        size_t slen = 0;
        const u8 *stext = (const u8 *)pk.scene_text;
        if (stext)
                slen = pk.scene_len;
        /* defensive: if the header's scene slot is not the meta's scene,
         * look the entry up by name */
        {
                const u8 *alt = rev_pack_asset(&pk, meta.scene, &slen);
                if (alt) {
                        stext = alt;
                } else if (!stext) {
                        snprintf(err, errcap, "theme '%s': entry scene '%s' not in pack",
                                 meta.id, meta.scene);
                        rev_pack_free(&pk);
                        return NULL;
                }
        }

        RTheme *th = (RTheme *)calloc(1, sizeof(RTheme));
        th->mode = THEME_PACK;
        snprintf(th->root, sizeof th->root, "%s", path);
        th->base_dir[0] = 0;
        th->meta = meta;
        th->pack = pk;
        th->scene_text = (char *)malloc(slen + 1);
        if (!th->scene_text) {
                snprintf(err, errcap, "out of memory");
                theme_free(th);
                return NULL;
        }
        memcpy(th->scene_text, stext, slen);
        th->scene_text[slen] = 0;
        th->scene_len = slen;
        theme_watch_snapshot(th);
        return th;
}

/* ---- public -------------------------------------------------------------------- */

RTheme *theme_load(const char *path, char *err, size_t errcap)
{
        if (!path || !*path) {
                snprintf(err, errcap, "theme: empty path");
                return NULL;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
                snprintf(err, errcap, "cannot stat '%s'", path);
                return NULL;
        }
        if (S_ISDIR(st.st_mode))
                return theme_load_dir(path, err, errcap);
        if (rev_pack_probe(path))
                return theme_load_pack(path, err, errcap);
        snprintf(err, errcap,
                 "'%s' is neither a theme folder nor a .revpack", path);
        return NULL;
}

void theme_free(RTheme *th)
{
        if (!th)
                return;
        free(th->scene_text);
        if (th->mode == THEME_PACK)
                rev_pack_free(&th->pack);
        for (int i = 0; i < th->ncache; i++) {
                free(th->cache[i].name);
                free(th->cache[i].data);
        }
        free(th);
}

const char *theme_scene_text(const RTheme *th, size_t *len)
{
        if (!th)
                return NULL;
        if (len)
                *len = th->scene_len;
        return th->scene_text;
}

const RThemeMeta *theme_meta(const RTheme *th)
{
        return th ? &th->meta : NULL;
}

const char *theme_base_dir(const RTheme *th)
{
        return th ? th->base_dir : "";
}

u8 *theme_asset(RTheme *th, const char *rel, size_t *len)
{
        if (!th || !rel || !*rel || !rev_theme_path_safe(rel))
                return NULL;
        if (th->mode == THEME_PACK)
                return (u8 *)rev_pack_asset(&th->pack, rel, len);

        for (int i = 0; i < th->ncache; i++)
                if (strcmp(th->cache[i].name, rel) == 0) {
                        if (len)
                                *len = th->cache[i].len;
                        return th->cache[i].data;
                }
        if (th->ncache >= 64)
                return NULL;    /* cache full — a theme with >64 assets is a bug */
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", th->root, rel);
        size_t flen = 0;
        u8 *data = rev_read_file(full, &flen);
        if (!data)
                return NULL;
        if (th->cache_bytes + flen > REV_THEME_CACHE_MAX) {
                free(data);
                LOGW("theme: asset cache cap hit loading '%s'", rel);
                return NULL;
        }
        th->cache[th->ncache].name = strdup(rel);
        th->cache[th->ncache].data = data;
        th->cache[th->ncache].len = flen;
        th->cache_bytes += flen;
        th->ncache++;    /* 2026-10: the entry was written but the count never
                          * moved — the cache could never hit (every lookup
                          * re-read the file and overwrote slot 0, leaking the
                          * previous entry) and theme_free freed nothing */
        if (len)
                *len = flen;
        return data;
}

/* ---- packing a folder theme ----------------------------------------------------- */

/* Snapshot helpers: collect every regular file under `dir` (bounded by
 * REV_THEME_WATCH_MAX entries / REV_THEME_WATCH_DEPTH levels — also bounds
 * symlink loops). Returns the entry count, or -1 on a transient failure. */
static int tw_collect(const char *dir, const char *prefix, TWatchEnt *out,
                       int max, int depth)
{
        if (depth > REV_THEME_WATCH_DEPTH)
                return 0;
        DIR *d = opendir(dir);
        if (!d)
                return 0;    /* unreadable subtree: simply unwatched */
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL && n < max) {
                const char *nm = de->d_name;
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                        continue;
                /* rel mirrors the entry field size: the snprintf guard below
                 * skips anything longer, so the copy into out[n].rel cannot
                 * truncate */
                char full[1024], rel[192];
                if (snprintf(full, sizeof full, "%s/%s", dir, nm) >= (int)sizeof full)
                        continue;
                if (snprintf(rel, sizeof rel, "%s%s%s", prefix ? prefix : "",
                             prefix ? "/" : "", nm) >= (int)sizeof rel)
                        continue;
                struct stat st;
                if (stat(full, &st) != 0)
                        continue;
                if (S_ISDIR(st.st_mode)) {
                        int sub = tw_collect(full, rel, out + n, max - n, depth + 1);
                        if (sub < 0) {
                                closedir(d);
                                return -1;
                        }
                        n += sub;
                        continue;
                }
                if (!S_ISREG(st.st_mode))
                        continue;
                snprintf(out[n].rel, sizeof out[n].rel, "%s", rel);
                out[n].mtime = (f64)st.st_mtim.tv_sec +
                               (f64)st.st_mtim.tv_nsec * 1e-9;
                out[n].size = (long long)st.st_size;
                n++;
        }
        closedir(d);
        return n;
}

/* Called once at load (and after each successful reload — the reload builds
 * a fresh RTheme): folders snapshot the whole tree, packs stat the single
 * container file. */
static void theme_watch_snapshot(RTheme *th)
{
        th->nwatch = 0;
        th->watch_capped = false;
        if (th->mode == THEME_PACK) {
                struct stat st;
                if (stat(th->root, &st) == 0) {
                        snprintf(th->watch[0].rel, sizeof th->watch[0].rel, "(pack)");
                        th->watch[0].mtime = (f64)st.st_mtim.tv_sec +
                                             (f64)st.st_mtim.tv_nsec * 1e-9;
                        th->watch[0].size = (long long)st.st_size;
                        th->nwatch = 1;
                }
                return;
        }
        int n = tw_collect(th->root, NULL, th->watch, REV_THEME_WATCH_MAX, 0);
        if (n < 0)
                n = 0;
        th->nwatch = n;
        if (n == REV_THEME_WATCH_MAX) {
                th->watch_capped = true;
                LOGW("theme: >%d files under '%s' — hot-reload watch is partial",
                     REV_THEME_WATCH_MAX, th->root);
        }
}

bool theme_files_changed(RTheme *th)
{
        if (!th || th->nwatch == 0)
                return false;    /* nothing snapshotted: leave firing to the caller */
        if (th->mode == THEME_PACK) {
                struct stat st;
                if (stat(th->root, &st) != 0)
                        return true;    /* container vanished — reload (fails loudly) */
                f64 mt = (f64)st.st_mtim.tv_sec + (f64)st.st_mtim.tv_nsec * 1e-9;
                return mt != th->watch[0].mtime ||
                       (long long)st.st_size != th->watch[0].size;
        }
        /* folder: re-walk and compare bidirectionally (readdir order is
         * arbitrary, so no positional assumptions) */
        static TWatchEnt cur[REV_THEME_WATCH_MAX];   /* daemon poll: single-threaded */
        int n = tw_collect(th->root, NULL, cur, REV_THEME_WATCH_MAX, 0);
        if (n < 0)
                return false;    /* transient walk failure — retry next poll */
        if (n != th->nwatch)
                return true;     /* a file was added or removed */
        for (int i = 0; i < n; i++) {
                const TWatchEnt *o = NULL;
                for (int j = 0; j < th->nwatch; j++)
                        if (strcmp(cur[i].rel, th->watch[j].rel) == 0) {
                                o = &th->watch[j];
                                break;
                        }
                if (!o || o->mtime != cur[i].mtime || o->size != cur[i].size)
                        return true;
        }
        return false;
}

static void pk32(u8 *p, u32 v)
{
        p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void pk64(u8 *p, u64 v)
{
        pk32(p, (u32)v);
        pk32(p + 4, (u32)(v >> 32));
}

typedef struct {
        char *rel;
        u8 *data;
        size_t len;
} TFile;

static int tf_collect(const char *dir, const char *prefix, TFile *out, int max, char *err, size_t errcap)
{
        DIR *d = opendir(dir);
        if (!d) {
                snprintf(err, errcap, "cannot open directory '%s'", dir);
                return -1;
        }
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
                const char *nm = de->d_name;
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                        continue;
                char full[1024], rel[1024];
                snprintf(full, sizeof full, "%s/%s", dir, nm);
                snprintf(rel, sizeof rel, "%s%s%s", prefix ? prefix : "",
                         prefix ? "/" : "", nm);
                struct stat st;
                if (stat(full, &st) != 0)
                        continue;
                if (S_ISDIR(st.st_mode)) {
                        int sub = tf_collect(full, rel, out + n, max - n, err, errcap);
                        if (sub < 0) {
                                closedir(d);
                                return -1;
                        }
                        n += sub;
                        continue;
                }
                if (!S_ISREG(st.st_mode))
                        continue;
                if (!rev_theme_path_safe(rel)) {
                        snprintf(err, errcap, "refusing to pack unsafe path '%s'", rel);
                        closedir(d);
                        return -1;
                }
                /* no executable content from themes — reject shared objects
                 * and scripts outright */
                size_t rl = strlen(rel);
                bool bad = false;
                if (rl >= 3 && strcmp(rel + rl - 3, ".so") == 0)
                        bad = true;
                if (rl >= 4 && strcmp(rel + rl - 4, ".dll") == 0)
                        bad = true;
                if (rl >= 4 && strcmp(rel + rl - 4, ".exe") == 0)
                        bad = true;
                if (st.st_mode & S_IXUSR)
                        bad = true;
                if (bad) {
                        snprintf(err, errcap,
                                 "refusing executable entry '%s' (themes carry data, not code)",
                                 rel);
                        closedir(d);
                        return -1;
                }
                if (n >= max) {
                        snprintf(err, errcap, "too many files in theme (>%d)", max);
                        closedir(d);
                        return -1;
                }
                size_t flen = 0;
                u8 *data = rev_read_file(full, &flen);
                if (!data) {
                        snprintf(err, errcap, "cannot read '%s'", full);
                        closedir(d);
                        return -1;
                }
                out[n].rel = strdup(rel);
                out[n].data = data;
                out[n].len = flen;
                n++;
        }
        closedir(d);
        return n;
}

bool theme_pack(const char *theme_dir, const char *out_path, char *err, size_t errcap)
{
        RTheme *th = theme_load(theme_dir, err, errcap);
        if (!th)
                return false;
        RThemeMeta meta = th->meta;
        char scene_name[256];
        snprintf(scene_name, sizeof scene_name, "%s", meta.scene);
        theme_free(th);

        TFile files[REV_PACK_MAX_ASSETS];
        int n = tf_collect(theme_dir, NULL, files, REV_PACK_MAX_ASSETS, err, errcap);
        if (n < 0)
                return false;

        /* locate the entry scene among the collected files */
        int scene_idx = -1;
        for (int i = 0; i < n; i++)
                if (strcmp(files[i].rel, scene_name) == 0) {
                        scene_idx = i;
                        break;
                }
        if (scene_idx < 0) {
                snprintf(err, errcap, "entry scene '%s' not found in '%s'",
                         scene_name, theme_dir);
                for (int i = 0; i < n; i++) {
                        free(files[i].rel);
                        free(files[i].data);
                }
                return false;
        }

        /* layout: header + entries + name blob + scene text + data blobs */
        u32 name_blob_off = 40 + (u32)n * 28;
        u32 name_blob_len = 0;
        for (int i = 0; i < n; i++)
                name_blob_len += (u32)strlen(files[i].rel) + 1;
        u32 scene_off = name_blob_off + name_blob_len;
        u32 scene_len32 = (u32)files[scene_idx].len;
        u32 data_off = scene_off + scene_len32 + 1;
        u64 total = data_off;
        for (int i = 0; i < n; i++)
                total += files[i].len;
        if (total > 0x7fffffffull) {
                snprintf(err, errcap, "theme pack too large (%llu bytes)",
                         (unsigned long long)total);
                for (int i = 0; i < n; i++) {
                        free(files[i].rel);
                        free(files[i].data);
                }
                return false;
        }

        u8 *out = (u8 *)calloc(1, (size_t)total + 16);
        if (!out) {
                snprintf(err, errcap, "out of memory");
                for (int i = 0; i < n; i++) {
                        free(files[i].rel);
                        free(files[i].data);
                }
                return false;
        }

        memcpy(out, REV_PACK_MAGIC, 6);
        out[6] = (u8)REV_PACK_VERSION;
        out[7] = (u8)(REV_PACK_VERSION >> 8);
        pk32(out + 8, scene_off);
        pk32(out + 12, scene_len32);
        pk32(out + 16, (u32)n);
        pk32(out + 20, name_blob_off);
        pk32(out + 24, name_blob_len);
        pk32(out + 28, data_off);
        pk32(out + 32, 0);
        out[36] = 0;
        out[37] = 2;      /* engine version marker bytes (informational) */
        out[38] = 0;
        out[39] = 1;      /* flags bit0: theme pack */

        u32 no = 0;
        u64 doff = data_off;
        for (int i = 0; i < n; i++) {
                u8 *e = out + 40 + (size_t)i * 28;
                u32 nl = (u32)strlen(files[i].rel) + 1;
                memcpy(out + name_blob_off + no, files[i].rel, nl);
                pk32(e, no);
                pk32(e + 4, nl);
                pk64(e + 8, doff);
                pk64(e + 16, files[i].len);
                pk32(e + 24, rev_crc32(files[i].data, files[i].len));
                no += nl;
                doff += files[i].len;
        }
        memcpy(out + scene_off, files[scene_idx].data, files[scene_idx].len);
        out[scene_off + files[scene_idx].len] = 0;
        doff = data_off;
        for (int i = 0; i < n; i++) {
                memcpy(out + doff, files[i].data, files[i].len);
                doff += files[i].len;
        }
        pk32(out + 32, rev_crc32(out, 32));

        bool ok = rev_write_file(out_path, out, (size_t)total);
        if (!ok)
                snprintf(err, errcap, "cannot write '%s'", out_path);
        else
                LOGI("theme pack: %d file(s), %llu bytes -> %s", n,
                     (unsigned long long)total, out_path);
        free(out);
        for (int i = 0; i < n; i++) {
                free(files[i].rel);
                free(files[i].data);
        }
        return ok;
}

int theme_list_dir(const char *dir, char **names, int max)
{
        DIR *d = opendir(dir);
        if (!d)
                return 0;
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL && n < max) {
                const char *nm = de->d_name;
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                        continue;
                char full[1024];
                snprintf(full, sizeof full, "%s/%s", dir, nm);
                struct stat st;
                if (stat(full, &st) != 0)
                        continue;
                if (S_ISDIR(st.st_mode)) {
                        char jpath[1100];
                        snprintf(jpath, sizeof jpath, "%s/theme.json", full);
                        if (!rev_file_exists(jpath))
                                continue;
                } else if (S_ISREG(st.st_mode)) {
                        size_t rl = strlen(nm);
                        if (rl < 8 || strcmp(nm + rl - 8, ".revpack") != 0)
                                continue;
                        if (!rev_pack_probe(full))
                                continue;
                } else {
                        continue;
                }
                names[n++] = strdup(nm);
        }
        closedir(d);
        return n;
}
