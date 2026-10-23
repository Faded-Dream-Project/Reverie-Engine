/* ============================================================================
 * Reverie Engine — tests/test_session.c
 * Unit tests for the last-launched memory (src/session.c): the record's
 * location under XDG_STATE_HOME, recording a parsed command line (absolute
 * content path, canonical flags, tool flags absent by construction), the
 * runtime-switch content update (other flags carried, media flags dropped),
 * and the --restore merge (record fills, explicit current flags win,
 * tool flags untouched, corrupt/hostile records rejected through the real
 * parser — including a record containing --restore itself). The --image
 * static media mode gets the same legs: it records --image + --scale, and
 * restores through the identical replay path.
 *
 * Every leg points XDG_STATE_HOME at a fresh temp directory; the last leg
 * checks the no-HOME-no-XDG refusal.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/cli.h"
#include "../src/json.h"
#include "../src/session.h"
#include "../src/util.h"

static int failures;

#define CHECK(cond, msg)                                                \
        do {                                                            \
                if (cond) {                                             \
                        printf("  ok: %s\n", msg);                      \
                } else {                                                \
                        printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
                        failures++;                                     \
                }                                                        \
        } while (0)

static char xdg[256];

static void fresh_xdg(void)
{
        char tmpl[] = "/tmp/revsession_XXXXXX";
        char *d = mkdtemp(tmpl);
        snprintf(xdg, sizeof xdg, "%s", d ? d : "/tmp/revsession_fallback");
        setenv("XDG_STATE_HOME", xdg, 1);
}

/* The process's ORIGINAL cwd: relative-path recording resolves against
 * it everywhere except the chdir'd scene leg, which captures its own.
 * Returns are checked, not discarded — Ubuntu's GCC 13 (fortify on)
 * marks getcwd/chdir warn_unused_result and -Werror makes an ignored
 * return a build break; GCC 14 relaxed the warning, which is how the
 * statements compiled clean locally while failing on the runner. */
static char cwd[512];

static bool record_cwd(void)
{
        return getcwd(cwd, sizeof cwd) != NULL;
}

static char scwd[512];   /* the chdir'd sandbox leg's cwd */

/* Builds a parsed RevArgs the way main() would see it for `reverie ...`.
 * Tokens are PIPE-separated so a single token may contain spaces (a theme
 * path like "themes/my theme" is exactly the case worth testing). */
static bool parse_spec(const char *spec, RevArgs *a, char *err, size_t errcap)
{
        static char *argv[32];
        static char buf[1024];
        int argc = 0;
        snprintf(buf, sizeof buf, "%s", spec);
        argv[argc++] = (char *)"reverie";
        for (char *tok = strtok(buf, "|"); tok && argc < 32;
             tok = strtok(NULL, "|"))
                argv[argc++] = tok;
        return rev_cli_parse(argc, argv, a, err, errcap);
}

/* Reads the record's argv tokens back through the loader. */
static bool load_tokens(RevSession *s, char *err, size_t errcap)
{
        return rev_session_load(s, err, errcap);
}

static bool tok_eq(const RevSession *s, int idx, const char *want)
{
        return idx < s->argc && !strcmp(s->argv[idx], want);
}

static bool write_record(const char *json)
{
        char dir[384];
        snprintf(dir, sizeof dir, "%s/reverie", xdg);
        if (!rev_mkdir_p(dir))
                return false;
        char path[384], err[128];
        if (!rev_session_path(path, sizeof path, err, sizeof err))
                return false;
        return rev_write_file(path, json, strlen(json));
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);
        CHECK(record_cwd(), "original cwd captured for the relative legs");
        char err[512] = { 0 };
        RevArgs a;
        RevSession s;

        /* ---- path resolution honors XDG_STATE_HOME -------------------- */
        fresh_xdg();
        {
                char path[384];
                CHECK(rev_session_path(path, sizeof path, err, sizeof err),
                      "record path resolves under XDG_STATE_HOME");
                CHECK(strstr(path, xdg) == path &&
                      strstr(path, "/reverie/last-session.json") != NULL,
                      "record path shape: <state>/reverie/last-session.json");
        }

        /* ---- recording a scene launch ---------------------------------- */
        fresh_xdg();
        {
                char tmpl2[] = "/tmp/revsession_cwd_XXXXXX";
                char *d2 = mkdtemp(tmpl2);
                CHECK(d2 != NULL, "cwd sandbox created");
                if (d2) {
                        if (chdir(d2) != 0)
                                CHECK(false, "chdir into cwd sandbox");
                        CHECK(getcwd(scwd, sizeof scwd) != NULL,
                              "sandbox cwd captured");
                        CHECK(parse_spec("--scene|themes/my theme", &a,
                                         err, sizeof err) &&
                              rev_session_record(&a, err, sizeof err),
                              "scene launch records successfully");
                        CHECK(load_tokens(&s, err, sizeof err) && s.argc == 2,
                              "record holds the canonical pair");
                        CHECK(tok_eq(&s, 0, "--scene"),
                              "positional/--scene normalizes to --scene");
                        char want[1024];
                        snprintf(want, sizeof want, "%s/themes/my theme", scwd);
                        CHECK(tok_eq(&s, 1, want),
                              "relative path recorded ABSOLUTE (cwd-joined), "
                              "space intact");
                        if (chdir(cwd) != 0)
                                CHECK(false, "original cwd restored after leg");
                }
        }

        /* ---- recording a media launch: the wallpaper flags -------------- */
        fresh_xdg();
        {
                CHECK(parse_spec("--video|clips/waves.mp4|--scale|fit|"
                                 "--no-loop|-f|24|-m|1|-R|cpu",
                                 &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "media launch records successfully");
                CHECK(load_tokens(&s, err, sizeof err) && s.argc == 11,
                      "record holds content + all wallpaper flags");
                char want[600];
                snprintf(want, sizeof want, "%s/clips/waves.mp4", cwd);
                CHECK(tok_eq(&s, 0, "--video") && tok_eq(&s, 1, want),
                      "video content recorded absolute");
                CHECK(tok_eq(&s, 2, "--scale") && tok_eq(&s, 3, "fit") &&
                      tok_eq(&s, 4, "--no-loop") &&
                      tok_eq(&s, 5, "--fps") && tok_eq(&s, 6, "24") &&
                      tok_eq(&s, 7, "--monitor") && tok_eq(&s, 8, "1") &&
                      tok_eq(&s, 9, "--renderer") && tok_eq(&s, 10, "cpu"),
                      "scale/no-loop/fps/monitor/renderer recorded");
        }

        /* ---- recording a static media (--image) launch ----------------- */
        fresh_xdg();
        {
                CHECK(parse_spec("--image|walls/dusk.png|--scale|center|"
                                 "-m|0|-R|cpu",
                                 &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "image launch records successfully");
                CHECK(load_tokens(&s, err, sizeof err) && s.argc == 8,
                      "image record holds content + its wallpaper flags");
                char want[600];
                snprintf(want, sizeof want, "%s/walls/dusk.png", cwd);
                CHECK(tok_eq(&s, 0, "--image") && tok_eq(&s, 1, want),
                      "image content recorded absolute under --image");
                CHECK(tok_eq(&s, 2, "--scale") && tok_eq(&s, 3, "center") &&
                      tok_eq(&s, 4, "--monitor") && tok_eq(&s, 5, "0") &&
                      tok_eq(&s, 6, "--renderer") && tok_eq(&s, 7, "cpu"),
                      "--scale is recorded for images (shared placement)");
                int has_no_loop = 0;
                for (int i = 0; i < s.argc; i++)
                        if (!strcmp(s.argv[i], "--no-loop"))
                                has_no_loop = 1;
                CHECK(!has_no_loop,
                      "--no-loop never recorded for a static image");
        }

        /* ---- defaults are not recorded; tool flags are not representable -- */
        fresh_xdg();
        {
                CHECK(parse_spec("themes/x", &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "bare scene records");
                CHECK(load_tokens(&s, err, sizeof err) && s.argc == 2,
                      "nothing beyond the content pair: defaults (fill "
                      "scale, no fps/monitor/renderer/backend) stay silent");
                /* a screenshot invocation: out_png/shot live in RevArgs but
                 * the recorder has no tokens for them at all */
                CHECK(parse_spec("--scene|themes/y|-O|shot.png|--shot|p.png",
                                 &a, err, sizeof err),
                      "tool flags parse");
                CHECK(rev_session_record(&a, err, sizeof err) &&
                      load_tokens(&s, err, sizeof err) && s.argc == 2,
                      "no --once/--out/--shot can ever appear in the record");
        }

        /* ---- restore: no record is a clean, named error ------------------ */
        fresh_xdg();
        {
                CHECK(parse_spec("--restore", &a, err, sizeof err),
                      "--restore parses");
                char e2[256];
                RevArgs ra = a;
                CHECK(!rev_session_apply(&ra, &s, e2, sizeof e2) &&
                      strstr(e2, "nothing to restore") != NULL &&
                      strstr(e2, "last-session.json") != NULL,
                      "missing record: error names the reason and the path");
        }

        /* ---- restore: the roundtrip merge -------------------------------- */
        fresh_xdg();
        {
                CHECK(parse_spec("--video|/abs/w.mp4|--scale|fit|--no-loop|"
                                 "-f|30|-m|0|-R|vulkan|-B|wayland",
                                 &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "record the full-fidelity launch");
                CHECK(parse_spec("--restore|--once|-O|shot.png", &a, err,
                                 sizeof err),
                      "restoring invocation with tool flags parses");
                CHECK(rev_session_apply(&a, &s, err, sizeof err),
                      "apply merges the record");
                CHECK(a.video_path && !strcmp(a.video_path, "/abs/w.mp4"),
                      "content comes from the record");
                CHECK(a.scale == REV_SCALE_FIT && a.no_loop &&
                      a.fps_override == 30 && a.only_monitor == 0 &&
                      a.renderer_pref && !strcmp(a.renderer_pref, "vulkan") &&
                      a.backend_pref && !strcmp(a.backend_pref, "wayland"),
                      "wallpaper flags come from the record");
                CHECK(a.once && a.out_png && !strcmp(a.out_png, "shot.png") &&
                      !a.restore,
                      "tool flags survive untouched; restore is consumed");
        }

        /* ---- restore: the --image roundtrip ------------------------------ */
        fresh_xdg();
        {
                CHECK(parse_spec("--image|/abs/wall.png|--scale|fit|-f|12",
                                 &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "record the image launch");
                CHECK(parse_spec("--restore|--once|-O|shot.png", &a, err,
                                 sizeof err),
                      "restoring invocation with tool flags parses");
                CHECK(rev_session_apply(&a, &s, err, sizeof err),
                      "apply merges the image record");
                CHECK(a.image_path && !strcmp(a.image_path, "/abs/wall.png") &&
                      !a.scene_path && !a.video_path,
                      "image content comes from the record");
                CHECK(a.scale == REV_SCALE_FIT && a.fps_override == 12 &&
                      !a.no_loop,
                      "image wallpaper flags come from the record");
        }

        /* ---- restore: --scale against an IMAGE record is legal; --no-loop
         * is not (video-only) ------------------------------------------------ */
        fresh_xdg();
        {
                CHECK(parse_spec("--image|/abs/wall.png|--scale|fit",
                                 &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "record the image launch");
                CHECK(parse_spec("--restore|--scale|stretch", &a, err,
                                 sizeof err),
                      "--restore --scale parses (deferred)");
                CHECK(rev_session_apply(&a, &s, err, sizeof err) &&
                      a.scale == REV_SCALE_STRETCH,
                      "explicit --scale against an image record wins");
                CHECK(parse_spec("--restore|--no-loop", &a, err, sizeof err),
                      "--restore --no-loop parses (deferred)");
                char e2[256];
                RevArgs ra = a;
                CHECK(!rev_session_apply(&ra, &s, e2, sizeof e2) &&
                      strstr(e2, "--no-loop applies to --video media mode")
                              != NULL,
                      "--no-loop against an image record fails at apply");
        }

        /* ---- restore: explicit current flags win -------------------------- */
        fresh_xdg();
        {
                CHECK(parse_spec("--video|/abs/w.mp4|--scale|fit|--no-loop|"
                                 "-R|cpu|-f|60",
                                 &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "record with distinct values");
                CHECK(parse_spec("--restore|-R|opengl|--scale|stretch",
                                 &a, err, sizeof err),
                      "current invocation overrides some flags");
                CHECK(rev_session_apply(&a, &s, err, sizeof err),
                      "apply with overrides succeeds");
                CHECK(a.renderer_pref && !strcmp(a.renderer_pref, "opengl") &&
                      a.scale == REV_SCALE_STRETCH,
                      "explicit current renderer/scale WIN");
                CHECK(a.no_loop && a.fps_override == 60,
                      "unspecified fields still come from the record");
        }

        /* ---- restore: media flags against a SCENE record ------------------ */
        fresh_xdg();
        {
                CHECK(parse_spec("themes/s", &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "record a scene");
                CHECK(parse_spec("--restore|--scale|fit", &a, err, sizeof err),
                      "--restore --scale parses (deferred)");
                char e2[256];
                CHECK(!rev_session_apply(&a, &s, e2, sizeof e2) &&
                      strstr(e2, "--scale applies to --video or --image")
                              != NULL,
                      "--scale against a scene record fails at apply");
        }

        /* ---- hostile/corrupt records go through the real parser ----------- */
        fresh_xdg();
        {
                CHECK(write_record(
                              "{\"version\":1,\"argv\":[\"--restore\"]}"),
                      "plant a record containing --restore");
                CHECK(parse_spec("--restore", &a, err, sizeof err),
                      "--restore parses");
                char e2[256];
                CHECK(!rev_session_apply(&a, &s, e2, sizeof e2) &&
                      strstr(e2, "contains --restore") != NULL,
                      "a record containing --restore is rejected (no loops)");
        }
        fresh_xdg();
        {
                CHECK(write_record(
                              "{\"version\":1,\"argv\":[\"-R\",\"cpu\"]}"),
                      "plant a record with no content");
                CHECK(!rev_session_apply(&a, &s, err, sizeof err) &&
                      strstr(err, "no content") != NULL,
                      "content-less record rejected");
        }
        fresh_xdg();
        {
                CHECK(write_record("{\"version\":2,\"argv\":[\"--scene\",\"/x\"]}"),
                      "plant a future-version record");
                CHECK(!rev_session_apply(&a, &s, err, sizeof err) &&
                      strstr(err, "version") != NULL,
                      "unknown record version rejected");
        }
        fresh_xdg();
        {
                CHECK(write_record("this is not json at all"),
                      "plant garbage");
                CHECK(!rev_session_apply(&a, &s, err, sizeof err),
                      "non-JSON record rejected (parser error surfaced)");
        }
        fresh_xdg();
        {
                CHECK(write_record("{\"version\":1,\"argv\":[\"--scene\","
                                   "\"/x\",\"--scale\",\"fit\"]}"),
                      "plant a scene record with media flags (hand-edited)");
                CHECK(!rev_session_apply(&a, &s, err, sizeof err) &&
                      strstr(err, "--scale applies to --video or --image") != NULL,
                      "scene record carrying --scale fails the re-parse");
        }
        fresh_xdg();
        {
                CHECK(write_record(
                              "{\"version\":1,\"argv\":[\"--scene\",\"/x\","
                              "\"--once\",\"--out\",\"/tmp/evil.png\"]}"),
                      "plant tool flags inside the record");
                CHECK(rev_session_apply(&a, &s, err, sizeof err) &&
                      !a.once && a.out_png && strcmp(a.out_png, "/tmp/evil.png"),
                      "tool flags in the record are INERT (merge never "
                      "copies them)");
        }
        fresh_xdg();
        {
                char big[9 * 1024];
                memset(big, 'x', sizeof big - 1);
                big[sizeof big - 1] = 0;
                CHECK(write_record(big), "plant an oversized record");
                CHECK(!rev_session_apply(&a, &s, err, sizeof err) &&
                      strstr(err, "too large") != NULL,
                      "oversized record refused before parsing");
        }

        /* ---- runtime switch: content update, flags carried ---------------- */
        fresh_xdg();
        {
                CHECK(parse_spec("--video|/abs/w.mp4|--scale|fit|-f|30",
                                 &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "record a media launch");
                CHECK(rev_session_record_content("themes/new one", err,
                                                 sizeof err),
                      "switch the content");
                CHECK(load_tokens(&s, err, sizeof err) && s.argc == 4,
                      "switch record: content pair + carried fps");
                char want[600];
                snprintf(want, sizeof want, "%s/themes/new one", cwd);
                CHECK(tok_eq(&s, 0, "--scene") && tok_eq(&s, 1, want),
                      "switch rewrites the content as an absolute --scene");
                CHECK(tok_eq(&s, 2, "--fps") && tok_eq(&s, 3, "30"),
                      "non-content flags are carried");
                int has_scale = 0;
                for (int i = 0; i < s.argc; i++)
                        if (!strcmp(s.argv[i], "--scale") ||
                            !strcmp(s.argv[i], "--no-loop"))
                                has_scale = 1;
                CHECK(!has_scale,
                      "media-only flags dropped when the content became a "
                      "scene (the record must re-parse)");
        }
        fresh_xdg();
        {
                /* no prior record: a switch still creates a minimal one */
                CHECK(rev_session_record_content("themes/first", err,
                                                 sizeof err),
                      "switch with no prior record creates one");
                CHECK(load_tokens(&s, err, sizeof err) && s.argc == 2 &&
                      tok_eq(&s, 0, "--scene"),
                      "minimal switch record is the content pair alone");
        }

        /* ---- atomicity: no .tmp litter, overwrite works -------------------- */
        fresh_xdg();
        {
                CHECK(parse_spec("themes/a", &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err) &&
                      parse_spec("themes/b", &a, err, sizeof err) &&
                      rev_session_record(&a, err, sizeof err),
                      "two consecutive records");
                char path[384], tmp[400], e2[128];
                CHECK(rev_session_path(path, sizeof path, e2, sizeof e2),
                      "path resolves");
                snprintf(tmp, sizeof tmp, "%s.tmp", path);
                CHECK(access(tmp, F_OK) != 0,
                      "no .tmp litter left behind (atomic rename)");
                CHECK(load_tokens(&s, err, sizeof err) && s.argc == 2,
                      "second record replaced the first cleanly");
        }

        /* ---- no HOME and no XDG_STATE_HOME is a clean error ---------------- */
        {
                const char *ox = getenv("XDG_STATE_HOME");
                const char *oh = getenv("HOME");
                unsetenv("XDG_STATE_HOME");
                unsetenv("HOME");
                char path[384];
                CHECK(!rev_session_path(path, sizeof path, err, sizeof err) &&
                      strstr(err, "nowhere to remember") != NULL,
                      "missing HOME/XDG_STATE is a clean refusal");
                CHECK(parse_spec("themes/x", &a, err, sizeof err),
                      "parse still works");
                CHECK(!rev_session_record(&a, err, sizeof err),
                      "record refuses without a state location");
                if (ox) setenv("XDG_STATE_HOME", ox, 1);
                if (oh) setenv("HOME", oh, 1);
        }

        printf(failures ? "session: %d FAILURE(S)\n" : "session: OK\n",
               failures);
        return failures ? 1 : 0;
}
