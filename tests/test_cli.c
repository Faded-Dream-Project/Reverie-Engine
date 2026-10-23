/* ============================================================================
 * Reverie Engine — tests/test_cli.c
 * Unit tests for the extracted argument parser (src/cli.c): the historical
 * contract must hold byte-for-byte (positional scenes, --shot legacy form,
 * the NAME=VALUE env-var guard, every error message), plus the new
 * --scene, --exit-if-running, the media-mode family (--video,
 * --scale, --no-loop) with all its conflict rules, and --restore
 * (the session-record content slot: deferred media checks, conflict
 * with any explicit content).
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

        CHECK(parse("themes/fluid-aurora/scene.rev", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.scene_path, "themes/fluid-aurora/scene.rev") && !usage_hint,
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

        CHECK(parse("--exit-if-running themes/snowfall/scene.rev", &a, err, sizeof err, &usage_hint) &&
              a.exit_if_running && !strcmp(a.scene_path, "themes/snowfall/scene.rev"),
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

        /* ---- option-like tokens are never values ------------------------- */
        /* `--video --scale stretch` used to eat `--scale` as the path and
         * mis-report a content-mode conflict (the scene was "stretch") */
        CHECK(!parse("--video --scale stretch", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "option '--video' needs a value") && usage_hint,
              "the forgotten-path command names its real problem");
        CHECK(!parse("--scene --video a.mp4", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "option '--scene' needs a value") && usage_hint,
              "option-like token is not a --scene value");
        CHECK(!parse("--video a.mp4 --scale", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "needs a value"),
              "trailing --scale still needs a value");
        CHECK(!parse("-R --video a.mp4", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "option '-R' needs a value") && usage_hint,
              "short option forms are protected too");
        CHECK(parse("--scene -frame.rev", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.scene_path, "-frame.rev"),
              "single-dash value stays a legal scene path");
        CHECK(parse("--shot -3 out.png", &a, err, sizeof err, &usage_hint) &&
              a.shot_after == -3.0 && !strcmp(a.shot_file, "out.png"),
              "legacy --shot with negative seconds still parses");
        CHECK(!parse("--scale fill", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "--scale applies to --video or --image"),
              "--scale without --video/--image rejected");
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
        CHECK(parse("--scale stretch --video a.mp4", &a, err, sizeof err, &usage_hint) &&
              a.scale == REV_SCALE_STRETCH && !strcmp(a.video_path, "a.mp4") &&
              !a.scene_path,
              "--scale before --video parses (position independent)");
        CHECK(parse("--no-loop --scale fit --video a.mp4", &a, err, sizeof err, &usage_hint) &&
              a.no_loop && a.scale == REV_SCALE_FIT,
              "--no-loop + --scale both before --video parse");
        CHECK(!parse("--scale fill --scene a.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "--scale applies to --video or --image"),
              "--scale still rejected in scene mode (post-parse check)");
        CHECK(parse("--video a.mp4 --exit-if-running -R cpu", &a, err, sizeof err, &usage_hint) &&
              a.exit_if_running && !strcmp(a.renderer_pref, "cpu"),
              "--video composes with the daemon flags");
        CHECK(!parse("--video a.mp4 REV_VK_VALIDATE=1", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "looks like an environment variable") &&
              strstr(err, "a video was already given"),
              "env-var guard notes the earlier video");

        /* ---- static media mode: --image (native PNG, same --scale) ------ */

        CHECK(parse("--image wall.png", &a, err, sizeof err, &usage_hint) &&
              !strcmp(a.image_path, "wall.png") && !a.scene_path &&
              !a.video_path && a.scale == REV_SCALE_FILL,
              "--image sets the static media slot (default fill)");
        CHECK(!parse("--image a.png --image b.png", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "multiple images given"),
              "--image twice rejected");
        CHECK(!parse("--image a.png --scene b.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "--image + --scene rejected");
        CHECK(!parse("--scene b.rev --image a.png", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "--scene + --image rejected (order independent)");
        CHECK(!parse("--image a.png --video b.mp4", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "--image + --video rejected");
        CHECK(!parse("--video b.mp4 --image a.png", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "--video + --image rejected (order independent)");
        CHECK(!parse("--image a.png positional.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "conflicting content modes"),
              "positional after --image rejected");
        CHECK(!parse("--image --scale fit", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "option '--image' needs a value") && usage_hint,
              "--image forgotten path names its real problem");
        CHECK(parse("--image a.png --scale fit", &a, err, sizeof err, &usage_hint) &&
              a.scale == REV_SCALE_FIT,
              "--scale fit parses next to --image");
        CHECK(parse("--scale center --image a.png", &a, err, sizeof err, &usage_hint) &&
              a.scale == REV_SCALE_CENTER && !strcmp(a.image_path, "a.png"),
              "--scale before --image parses (position independent)");
        CHECK(!parse("--image a.png --no-loop", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "--no-loop applies to --video media mode"),
              "--no-loop is --video-only (a static image has no loop)");
        CHECK(parse("--image a.png --exit-if-running -R cpu", &a, err, sizeof err, &usage_hint) &&
              a.exit_if_running && !strcmp(a.renderer_pref, "cpu"),
              "--image composes with the daemon flags");

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

        /* ---- --restore (the session-record content slot) ------------------ */

        CHECK(parse("--restore", &a, err, sizeof err, &usage_hint) &&
              a.restore && !a.scene_path && !a.video_path && !a.image_path,
              "--restore parses and claims the content slot");
        CHECK(parse("--restore --exit-if-running", &a, err, sizeof err, &usage_hint) &&
              a.restore && a.exit_if_running,
              "--restore composes with the daemon flags");
        CHECK(parse("--restore --once", &a, err, sizeof err, &usage_hint) &&
              a.restore && a.once,
              "--restore composes with --once (screenshot the last wallpaper)");
        CHECK(parse("--restore --scale fit", &a, err, sizeof err, &usage_hint) &&
              a.restore && a.scale == REV_SCALE_FIT && a.scale_seen,
              "--restore defers the --scale media check (content not known yet)");
        CHECK(parse("--restore --no-loop", &a, err, sizeof err, &usage_hint) &&
              a.restore && a.no_loop_seen,
              "--restore defers the --no-loop media check");
        CHECK(!parse("--restore --scene a.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "--restore replays the last launched content") &&
              strstr(err, "cannot be combined with"),
              "--restore + --scene rejected");
        CHECK(!parse("--restore --video a.mp4", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "cannot be combined with --video"),
              "--restore + --video rejected (message names --video)");
        CHECK(!parse("--restore --image a.png", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "cannot be combined with --image"),
              "--restore + --image rejected (message names --image)");
        CHECK(!parse("--restore a.rev", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "cannot be combined with"),
              "--restore + positional rejected");
        CHECK(!parse("a.rev --restore", &a, err, sizeof err, &usage_hint) &&
              strstr(err, "cannot be combined with"),
              "positional + --restore rejected (order independent)");
        CHECK(parse("--restore --restore", &a, err, sizeof err, &usage_hint) &&
              a.restore,
              "a repeated --restore is idempotent, not an error");
        CHECK(parse("--video a.mp4 --scale fit", &a, err, sizeof err, &usage_hint) &&
              a.scale_seen && a.scale == REV_SCALE_FIT,
              "scale_seen output marks an explicit --scale");
        CHECK(parse("--video a.mp4", &a, err, sizeof err, &usage_hint) &&
              !a.scale_seen && !a.no_loop_seen,
              "scale_seen/no_loop_seen stay false when unset (record merge key)");
        CHECK(parse("--video a.mp4 --scale fill", &a, err, sizeof err, &usage_hint) &&
              a.scale_seen && a.scale == REV_SCALE_FILL,
              "an explicit --scale fill is seen even though it equals the default");

        printf(failures ? "cli: %d FAILURE(S)\n" : "cli: OK\n", failures);
        return failures ? 1 : 0;
}
