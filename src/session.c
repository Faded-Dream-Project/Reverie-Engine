/* ============================================================================
 * Reverie Engine — session.c
 * The last-launched memory behind `reverie --restore` (see session.h).
 * One JSON file under $XDG_STATE_HOME/reverie/: the wallpaper-defining
 * subset of the command line with the content path made absolute. Writes
 * are atomic (tmp + rename) so a daemon killed mid-save can never leave a
 * half-written record behind; reads go through the REAL CLI parser, which
 * is the allowlist: a record can never make reverie do anything a typed
 * command line could not.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arena.h"
#include "json.h"
#include "util.h"

/* ---- where the record lives ------------------------------------------------ */

static bool state_dir(char *buf, size_t cap, char *err, size_t errcap)
{
        const char *xdg = getenv("XDG_STATE_HOME");
        if (xdg && *xdg) {
                if (snprintf(buf, cap, "%s/reverie", xdg) >= (int)cap)
                        goto too_long;
                return true;
        }
        const char *home = getenv("HOME");
        if (!home || !*home) {
                snprintf(err, errcap, "no XDG_STATE_HOME and no HOME — "
                                      "nowhere to remember the session");
                return false;
        }
        if (snprintf(buf, cap, "%s/.local/state/reverie", home) >= (int)cap)
                goto too_long;
        return true;
too_long:
        snprintf(err, errcap, "state directory path too long");
        return false;
}

bool rev_session_path(char *buf, size_t cap, char *err, size_t errcap)
{
        char dir[REV_MKDIR_P_MAX];
        if (!state_dir(dir, sizeof dir, err, errcap))
                return false;
        if (snprintf(buf, cap, "%s/last-session.json", dir) >= (int)cap) {
                snprintf(err, errcap, "session record path too long");
                return false;
        }
        return true;
}

/* ---- absolute content paths ------------------------------------------------ */

/* The daemon resolves relative content paths against ITS working directory
 * when loading, so the record must freeze that resolution: at restore time
 * (a login session, a different cwd) the bare relative path would point
 * nowhere. The user's path shape is preserved — no symlink canonicalization,
 * no trailing-slash games: what loaded is what gets recorded. */
static bool make_absolute(const char *p, char *out, size_t cap)
{
        if (!p || !*p)
                return false;
        if (p[0] == '/')
                return snprintf(out, cap, "%s", p) < (int)cap;
        char cwd[512];
        if (!getcwd(cwd, sizeof cwd))
                return false;
        int w = snprintf(out, cap, "%s/%s", cwd, p);
        return w > 0 && (size_t)w < cap;
}

/* ---- writing ---------------------------------------------------------------- */

/* Appends `s` as a JSON string to `buf` (bounded), with the standard
 * escapes. Returns false when it does not fit. */
static bool json_append_string(char *buf, size_t cap, const char *s)
{
        size_t n = strlen(buf);
        if (n + 2 >= cap)
                return false;
        buf[n++] = '"';
        for (const char *c = s; *c; c++) {
                const char *esc = NULL;
                switch (*c) {
                case '"':  esc = "\\\""; break;
                case '\\': esc = "\\\\"; break;
                case '\n': esc = "\\n"; break;
                case '\r': esc = "\\r"; break;
                case '\t': esc = "\\t"; break;
                default: break;
                }
                if (esc) {
                        if (n + 2 >= cap)
                                return false;
                        buf[n++] = esc[0];
                        buf[n++] = esc[1];
                } else {
                        if (n + 1 >= cap)
                                return false;
                        buf[n++] = *c;
                }
        }
        if (n + 2 >= cap)
                return false;
        buf[n++] = '"';
        buf[n] = 0;
        return true;
}

/* Atomic write: the JSON lands in a sibling .tmp first, then rename()
 * swaps it in. A crash between the two leaves the PREVIOUS record intact
 * (or nothing, on the very first launch) — never a torn file. */
static bool session_write(const char *const *toks, int n,
                          char *err, size_t errcap)
{
        char path[384];
        if (!rev_session_path(path, sizeof path, err, errcap))
                return false;
        char dir[REV_MKDIR_P_MAX];
        if (!state_dir(dir, sizeof dir, err, errcap))
                return false;
        if (!rev_mkdir_p(dir)) {
                snprintf(err, errcap, "cannot create %s", dir);
                return false;
        }

        char json[REV_SESSION_MAX_JSON];
        snprintf(json, sizeof json, "{\n  \"version\": 1,\n  \"argv\": [");
        for (int i = 0; i < n; i++) {
                if (i) {
                        size_t l = strlen(json);
                        if (l + 1 < sizeof json)
                                json[l] = ',', json[l + 1] = 0;
                }
                if (!json_append_string(json, sizeof json, toks[i])) {
                        snprintf(err, errcap, "session record too long");
                        return false;
                }
        }
        strncat(json, "]\n}\n", sizeof json - strlen(json) - 1);

        char tmp[416];
        if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
                snprintf(err, errcap, "session record path too long");
                return false;
        }
        if (!rev_write_file(tmp, json, strlen(json)) ||
            rename(tmp, path) != 0) {
                snprintf(err, errcap, "cannot write %s", path);
                return false;
        }
        return true;
}

/* ---- reading ---------------------------------------------------------------- */

bool rev_session_load(RevSession *s, char *err, size_t errcap)
{
        memset(s, 0, sizeof *s);
        char path[384];
        if (!rev_session_path(path, sizeof path, err, errcap))
                return false;
        size_t len = 0;
        u8 *text = rev_read_file(path, &len);
        if (!text) {
                snprintf(err, errcap, "nothing to restore — no previous "
                                      "session recorded (%s)", path);
                return false;
        }
        if (len > REV_SESSION_MAX_JSON) {
                free(text);
                snprintf(err, errcap, "session record is too large (%s)",
                         path);
                return false;
        }
        RArena *ar = NULL;
        RJVal *root = rev_json_parse((const char *)text, len, &ar,
                                     err, errcap);
        free(text);
        if (!root)
                return false;    /* err: the parser's message + offset */

        const RJVal *ver = rev_json_get(root, "version");
        if (!ver || rev_json_num(ver, -1.0) != 1.0) {
                snprintf(err, errcap, "unsupported session record version "
                                      "(want 1)");
                arena_destroy(ar);
                return false;
        }
        const RJVal *arr = rev_json_get(root, "argv");
        if (!arr || arr->kind != RJ_ARR) {
                snprintf(err, errcap, "session record has no argv array");
                arena_destroy(ar);
                return false;
        }
        int n = rev_json_len(arr);
        if (n <= 0 || n > REV_SESSION_MAX_ARGS) {
                snprintf(err, errcap, "session record argv is empty or "
                                      "oversized (%d entries)", n);
                arena_destroy(ar);
                return false;
        }
        size_t used = 0;
        for (int i = 0; i < n; i++) {
                const char *t = rev_json_str(arr->items[i], NULL);
                if (!t || !*t) {
                        snprintf(err, errcap, "session record argv entry %d "
                                              "is not a non-empty string", i);
                        arena_destroy(ar);
                        return false;
                }
                size_t l = strlen(t) + 1;
                if (used + l > sizeof s->line) {
                        snprintf(err, errcap, "session record arguments too "
                                              "long");
                        arena_destroy(ar);
                        return false;
                }
                memcpy(s->line + used, t, l);
                s->argv[i] = s->line + used;
                used += l;
        }
        s->argc = n;
        arena_destroy(ar);
        return true;
}

/* ---- recording a launch ------------------------------------------------------ */

static const char *scale_name(int m)
{
        switch (m) {
        case REV_SCALE_FILL:    return "fill";
        case REV_SCALE_FIT:     return "fit";
        case REV_SCALE_STRETCH: return "stretch";
        case REV_SCALE_CENTER:  return "center";
        default:                return "fill";
        }
}

bool rev_session_record(const RevArgs *a, char *err, size_t errcap)
{
        const char *content = a->video_path ? a->video_path
                          : a->image_path ? a->image_path
                          : a->scene_path;
        if (!content) {
                snprintf(err, errcap, "nothing to record — no content");
                return false;
        }
        char abs[512];
        if (!make_absolute(content, abs, sizeof abs)) {
                snprintf(err, errcap, "content path too long to record");
                return false;
        }

        /* Only what defines the wallpaper. Tool flags (--once/--out/--shot,
         * verbosity) are properties of a particular invocation, and env-var
         * fallbacks are deliberately NOT frozen here: the record replays
         * the command line, not the environment. --scale is shared by both
         * media modes; --no-loop only means something for --video. */
        char nfps[24], nmon[24];
        const char *toks[REV_SESSION_MAX_ARGS];
        int n = 0;
        toks[n++] = a->video_path ? "--video"
                  : a->image_path ? "--image" : "--scene";
        toks[n++] = abs;
        if ((a->video_path || a->image_path) && a->scale_seen) {
                toks[n++] = "--scale";
                toks[n++] = scale_name(a->scale);
        }
        if (a->video_path && a->no_loop)
                toks[n++] = "--no-loop";
        if (a->fps_override > 0) {
                snprintf(nfps, sizeof nfps, "%d", a->fps_override);
                toks[n++] = "--fps";
                toks[n++] = nfps;
        }
        if (a->only_monitor >= 0) {
                snprintf(nmon, sizeof nmon, "%d", a->only_monitor);
                toks[n++] = "--monitor";
                toks[n++] = nmon;
        }
        if (a->renderer_pref) {
                toks[n++] = "--renderer";
                toks[n++] = a->renderer_pref;
        }
        if (a->backend_pref) {
                toks[n++] = "--backend";
                toks[n++] = a->backend_pref;
        }
        return session_write(toks, n, err, errcap);
}

bool rev_session_record_content(const char *scene_path, char *err,
                                size_t errcap)
{
        if (!scene_path || !*scene_path) {
                snprintf(err, errcap, "no scene path to record");
                return false;
        }
        char abs[512];
        if (!make_absolute(scene_path, abs, sizeof abs)) {
                snprintf(err, errcap, "scene path too long to record");
                return false;
        }

        RevSession old;
        char e2[128];
        bool have_old = rev_session_load(&old, e2, sizeof e2);

        /* carry the previous record's non-content flags over — the switch
         * changes WHAT is on the desktop, not how it was launched. Strict
         * allowlist: only the four value-taking wallpaper flags survive
         * (--fps/--monitor/--renderer/--backend, value included); the old
         * content pair, media-only flags, positionals, and anything a
         * hand-edited record smuggled in are dropped. The new record is
         * re-validated through the real parser at the next --restore. */
        const char *toks[REV_SESSION_MAX_ARGS];
        int n = 0;
        toks[n++] = "--scene";
        toks[n++] = abs;
        if (have_old) {
                for (int i = 0; i < old.argc; ) {
                        const char *t = old.argv[i];
                        bool takes = !strcmp(t, "--fps") ||
                                     !strcmp(t, "--monitor") ||
                                     !strcmp(t, "--renderer") ||
                                     !strcmp(t, "--backend");
                        if (!takes) {
                                i++;
                                continue;
                        }
                        if (i + 1 >= old.argc)
                                break;    /* truncated pair: drop the rest */
                        if (n + 2 > REV_SESSION_MAX_ARGS)
                                break;    /* out of room: stop carrying */
                        toks[n++] = t;
                        toks[n++] = old.argv[i + 1];
                        i += 2;
                }
        }
        return session_write(toks, n, err, errcap);
}

/* ---- --restore ---------------------------------------------------------------- */

bool rev_session_apply(RevArgs *a, RevSession *s, char *err, size_t errcap)
{
        if (!rev_session_load(s, err, errcap))
                return false;

        /* replay through the real parser — the parser is the allowlist, so
         * whatever a corrupt or hand-edited record contains can only fail,
         * never inject behavior a typed command line could not produce.
         * The merged content pointers in *a reference s->line: the caller
         * keeps `s` alive (it owns the tokens). */
        char *rargv[REV_SESSION_MAX_ARGS + 1];
        rargv[0] = (char *)"reverie";   /* argv[0] placeholder */
        for (int i = 0; i < s->argc; i++)
                rargv[i + 1] = s->argv[i];

        RevArgs ra;
        char perr[256];
        if (!rev_cli_parse(s->argc + 1, rargv, &ra, perr, sizeof perr)) {
                snprintf(err, errcap, "session record does not parse: %s",
                         perr);
                return false;
        }
        if (ra.restore) {
                snprintf(err, errcap, "session record contains --restore "
                                      "(corrupt or hand-edited)");
                return false;
        }
        if (!ra.scene_path && !ra.video_path && !ra.image_path) {
                snprintf(err, errcap, "session record has no content");
                return false;
        }

        /* merge: the record supplies the content and its launch settings;
         * anything the restoring invocation spelled out explicitly wins
         * (the sentinels are unambiguous — NULL / 0 / -1 / *_seen) */
        a->scene_path = ra.scene_path;
        a->video_path = ra.video_path;
        a->image_path = ra.image_path;
        if (!a->scale_seen)
                a->scale = ra.scale;
        if (!a->no_loop_seen)
                a->no_loop = ra.no_loop;
        if (a->fps_override <= 0)
                a->fps_override = ra.fps_override;
        if (a->only_monitor < 0)
                a->only_monitor = ra.only_monitor;
        if (!a->renderer_pref)
                a->renderer_pref = ra.renderer_pref;
        if (!a->backend_pref)
                a->backend_pref = ra.backend_pref;

        /* the media-flag checks cli.c deferred past --restore (the content
         * they validate against was not known at parse time yet) */
        if (a->scale_seen && !a->video_path && !a->image_path) {
                snprintf(err, errcap, "--scale applies to --video or --image");
                return false;
        }
        if (a->no_loop_seen && !a->video_path) {
                snprintf(err, errcap, "--no-loop applies to --video media mode");
                return false;
        }
        a->restore = false;    /* consumed */
        return true;
}
