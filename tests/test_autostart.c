/* ============================================================================
 * Reverie Engine — tests/test_autostart.c
 * Unit tests for the XDG Autostart integration (src/autostart.c) under
 * the --restore contract: enable writes ONE .desktop entry whose Exec
 * line is "<reverie> --restore --exit-if-running" and nothing else; there
 * is no argument memory (arguments to enable are refused with the
 * redirect message); disable removes the entry and cleans up the legacy
 * autostart.json; status reports the entry and (via the session record)
 * what --restore would replay right now.
 *
 * Each leg points XDG_CONFIG_HOME (and, where it matters,
 * XDG_STATE_HOME) at fresh temp directories; the last leg checks the
 * no-HOME-no-XDG refusal. The live session behavior (a session manager
 * actually launching the entry) is covered by docs/AUTOSTART.md's manual
 * steps — it needs a real login session.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/autostart.h"
#include "../src/cli.h"
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
static char state[320];

static void fresh_xdg(void)
{
        char tmpl[] = "/tmp/revautostart_XXXXXX";
        char *d = mkdtemp(tmpl);
        snprintf(xdg, sizeof xdg, "%s", d ? d : "/tmp/revautostart_fallback");
        setenv("XDG_CONFIG_HOME", xdg, 1);
        /* isolate the session record too: status must be deterministic */
        snprintf(state, sizeof state, "%s/state", xdg);
        setenv("XDG_STATE_HOME", state, 1);
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

/* Records a fake "last launched wallpaper" so status has something to
 * show (the real recording is covered by tests/test_session.c). */
static bool plant_session(const char *spec)
{
        static char *argv[16];
        static char buf[256];
        int argc = 0;
        snprintf(buf, sizeof buf, "%s", spec);
        argv[argc++] = (char *)"reverie";
        for (char *tok = strtok(buf, "|"); tok && argc < 16;
             tok = strtok(NULL, "|"))
                argv[argc++] = tok;
        RevArgs a;
        char err[192];
        if (!rev_cli_parse(argc, argv, &a, err, sizeof err))
                return false;
        return rev_session_record(&a, err, sizeof err);
}

int main(void)
{
        setvbuf(stdout, NULL, _IONBF, 0);
        char err[512] = { 0 };

        /* ---- path resolution honors XDG_CONFIG_HOME -------------------- */
        fresh_xdg();
        {
                char desk[384], cfg[384];
                CHECK(rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                          err, sizeof err),
                      "paths resolve under XDG_CONFIG_HOME");
                CHECK(strstr(desk, xdg) == desk &&
                      strstr(desk, "/autostart/reverie.desktop") != NULL,
                      "desktop path sits under XDG_CONFIG_HOME");
                CHECK(strstr(cfg, "/reverie/autostart.json") != NULL,
                      "legacy config path keeps its historical shape");
        }

        /* ---- enable: one entry, Exec = --restore --exit-if-running ------ */
        {
                CHECK(rev_autostart_enable(NULL, 0, err, sizeof err),
                      "enable without arguments succeeds");
                char desk[384], cfg[384];
                rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                    err, sizeof err);
                CHECK(access(desk, R_OK) == 0, ".desktop entry written");
                CHECK(access(cfg, F_OK) != 0,
                      "no argument-memory config is written anymore");
                CHECK(file_has(desk, "[Desktop Entry]") &&
                      file_has(desk, "Type=Application") &&
                      file_has(desk, "Terminal=false"),
                      "desktop entry carries the required keys");
                CHECK(file_has(desk, "--restore") &&
                      file_has(desk, "--exit-if-running"),
                      "Exec line restores the session and carries the "
                      "duplicate-instance guard");
                /* the Exec line is exactly <bin> --restore --exit-if-running:
                 * no stored theme, no stored video, nothing to drift */
                CHECK(!file_has(desk, "--scene") && !file_has(desk, "--video"),
                      "Exec line pins no content — the record owns it");
        }

        /* ---- enable with arguments: the redirect, not a silent ignore --- */
        {
                const char *args[] = { "--scene", "themes/ripple lake" };
                CHECK(!rev_autostart_enable(args, 2, err, sizeof err) &&
                      strstr(err, "restores the last session") != NULL &&
                      strstr(err, "no arguments") != NULL,
                      "arguments are refused with the redirect message");
        }

        /* ---- status: entry + what --restore would replay ------------------ */
        {
                RevAutostartStatus st;
                CHECK(rev_autostart_status(&st, err, sizeof err),
                      "status succeeds");
                CHECK(st.enabled, "status reports enabled");
                CHECK(strstr(st.exec, "--restore") != NULL &&
                      strstr(st.exec, "--exit-if-running") != NULL,
                      "status exposes the Exec line");
                CHECK(!st.restores[0],
                      "nothing recorded yet: restores is empty (honest)");
        }
        {
                CHECK(plant_session("--scene|/home/u/themes/my theme"),
                      "plant a last-launched session");
                RevAutostartStatus st;
                CHECK(rev_autostart_status(&st, err, sizeof err),
                      "status succeeds with a record present");
                CHECK(strstr(st.restores, "--scene") != NULL &&
                      strstr(st.restores, "/home/u/themes/my theme") != NULL,
                      "status shows WHAT --restore would replay");
        }

        /* ---- disable: entry gone, legacy memory cleaned -------------------- */
        {
                CHECK(rev_autostart_disable(err, sizeof err),
                      "disable removes the entry");
                char desk[384], cfg[384];
                rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                    err, sizeof err);
                CHECK(access(desk, F_OK) != 0, "entry gone after disable");
                CHECK(access(cfg, F_OK) != 0,
                      "no legacy config left behind either");
                CHECK(!rev_autostart_disable(err, sizeof err) &&
                      strstr(err, "no autostart entry") != NULL,
                      "disable without an entry is a clean error");
        }

        /* ---- disable cleans up the OLD mechanism's leftover file ---------- */
        fresh_xdg();
        {
                CHECK(rev_autostart_enable(NULL, 0, err, sizeof err),
                      "enable for the legacy-cleanup leg");
                char desk[384], cfg[384];
                rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                    err, sizeof err);
                /* a leftover from the pre---restore autostart (argument memory) */
                rev_mkdir_p("/tmp/rev_legacy_dir");
                char revdir[384];
                snprintf(revdir, sizeof revdir, "%s/reverie", xdg);
                rev_mkdir_p(revdir);
                rev_write_file(cfg, "{\"args\":[\"--scene\",\"old.rev\"]}",
                               strlen("{\"args\":[\"--scene\",\"old.rev\"]}"));
                CHECK(access(cfg, F_OK) == 0, "legacy config planted");
                CHECK(rev_autostart_disable(err, sizeof err),
                      "disable succeeds with the legacy file present");
                CHECK(access(desk, F_OK) != 0 && access(cfg, F_OK) != 0,
                      "disable removed BOTH the entry and the legacy memory");
        }

        /* ---- the session record survives enable/disable cycles ------------- */
        fresh_xdg();
        {
                CHECK(plant_session("--scene|/t/x"), "plant a session");
                CHECK(rev_autostart_enable(NULL, 0, err, sizeof err) &&
                      rev_autostart_disable(err, sizeof err) &&
                      rev_autostart_enable(NULL, 0, err, sizeof err),
                      "enable/disable/enable round-trips");
                RevAutostartStatus st;
                CHECK(rev_autostart_status(&st, err, sizeof err) &&
                      st.enabled &&
                      strstr(st.restores, "/t/x") != NULL,
                      "the record is the memory — it outlives autostart "
                      "state by design");
        }

        /* ---- no HOME and no XDG_CONFIG_HOME is a clean error ------------- */
        {
                const char *ox = getenv("XDG_CONFIG_HOME");
                const char *os = getenv("XDG_STATE_HOME");
                const char *oh = getenv("HOME");
                unsetenv("XDG_CONFIG_HOME");
                unsetenv("XDG_STATE_HOME");
                unsetenv("HOME");
                char desk[8], cfg[8];
                CHECK(!rev_autostart_paths(desk, sizeof desk, cfg, sizeof cfg,
                                           err, sizeof err) &&
                      strstr(err, "nowhere") != NULL,
                      "missing HOME/XDG is a clean refusal");
                CHECK(!rev_autostart_enable(NULL, 0, err, sizeof err),
                      "enable refuses without a config location");
                /* restore for any later legs */
                if (ox) setenv("XDG_CONFIG_HOME", ox, 1);
                if (os) setenv("XDG_STATE_HOME", os, 1);
                if (oh) setenv("HOME", oh, 1);
        }

        printf(failures ? "autostart: %d FAILURE(S)\n" : "autostart: OK\n",
               failures);
        return failures ? 1 : 0;
}
