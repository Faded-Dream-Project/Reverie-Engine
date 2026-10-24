/* ============================================================================
 * Reverie Engine — cli.c
 * The argument parser (see cli.h). Behavior is byte-compatible with the
 * loop that used to live in main(): same option set, same accept/reject
 * messages, same legacy --shot N FILE form, same NAME=VALUE env-var guard.
 * Added: --scene PATH (the explicit form of the positional), --video PATH
 * (the media mode, mutually exclusive with any scene), --image PATH (the
 * static media mode — native PNG, same --scale vocabulary), --scale
 * fill|fit|stretch|center (video + image; --fill/--fit/--stretch/--center
 * are the valueless shorthands), --no-loop (video only),
 * --exit-if-running (yield to a live daemon via the IPC probe), and
 * --restore (replay the last launched content; the content slot is
 * filled by the session record — an error next to any explicit
 * scene/video/image).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"

static void set_scene(RevArgs *a, const char *path, char *err, size_t errcap)
{
        if (a->scene_path) {
                snprintf(err, errcap, "multiple scenes given");
                return;
        }
        if (a->video_path) {
                snprintf(err, errcap,
                         "conflicting content modes: --video and a scene "
                         "cannot be combined");
                return;
        }
        if (a->image_path) {
                snprintf(err, errcap,
                         "conflicting content modes: --image and a scene "
                         "cannot be combined");
                return;
        }
        a->scene_path = path;
}

static void set_video(RevArgs *a, const char *path, char *err, size_t errcap)
{
        if (a->video_path) {
                snprintf(err, errcap, "multiple videos given");
                return;
        }
        if (a->scene_path) {
                snprintf(err, errcap,
                         "conflicting content modes: --video and a scene "
                         "cannot be combined");
                return;
        }
        if (a->image_path) {
                snprintf(err, errcap,
                         "conflicting content modes: --video and --image "
                         "cannot be combined");
                return;
        }
        a->video_path = path;
}

static void set_image(RevArgs *a, const char *path, char *err, size_t errcap)
{
        if (a->image_path) {
                snprintf(err, errcap, "multiple images given");
                return;
        }
        if (a->scene_path) {
                snprintf(err, errcap,
                         "conflicting content modes: --image and a scene "
                         "cannot be combined");
                return;
        }
        if (a->video_path) {
                snprintf(err, errcap,
                         "conflicting content modes: --image and --video "
                         "cannot be combined");
                return;
        }
        a->image_path = path;
}

int rev_scale_mode(const char *name)
{
        if (!name)
                return -1;
        if (!strcmp(name, "fill"))
                return REV_SCALE_FILL;
        if (!strcmp(name, "fit"))
                return REV_SCALE_FIT;
        if (!strcmp(name, "stretch"))
                return REV_SCALE_STRETCH;
        if (!strcmp(name, "center"))
                return REV_SCALE_CENTER;
        return -1;
}

const char *rev_cli_canon_renderer(const char *name)
{
        if (!name)
                return NULL;
        if (!strcmp(name, "auto") || !strcmp(name, "vulkan") ||
            !strcmp(name, "opengl") || !strcmp(name, "gl") ||
            !strcmp(name, "cpu"))
                return name;
        return NULL;
}

const char *rev_cli_canon_backend(const char *name)
{
        if (!name)
                return NULL;
        if (!strcmp(name, "auto") || !strcmp(name, "x11") ||
            !strcmp(name, "wayland"))
                return name;
        return NULL;
}

/* A token that is itself a long option never starts a value: in
 * `--video --scale stretch` the path was simply forgotten, and eating
 * `--scale` as the path made the parser mis-report a content-mode
 * conflict instead of the real problem. Values may still open with a
 * single '-' (negative numbers — the legacy `--shot -3 out.png` form —
 * or files like "-frame.png"); a file really named `--...` can be
 * passed with an explicit ./ or path prefix. */
static bool option_like(const char *s)
{
        return s[0] == '-' && s[1] == '-';
}

/* Valueless flags that may legally stand between a content option and its
 * path: `--video --stretch clip.mp4` is how the 2026-10 field report
 * spelled it, so the path is taken as deferred and the flag is parsed
 * normally. Options that TAKE values (--scale and friends) do not defer —
 * `--video --scale stretch` is still the forgotten-path error. */
static bool valueless_flag(const char *s)
{
        static const char *const names[] = {
                "--fill", "--fit", "--stretch", "--center",
                "--no-loop", "--exit-if-running",
                "-o", "--once", "-v", "--verbose", "-q", "--quiet",
                "-l", "--list",
        };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
                if (strcmp(s, names[i]) == 0)
                        return true;
        return false;
}

bool rev_cli_parse(int argc, char **argv, RevArgs *a, char *err, size_t errcap)
{
        memset(a, 0, sizeof *a);
        a->only_monitor = -1;
        a->out_w = 1920;
        a->out_h = 1080;
        a->out_png = "reverie.png";
        a->scale = REV_SCALE_FILL;
        a->shot_after = -1;
        err[0] = 0;
        /* deferred content path state: 's'/'v'/'i' while a --scene/--video/
         * --image waits for its path behind valueless flags */
        char pending = 0;
        const char *pending_name = NULL;
        /* Media-mode flags are recorded during the scan (a->scale_seen /
         * a->no_loop_seen — they are parser outputs the session-record merge
         * also needs) but validated once, after the whole line is parsed —
         * see the post-loop checks. */

        #define NEED_VALUE(opt) do { \
                if (i + 1 >= argc || option_like(argv[i + 1])) { \
                        snprintf(err, errcap, "option '%s' needs a value", (opt)); \
                        a->show_usage = true; \
                        return false; \
                } \
        } while (0)

        for (int i = 1; i < argc; i++) {
                /* deferred content path: --video --stretch clip.mp4 set a
                 * pending slot; the first bare positional fills it */
                if (pending && argv[i][0] != '-' && !strchr(argv[i], '=')) {
                        const char *path = argv[i];
                        char kind = pending;
                        pending = 0;
                        if (kind == 'v')
                                set_video(a, path, err, errcap);
                        else if (kind == 'i')
                                set_image(a, path, err, errcap);
                        else
                                set_scene(a, path, err, errcap);
                        if (err[0])
                                return false;
                        continue;
                }
                if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fps")) {
                        NEED_VALUE(argv[i]);
                        a->fps_override = atoi(argv[++i]);
                } else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--monitor")) {
                        NEED_VALUE(argv[i]);
                        a->only_monitor = atoi(argv[++i]);
                } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--once")) {
                        a->once = true;
                } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--size")) {
                        NEED_VALUE(argv[i]);
                        if (sscanf(argv[++i], "%ux%u", &a->out_w, &a->out_h) != 2) {
                                snprintf(err, errcap, "bad --size (want WxH)");
                                return false;
                        }
                } else if (!strcmp(argv[i], "-O") || !strcmp(argv[i], "--out")) {
                        NEED_VALUE(argv[i]);
                        a->out_png = argv[++i];
                } else if (!strcmp(argv[i], "-R") || !strcmp(argv[i], "--renderer")) {
                        NEED_VALUE(argv[i]);
                        a->renderer_pref = argv[++i];
                        if (!rev_cli_canon_renderer(a->renderer_pref)) {
                                snprintf(err, errcap,
                                         "bad --renderer '%s' (want auto | "
                                         "vulkan | opengl | cpu)",
                                         a->renderer_pref);
                                return false;
                        }
                } else if (!strcmp(argv[i], "-B") || !strcmp(argv[i], "--backend")) {
                        NEED_VALUE(argv[i]);
                        a->backend_pref = argv[++i];
                        if (!rev_cli_canon_backend(a->backend_pref)) {
                                snprintf(err, errcap,
                                         "bad --backend '%s' (want auto | "
                                         "x11 | wayland)",
                                         a->backend_pref);
                                return false;
                        }
                } else if (!strcmp(argv[i], "--scene")) {
                        if (i + 1 >= argc || option_like(argv[i + 1])) {
                                if (i + 1 < argc && valueless_flag(argv[i + 1])) {
                                        pending = 's';
                                        pending_name = "--scene";
                                        continue;
                                }
                                snprintf(err, errcap, "option '--scene' needs a value");
                                a->show_usage = true;
                                return false;
                        }
                        set_scene(a, argv[++i], err, errcap);
                        if (err[0])
                                return false;
                } else if (!strcmp(argv[i], "--video")) {
                        if (i + 1 >= argc || option_like(argv[i + 1])) {
                                if (i + 1 < argc && valueless_flag(argv[i + 1])) {
                                        pending = 'v';
                                        pending_name = "--video";
                                        continue;
                                }
                                snprintf(err, errcap, "option '--video' needs a value");
                                a->show_usage = true;
                                return false;
                        }
                        set_video(a, argv[++i], err, errcap);
                        if (err[0])
                                return false;
                } else if (!strcmp(argv[i], "--image")) {
                        if (i + 1 >= argc || option_like(argv[i + 1])) {
                                if (i + 1 < argc && valueless_flag(argv[i + 1])) {
                                        pending = 'i';
                                        pending_name = "--image";
                                        continue;
                                }
                                snprintf(err, errcap, "option '--image' needs a value");
                                a->show_usage = true;
                                return false;
                        }
                        set_image(a, argv[++i], err, errcap);
                        if (err[0])
                                return false;
                } else if (!strcmp(argv[i], "--scale")) {
                        NEED_VALUE(argv[i]);
                        const char *val = argv[++i];
                        int m = rev_scale_mode(val);
                        if (m < 0) {
                                snprintf(err, errcap,
                                         "bad --scale (want fill|fit|stretch|center)");
                                return false;
                        }
                        a->scale = m;
                        a->scale_seen = true;
                } else if (!strcmp(argv[i], "--fill") || !strcmp(argv[i], "--fit") ||
                           !strcmp(argv[i], "--stretch") || !strcmp(argv[i], "--center")) {
                        /* media-mode shorthand: `--stretch` IS `--scale
                         * stretch`. The 2026-10 field report ran
                         * `--video --stretch f.mp4` and died on "option
                         * '--video' needs a value" — the placement modes
                         * were documented but not spellable as flags. */
                        a->scale = rev_scale_mode(argv[i] + 2);
                        a->scale_seen = true;
                } else if (!strcmp(argv[i], "--no-loop")) {
                        a->no_loop = true;
                        a->no_loop_seen = true;
                } else if (!strcmp(argv[i], "--exit-if-running")) {
                        a->exit_if_running = true;
                } else if (!strcmp(argv[i], "--restore")) {
                        a->restore = true;
                } else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list")) {
                        a->list = true;
                } else if (!strcmp(argv[i], "--shot")) {
                        /* two accepted forms:
                         *   --shot SECONDS FILE           (legacy, scripts/e2e.sh)
                         *   --shot FILE [--shot-after N]  (as documented in
                         *                                 docs/BACKENDS.md) */
                        NEED_VALUE(argv[i]);
                        {
                                char *end = NULL;
                                double sec = strtod(argv[i + 1], &end);
                                if (end && *end == 0 && end != argv[i + 1]) {
                                        /* numeric first arg: legacy form */
                                        if (i + 2 >= argc) {
                                                snprintf(err, errcap,
                                                         "option '--shot' needs "
                                                         "SECONDS and FILE");
                                                a->show_usage = true;
                                                return false;
                                        }
                                        a->shot_after = sec;
                                        a->shot_file = argv[i + 2];
                                        i += 2;
                                } else {
                                        a->shot_file = argv[i + 1];
                                        i += 1;
                                }
                        }
                } else if (!strcmp(argv[i], "--shot-after")) {
                        NEED_VALUE(argv[i]);
                        a->shot_after = atof(argv[++i]);
                } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
                        a->verbosity = 1;
                } else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
                        a->verbosity = -1;
                } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
                        a->want_help = true;
                        return true;
                } else if (argv[i][0] == '-' && argv[i][1]) {
                        snprintf(err, errcap, "unknown option '%s'", argv[i]);
                        a->show_usage = true;
                        return false;
                } else if (strchr(argv[i], '=') && argv[i][0] != '-' &&
                           argv[i][0] != '/') {
                        /* a NAME=VALUE positional is almost always an env var
                         * that belongs BEFORE the command (REV_VK_VALIDATE=1
                         * ./build/reverie ...), never a scene path */
                        const char *eq = strchr(argv[i], '=');
                        bool name_ok = eq != argv[i];
                        for (const char *c = argv[i]; c < eq && name_ok; c++)
                                if (!((*c >= 'A' && *c <= 'Z') ||
                                      (*c >= '0' && *c <= '9') || *c == '_'))
                                        name_ok = false;
                        if (name_ok) {
                                snprintf(err, errcap,
                                         "'%.*s' looks like an environment "
                                         "variable — put it before the "
                                         "command, e.g. %s ./build/reverie ...%s",
                                         (int)(eq - argv[i] + 1), argv[i], argv[i],
                                         a->scene_path
                                                 ? " (a scene was already given)"
                                         : a->video_path
                                                 ? " (a video was already given)"
                                                 : "");
                                return false;
                        }
                        set_scene(a, argv[i], err, errcap);
                        if (err[0])
                                return false;
                } else {
                        set_scene(a, argv[i], err, errcap);
                        if (err[0])
                                return false;
                }
        }
        if (pending) {
                /* a deferred content option that never met its path */
                snprintf(err, errcap, "option '%s' needs a value", pending_name);
                a->show_usage = true;
                return false;
        }
        /* Media-only flags are rejected here, once the whole line is known:
         * `--scale stretch --video f.mp4` is the same command as
         * `--video f.mp4 --scale stretch`, and the old inline check made
         * the order significant for no reason. Orphaned flags (no --video
         * or --image anywhere) keep the historical error messages. With
         * --restore the media may be arriving from the session record —
         * the check is deferred to rev_session_apply(), which re-runs it
         * once the merged line is known.
         * --scale applies to BOTH media modes (video and image — placement
         * is the same problem); --no-loop is --video-only because a static
         * image has no loop to decline. */
        if (a->scale_seen && !a->video_path && !a->image_path && !a->restore) {
                snprintf(err, errcap, "--scale applies to --video or --image");
                return false;
        }
        if (a->no_loop_seen && !a->video_path && !a->restore) {
                snprintf(err, errcap, "--no-loop applies to --video media mode");
                return false;
        }
        /* --restore IS the content: the record supplies it. Combining it
         * with an explicit scene/video/image/positional is ambiguous (which
         * one should win?) and therefore a hard error, not a silent choice. */
        if (a->restore && (a->scene_path || a->video_path || a->image_path)) {
                snprintf(err, errcap, "--restore replays the last launched "
                                     "content and cannot be combined with "
                                     "%s",
                         a->video_path
                                 ? "--video"
                                 : a->image_path
                                           ? "--image"
                                           : a->scene_path && a->scene_path[0] == '-'
                                                     ? "--scene"
                                                     : "a scene path");
                return false;
        }
        return true;
}
