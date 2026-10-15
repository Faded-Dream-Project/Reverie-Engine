/* ============================================================================
 * Reverie Engine — tests/test_autostart.c
 * Unit tests for the XDG Autostart integration (src/autostart.c):
 * enable/disable/status, path resolution under XDG_CONFIG_HOME, the
 * Exec-line quoting rules (spaces quoted, field codes rejected), the
 * JSON argument roundtrip, and the remembered-arguments semantics.
 *
 * Each leg points XDG_CONFIG_HOME at a fresh temp directory; the last
 * leg also checks the no-HOME-no-XDG refusal. The live session behavior
 * (a session manager actually launching the entry) is covered by
 * docs/AUTOSTART.md's manual steps — it needs a real login session.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/autostart.h"
#include "../src/json.h"
#include "../src/util.h"

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

static char xdg[256];

static void fresh_xdg(void)
{
        /* a fresh template per call: glibc mkdtemp requires the trailing
         * XXXXXX — a shared (static) template silently fails on the second
         * call and every leg after that would share one directory */
        char tmpl[] = "/tmp/revautostart_XXXXXX";
        char *d = mkdtemp(tmpl);
        snprintf(xdg, sizeof xdg, "%s", d ? d : "/tmp/revautostart_fallback");
        setenv("XDG_CONFIG_HOME", xdg, 1);
}

static bool file_has(const char *path, const char *needle)
{
        size_t len = 0;
        u8 *t = rev_read_file(path, &len);
        if (!t)
                return false;
        bool found = strstr((const char *)t, needle) != NULL;
        free(t);
        return found;
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);
        char err[512] = { 0 };

        /* ---- path resolution honors XDG_CONFIG_HOME -------------------- */
        fresh_xdg();
        {
                char desk[384], cfg[384];
                CHECK(rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                          err, sizeof err),
                      "paths resolve under XDG_CONFIG_HOME");
                CHECK(!strcmp(desk, "/autostart/reverie.desktop") +
                              strlen(xdg) - strlen(desk) == 0 ||
                      strstr(desk, xdg) == desk,
                      "desktop path sits under XDG_CONFIG_HOME");
                CHECK(strstr(desk, "/autostart/reverie.desktop") != NULL &&
                      strstr(cfg, "/reverie/autostart.json") != NULL,
                      "path shapes match the XDG Autostart convention");
        }

        /* ---- enable: both files, Exec contract, JSON roundtrip ---------- */
        {
                const char *args[] = { "--scene", "themes/ripple lake",
                                       "--scale", "fit" };
                CHECK(rev_autostart_enable(args, 4, err, sizeof err),
                      "enable with a spaced path succeeds");
                char desk[384], cfg[384];
                rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                    err, sizeof err);
                CHECK(access(desk, R_OK) == 0, ".desktop entry written");
                CHECK(access(cfg, R_OK) == 0, "config written");

                CHECK(file_has(desk, "[Desktop Entry]") &&
                      file_has(desk, "Type=Application") &&
                      file_has(desk, "Terminal=false"),
                      "desktop entry carries the required keys");
                CHECK(file_has(desk, "--exit-if-running"),
                      "Exec line carries the duplicate-instance guard");
                CHECK(file_has(desk, "\"themes/ripple lake\""),
                      "spaced argument is quoted in the Exec line");

                /* config JSON parses and round-trips exactly */
                size_t len = 0;
                u8 *text = rev_read_file(cfg, &len);
                RArena *ar = NULL;
                RJVal *root = text ? rev_json_parse((const char *)text, len,
                                                    &ar, err, sizeof err)
                                   : NULL;
                CHECK(root != NULL, "config is valid JSON");
                if (root) {
                        const RJVal *arr = rev_json_get(root, "args");
                        CHECK(arr && rev_json_len(arr) == 4,
                              "config stores all four arguments");
                        CHECK(arr && !strcmp(rev_json_str(arr->items[1], ""),
                                             "themes/ripple lake"),
                              "spaced argument survives the JSON roundtrip");
                        arena_destroy(ar);
                }
                free(text);
        }

        /* ---- status reflects enable -------------------------------------- */
        {
                RevAutostartStatus st;
                CHECK(rev_autostart_status(&st, err, sizeof err),
                      "status succeeds");
                CHECK(st.enabled, "status reports enabled");
                CHECK(strstr(st.exec, "--exit-if-running") != NULL,
                      "status exposes the Exec line");
                CHECK(!strcmp(st.args,
                              "--scene themes/ripple lake --scale fit"),
                      "status exposes the stored arguments");
        }

        /* ---- re-enable without arguments restores the remembered ones --- */
        {
                /* disable first (entry removed, config kept) */
                CHECK(rev_autostart_disable(err, sizeof err),
                      "disable removes the entry");
                char desk[384], cfg[384];
                rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                    err, sizeof err);
                CHECK(access(desk, R_OK) != 0, "entry gone after disable");
                CHECK(access(cfg, R_OK) == 0, "config retained (memory)");

                CHECK(rev_autostart_enable(NULL, 0, err, sizeof err),
                      "enable without arguments restores the stored ones");
                CHECK(file_has(desk, "\"themes/ripple lake\"") &&
                      file_has(desk, "--exit-if-running"),
                      "restored Exec line matches the remembered arguments");
        }

        /* ---- fresh location: enable without arguments and no config ----- */
        fresh_xdg();
        {
                CHECK(!rev_autostart_enable(NULL, 0, err, sizeof err) &&
                      strstr(err, "no stored arguments") != NULL,
                      "enable without arguments needs a prior config");
        }

        /* ---- field codes are refused -------------------------------------- */
        {
                const char *bad[] = { "50%loop.mp4" };
                CHECK(!rev_autostart_enable(bad, 1, err, sizeof err) &&
                      strstr(err, "cannot carry") != NULL,
                      "percent field code rejected");
                const char *bad2[] = { "a$b.mp4" };
                CHECK(!rev_autostart_enable(bad2, 1, err, sizeof err),
                      "dollar field code rejected");
        }

        /* ---- media-mode arguments work end to end ------------------------ */
        fresh_xdg();
        {
                const char *args[] = { "--video", "/media/waves.mp4",
                                       "--scale", "fill", "--no-loop" };
                CHECK(rev_autostart_enable(args, 5, err, sizeof err),
                      "media-mode enable succeeds");
                char desk[384], cfg[384];
                rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                    err, sizeof err);
                CHECK(file_has(desk, "--video /media/waves.mp4") &&
                      file_has(desk, "--no-loop") &&
                      file_has(desk, "--exit-if-running"),
                      "media Exec line carries the mode and the guard");
        }

        /* ---- no HOME and no XDG_CONFIG_HOME is a clean error ------------- */
        {
                const char *ox = getenv("XDG_CONFIG_HOME");
                const char *oh = getenv("HOME");
                unsetenv("XDG_CONFIG_HOME");
                unsetenv("HOME");
                char desk[8], cfg[8];
                CHECK(!rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                           err, sizeof err) &&
                      strstr(err, "nowhere") != NULL,
                      "missing HOME/XDG is a clean refusal");
                const char *args[] = { "x.rev" };
                CHECK(!rev_autostart_enable(args, 1, err, sizeof err),
                      "enable refuses without a config location");
                /* restore for any later legs */
                if (ox) setenv("XDG_CONFIG_HOME", ox, 1);
                if (oh) setenv("HOME", oh, 1);
        }

        printf(failures ? "autostart: %d FAILURE(S)\n" : "autostart: OK\n",
               failures);
        return failures ? 1 : 0;
}
