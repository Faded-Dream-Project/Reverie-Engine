/* ============================================================================
 * Reverie Engine — tests/test_x11idle.c
 * Unit tests for the X11 display-power state layer used by idle power-save
 * (the REV_X11_DPMS_FILE hook parser + null contracts) and the
 * REV_X11_MON_FILE hotplug monitor-list parser (pure).
 *
 * The full idle cycle (query -> hidden -> frozen clock -> resume) is driven
 * end-to-end under Xvfb by scripts/e2e.sh instead — it needs a live server.
 * The full hotplug reconcile cycle is likewise e2e (e2e.sh hotplug legs).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/x11.h"

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

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);

        char tmp[128];
        snprintf(tmp, sizeof tmp, "/tmp/revmon.%d.txt", (int)getpid());
        RRect m[8];

        /* ---- the hook-file parser: exact levels ------------------------- */
        CHECK(x11_dpms_file_level("on") == 0, "\"on\" parses to level 0");
        CHECK(x11_dpms_file_level("off") == 3, "\"off\" parses to level 3");
        CHECK(x11_dpms_file_level("standby") == 1, "\"standby\" parses to level 1");
        CHECK(x11_dpms_file_level("suspend") == 2, "\"suspend\" parses to level 2");

        /* shell echo writes a trailing newline — the e2e path depends on it */
        CHECK(x11_dpms_file_level("off\n") == 3, "trailing newline tolerated");
        CHECK(x11_dpms_file_level("  on  \n") == 0, "leading/trailing spaces tolerated");
        CHECK(x11_dpms_file_level("\r\nstandby\r\n") == 1, "CRLF tolerated");

        /* rejects junk: a wrong word must never parse as a level */
        CHECK(x11_dpms_file_level("") == -1, "empty text rejected");
        CHECK(x11_dpms_file_level("garbage") == -1, "unknown word rejected");
        CHECK(x11_dpms_file_level("onx") == -1, "prefix-with-tail rejected (onx)");
        CHECK(x11_dpms_file_level("offshore") == -1, "prefix-with-tail rejected (offshore)");
        CHECK(x11_dpms_file_level(NULL) == -1, "NULL rejected");
        CHECK(x11_dpms_file_level("4") == -1, "bare digits rejected (words only)");
        CHECK(x11_dpms_file_level("ON") == -1, "case-sensitive (lowercase only)");

        /* ---- null-connection contracts ----------------------------------- */
        CHECK(x11_display_active(NULL) == -1, "x11_display_active(NULL) = cannot tell");
        CHECK(!x11_display_state_available(NULL), "x11_display_state_available(NULL) = false");

        /* no connection, no hook: unavailable; with the hook configured the
         * availability answer still needs a connection (the hook is read
         * through the X11 object's query path) */
        unsetenv("REV_X11_DPMS_FILE");
        CHECK(!x11_display_state_available(NULL), "no connection, no hook = unavailable");
        setenv("REV_X11_DPMS_FILE", "/nonexistent/rev-dpms", 1);
        CHECK(!x11_display_state_available(NULL), "hook alone does not make NULL available");
        unsetenv("REV_X11_DPMS_FILE");

        /* ---- REV_X11_MON_FILE parser (hotplug e2e hook) ------------------- */
        CHECK(x11_mon_file_parse(NULL, m, 8) == -1, "NULL path rejected");
        CHECK(x11_mon_file_parse("", m, 8) == -1, "empty path rejected");
        CHECK(x11_mon_file_parse("/nonexistent/rev-mon", m, 8) == -1,
              "missing file rejected");

        {
                FILE *f = fopen(tmp, "w");
                CHECK(f != NULL, "mon fixture: empty file writable");
                if (f)
                        fclose(f);
        }
        CHECK(x11_mon_file_parse(tmp, m, 8) == -1, "empty file rejected (no monitors)");

#define MONFIX(content) do {                                           \
                FILE *f = fopen(tmp, "w");                             \
                if (f) { fputs(content, f); fclose(f); }               \
        } while (0)

        MONFIX("0 0 1920 1080\n");
        {
                int n = x11_mon_file_parse(tmp, m, 8);
                CHECK(n == 1, "single monitor parses");
                CHECK(n == 1 && m[0].x == 0 && m[0].y == 0 &&
                      m[0].w == 1920 && m[0].h == 1080,
                      "single monitor values exact");
        }

        MONFIX("# two outputs\n0 0 1920 1080\n1920 0 1280 720\n");
        {
                int n = x11_mon_file_parse(tmp, m, 8);
                CHECK(n == 2, "comment + two monitors parse");
                CHECK(n == 2 && m[1].x == 1920 && m[1].y == 0 &&
                      m[1].w == 1280 && m[1].h == 720,
                      "second monitor values exact");
        }

        MONFIX("  0   0\t640 480 \n");
        {
                int n = x11_mon_file_parse(tmp, m, 8);
                CHECK(n == 1 && m[0].w == 640 && m[0].h == 480,
                      "tabs/spaces tolerated around fields");
        }

        MONFIX("0 0 1920\n");
        CHECK(x11_mon_file_parse(tmp, m, 8) == -1, "3-field line rejected");

        MONFIX("0 0 -100 480\n");
        CHECK(x11_mon_file_parse(tmp, m, 8) == -1, "negative width rejected");

        MONFIX("-5 0 640 480\n");
        CHECK(x11_mon_file_parse(tmp, m, 8) == -1, "negative origin rejected");

        MONFIX("0 0 0 480\n");
        CHECK(x11_mon_file_parse(tmp, m, 8) == -1, "zero height rejected");

        MONFIX("0 0 640 abc\n");
        CHECK(x11_mon_file_parse(tmp, m, 8) == -1, "non-numeric field rejected");

        MONFIX("0 0 640 480\n0 0 800 600\n");
        {
                /* duplicate origins are legal input (the identity collision
                 * is the reconciler's policy problem, not the parser's) */
                int n = x11_mon_file_parse(tmp, m, 8);
                CHECK(n == 2, "duplicate origins parse (identity is caller policy)");
        }

        MONFIX("0 0 70000 480\n");
        CHECK(x11_mon_file_parse(tmp, m, 8) == -1, "u16-overflowing width rejected");

        MONFIX("0 0 640 480\n0 0 640 480\n0 0 640 480\n0 0 640 480\n"
               "0 0 640 480\n0 0 640 480\n0 0 640 480\n0 0 640 480\n"
               "0 0 640 480\n");
        {
                int n = x11_mon_file_parse(tmp, m, 8);
                CHECK(n == 8, "list capped at max (ninth line ignored)");
        }

        unlink(tmp);

        if (failures == 0) {
                printf("x11idle: OK\n");
                return 0;
        }
        printf("x11idle: %d failure(s)\n", failures);
        return 1;
}
