/* ============================================================================
 * Reverie Engine — tests/test_cli.c
 * Unit tests for the extracted argument parser (src/cli.c): the historical
 * contract must hold byte-for-byte (positional scenes, --shot legacy form,
 * the NAME=VALUE env-var guard, every error message), plus the new
 * --scene, --exit-if-running, and the media-mode family (--video,
 * --scale, --no-loop) with all its conflict rules.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cli.h"

static int failures;

#define CHECK(cond, msg)                                                \
        do {                                                            \
                if (cond) {                                             \
                        printf("  ok: %s\n", msg);                      \
                } else {                                                \
                        printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
                        failures++;                                     \
                }                                                       \
        } while (0)

/* Builds an argv from a space-separated list of literals (none of the
 * tested arguments contain spaces). argv[0] is a dummy program name. */
static bool parse(const char *spec, RevArgs *a, char *err, size_t errcap,
                  bool *show_usage)
{
        static char *argv[32];
        int argc = 0;
        static char buf[1024];
        snprintf(buf, sizeof buf, "%s", spec);
        argv[argc++] = (char *)"reverie";
        for (char *tok = strtok(buf, " "); tok && argc < 32; tok = strtok(NULL, " "))
                argv[argc++] = tok;
        memset(a, 0xCD, sizeof *a);    /* garbage in: parser must init */
        err[0] = 0;
        bool ok = rev_cli_parse(argc, argv, a, err, errcap);
        *show_usage = a->show_usage;
        return ok;
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);

        char err[256];
        RevArgs a;
        bool usage_hint;

        /* ---- the historical contract ---------------------------------- */

        CHECK(parse("scenes/fluid-aurora.rev", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.scene_path, "scenes/fluid-aurora.rev") && !usage_hint,
              "positional scene is accepted (compat)");

        CHECK(parse("-R vulkan --renderer cpu", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.renderer_pref, "cpu"), "-R then --renderer: last wins");
        CHECK(parse("-B wayland", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.backend_pref, "wayland"), "-B stores the backend pref");
        CHECK(parse("-f 30 -m 1", &a, err, sizeof err, &usage_hint) &&
              a.fps_override == 30 && a.only_monitor == 1,
              "-f and -m parse");
        CHECK(parse("-o -s 1280x720 -O out.png", &a, err, sizeof err, &usage_hint) &&
              a.once && a.out_w == 1280 && a.out_h == 720 &&
              !strcmp(a.out_png, "out.png"), "-o, -s WxH, -O FILE parse");
        CHECK(parse("", &a, err, sizeof err, &usage_hint) &&
              a.out_w == 1920 && a.out_h == 1080 &&
              !strcmp(a.out_png, "reverie.png") && a.scale == REV_SCALE_FILL &&
              a.only_monitor == -1 && a.shot_after < 0 && a.scene_path == NULL,
              "defaults: 1920x1080, reverie.png, scale=fill, no scene");

        CHECK(parse("--shot 3 shot.png", &a, err, sizeof err, &usage_hint) &&
              a.shot_after == 3.0 && !strcmp(a.shot_file, "shot.png"),
              "legacy --shot SECONDS FILE form");
        CHECK(parse("--shot shot.png --shot-after 5", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.shot_file, "shot.png") && a.shot_after == 5.0,
              "--shot FILE + --shot-after N form");

        CHECK(parse("-h", &a, err, sizeof err, &usage_hint) && a.want_help,
              "-h sets want_help (caller prints usage, exits 0)");
        CHECK(parse("-v -q", &a, err, sizeof err, &usage_hint) && a.verbosity == -1,
              "-q after -v: quiet wins (last flag)");
        CHECK(parse("-l", &a, err, sizeof err, &usage_hint) && a.list,
              "--list parses");

        /* ---- rejections, message text included ------------------------ */

        CHECK(!parse("-R", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "needs a value") && usage_hint,
              "missing value: error + usage hint");
        CHECK(!parse("--bogus", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "unknown option '--bogus'") && usage_hint,
              "unknown option: named + usage hint");
        CHECK(!parse("-s 1280", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "bad --size") && !usage_hint,
              "bad --size: message, no usage (historical behavior)");
        CHECK(!parse("a.rev b.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "multiple scenes given"), "two positionals rejected");
        CHECK(!parse("a.rev b.rev", &a, err, sizeof err, &usage_hint),
              "rejection is stable across identical calls");
        CHECK(!parse("REV_VK_VALIDATE=1", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "looks like an environment variable") &&
              strstr(err, "REV_VK_VALIDATE=1"),
              "NAME=VALUE positional: env-var guard");
        CHECK(!parse("a.rev REV_VK_VALIDATE=1", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "a scene was already given"),
              "env-var guard notes the earlier scene");
        CHECK(parse("lower=case.rev", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.scene_path, "lower=case.rev"),
              "lowercase NAME=VALUE is a legal scene path (no guard)");

        /* ---- new: --scene ----------------------------------------------- */

        CHECK(parse("--scene themes/ripple-lake", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.scene_path, "themes/ripple-lake"),
              "--scene PATH sets the scene slot");
        CHECK(!parse("--scene a.rev --scene b.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "multiple scenes given"),
              "--scene twice rejected");
        CHECK(!parse("--scene a.rev positional.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "multiple scenes given"),
              "--scene + positional rejected");
        CHECK(!parse("positional.rev --scene a.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "multiple scenes given"),
              "positional + --scene rejected (order independent)");

        /* ---- new: --exit-if-running ------------------------------------- */

        CHECK(parse("--exit-if-running scenes/snowfall.rev", &a, err, sizeof err, &usage_hint) &&
              a.exit_if_running && !strcmp(a.scene_path, "scenes/snowfall.rev"),
              "--exit-if-running parses alongside a scene");
        CHECK(parse("--exit-if-running", &a, err, sizeof err, &usage_hint) &&
              a.exit_if_running, "--exit-if-running alone parses (caller demands content)");

        /* ---- scale-mode vocabulary (media modes use it later) ----------- */

        CHECK(rev_scale_mode("fill") == REV_SCALE_FILL &&
              rev_scale_mode("fit") == REV_SCALE_FIT &&
              rev_scale_mode("stretch") == REV_SCALE_STRETCH &&
              rev_scale_mode("center") == REV_SCALE_CENTER,
              "scale vocabulary maps");
        CHECK(rev_scale_mode("tile") < 0 && rev_scale_mode("") < 0 &&
              rev_scale_mode(NULL) < 0,
              "unknown scale names rejected (tile stays scene-only)");

        /* ---- media mode: --video / --scale / --no-loop ------------------- */

        CHECK(parse("--video loop.mp4", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.video_path, "loop.mp4") && !a.scene_path &&
              a.scale == REV_SCALE_FILL && !a.no_loop,
              "--video sets the media slot (defaults: fill, loop)");
        CHECK(!parse("--video a.mp4 --video b.mp4", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "multiple videos given"),
              "--video twice rejected");
        CHECK(!parse("--video a.mp4 --scene b.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "--video + --scene rejected");
        CHECK(!parse("--scene b.rev --video a.mp4", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "--scene + --video rejected (order independent)");
        CHECK(!parse("--video a.mp4 b.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "positional after --video rejected");
        CHECK(!parse("b.rev --video a.mp4", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "positional before --video rejected");
        CHECK(!parse("--scale fill", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "--scale applies to --video media mode"),
              "--scale without --video rejected");
        CHECK(!parse("--no-loop", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "--no-loop applies to --video media mode"),
              "--no-loop without --video rejected");
        CHECK(!parse("--video a.mp4 --scale bogus", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "bad --scale (want fill|fit|stretch|center)"),
              "bad --scale value rejected");
        CHECK(parse("--video a.mp4 --scale fit", &a, err, sizeof err, &usage_hint) &&
              a.scale == REV_SCALE_FIT,
              "--scale fit parses next to --video");
        CHECK(parse("--video a.mp4 --scale center --no-loop", &a, err, sizeof err, &usage_hint) &&
              a.scale == REV_SCALE_CENTER && a.no_loop,
              "--scale center + --no-loop parse");
        CHECK(parse("--video a.mp4 --exit-if-running -R cpu", &a, err, sizeof err, &usage_hint) &&
              a.exit_if_running && !strcmp(a.renderer_pref, "cpu"),
              "--video composes with the daemon flags");
        CHECK(!parse("--video a.mp4 REV_VK_VALIDATE=1", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "looks like an environment variable") &&
              strstr(err, "a video was already given"),
              "env-var guard notes the earlier video");

        /* ---- renderer/backend vocabulary ---------------------------------- */

        CHECK(!parse("-R bogus a.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "bad --renderer 'bogus'") &&
              strstr(err, "vulkan | opengl | cpu"),
              "unknown renderer name rejected (was a silent auto before)");
        CHECK(!parse("--backend waylandd a.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "bad --backend 'waylandd'"),
              "unknown backend name rejected");
        CHECK(parse("-R gl a.rev", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.renderer_pref, "gl"),
              "'gl' stays accepted (historical opengl alias)");
        CHECK(parse("-R auto -B x11 a.rev", &a, err, sizeof err, &usage_hint),
              "auto/x11 parse");
        CHECK(rev_cli_canon_renderer("vulkan") && rev_cli_canon_renderer("gl") &&
              !rev_cli_canon_renderer("Vk") && !rev_cli_canon_renderer(NULL) &&
              rev_cli_canon_backend("wayland") && !rev_cli_canon_backend("waylands"),
              "canon validators: exact vocabulary, case-sensitive");

        printf(failures ? "cli: %d FAILURE(S)\n" : "cli: OK\n", failures);
        return failures ? 1 : 0;
}
