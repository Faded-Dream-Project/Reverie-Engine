/* ============================================================================
 * Reverie Engine — cli.h
 * Command-line parsing, extracted from main() so the argument contract is
 * unit-testable (tests/test_cli.c).
 *
 * Content modes, mutually exclusive:
 *   - a positional path (compat: themes/<id>, any .rev scene, packs)
 *   - --scene PATH (the same slot, explicit)
 *   - --video PATH (media mode; wired in the media-mode commit)
 *   - --image PATH (static media mode: one PNG through the native codec,
 *     no FFmpeg anywhere in the path — same --scale placement vocabulary
 *     as --video)
 *   - --restore (fills the content slot from the session record at
 *     startup — not a fifth mode, but it cannot be combined with any
 *     of the four above)
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_CLI_H
#define REV_CLI_H

#include "rev_types.h"

typedef struct {
        /* ---- content ---- */
        const char *scene_path;   /* positional or --scene; NULL when none */
        const char *video_path;   /* --video media mode; NULL when absent  */
        const char *image_path;   /* --image static media mode; NULL when
                                     absent (native PNG, no FFmpeg)        */

        /* ---- selection ---- */
        const char *renderer_pref;/* --renderer / -R value ("auto" unset)  */
        const char *backend_pref; /* --backend / -B value                  */
        int  fps_override;        /* --fps; 0 = scene default              */
        int  only_monitor;        /* --monitor; -1 = all                   */

        /* ---- offline / info ---- */
        bool once;                /* --once: one frame to PNG, no display  */
        bool list;                /* --list: enumerate monitors, exit      */
        u32  out_w, out_h;        /* --size (defaults 1920x1080)           */
        const char *out_png;      /* --out (default reverie.png)           */

        /* ---- media mode ---- */
        int  scale;               /* --scale: REV_SCALE_* (default FILL) —
                                     applies to --video AND --image       */
        bool no_loop;             /* --no-loop: hold last frame at EOF
                                     (--video only: a static image has no
                                     loop to decline)                      */

        /* ---- daemon ---- */
        bool exit_if_running;     /* --exit-if-running: yield to a live
                                     daemon (IPC probe) instead of stacking
                                     a second wallpaper instance           */
        bool restore;             /* --restore: content comes from the last
                                     session record (see session.h) — an
                                     error in combination with any explicit
                                     content on the same line              */

        /* ---- test hook ---- */
        f64  shot_after;          /* --shot-after (or legacy --shot N F);
                                     -1 = no screenshot                    */
        const char *shot_file;

        /* ---- parser outputs (not options) ---- */
        bool want_help;           /* -h/--help seen                        */
        bool show_usage;          /* accompany the parse error with usage() */
        int  verbosity;           /* -1 quiet, 0 default, +1 verbose       */
        bool scale_seen;          /* --scale was explicit (the record merge
                                     and the deferred media checks need to
                                     tell an explicit FILL from the default)*/
        bool no_loop_seen;        /* --no-loop was explicit                */
} RevArgs;

/* Scaling modes for media mode (--scale). Scenes keep their own layer
 * `fit` semantics (fill|fit|stretch|tile) — these are deliberately a
 * separate vocabulary for the media path. */
enum {
        REV_SCALE_FILL = 0,      /* cover, crop excess (default)           */
        REV_SCALE_FIT,           /* contain, letterbox/pillarbox           */
        REV_SCALE_STRETCH,       /* fill, ignore aspect                    */
        REV_SCALE_CENTER         /* native size, centered (crops if the
                                    source exceeds the viewport)           */
};

/* Parses argv[1..]. Returns true when the command line is well-formed
 * (including the want_help / list cases — the caller decides what to do).
 * On false, `err` holds the message ("reverie: " prefix and exit are the
 * caller's job) and a.show_usage says whether usage() should follow it
 * (unknown option / missing value: yes; semantic errors: no — matches the
 * historical behavior of each error path). */
bool rev_cli_parse(int argc, char **argv, RevArgs *a, char *err, size_t errcap);

/* "fill" | "fit" | "stretch" | "center" -> REV_SCALE_*; -1 when unknown. */
int rev_scale_mode(const char *name);

/* Validates the --renderer / --backend value vocabulary. Returns the
 * canonical spelling (static storage, do not free) or NULL when the
 * name is unknown — an unknown request is a hard error, never a silent
 * fall-back into auto. "gl" stays accepted as the historical opengl
 * alias. */
const char *rev_cli_canon_renderer(const char *name);
const char *rev_cli_canon_backend(const char *name);

#endif /* REV_CLI_H */
