/* ============================================================================
 * Reverie Engine — cli.c
 * The argument parser (see cli.h). Behavior is byte-compatible with the
 * loop that used to live in main(): same option set, same accept/reject
 * messages, same legacy --shot N FILE form, same NAME=VALUE env-var guard.
 * Added: --scene PATH (the explicit form of the positional), --video PATH
 * (the media mode, mutually exclusive with any scene), --scale
 * fill|fit|stretch|center and --no-loop (media mode only), and
 * --exit-if-running (yield to a live daemon via the IPC probe).
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
        a->video_path = path;
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

        #define NEED_VALUE(opt) do { \
                if (i + 1 >= argc) { \
                        snprintf(err, errcap, "option '%s' needs a value", (opt)); \
                        a->show_usage = true; \
                        return false; \
                } \
        } while (0)

        for (int i = 1; i < argc; i++) {
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
                        NEED_VALUE(argv[i]);
                        set_scene(a, argv[++i], err, errcap);
                        if (err[0])
                                return false;
                } else if (!strcmp(argv[i], "--video")) {
                        NEED_VALUE(argv[i]);
                        set_video(a, argv[++i], err, errcap);
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
                        if (!a->video_path) {
                                snprintf(err, errcap,
                                         "--scale applies to --video media mode");
                                return false;
                        }
                        a->scale = m;
                } else if (!strcmp(argv[i], "--no-loop")) {
                        if (!a->video_path) {
                                snprintf(err, errcap,
                                         "--no-loop applies to --video media mode");
                                return false;
                        }
                        a->no_loop = true;
                } else if (!strcmp(argv[i], "--exit-if-running")) {
                        a->exit_if_running = true;
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
        return true;
}
