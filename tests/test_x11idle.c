/* ============================================================================
 * Reverie Engine — tests/test_x11idle.c
 * Unit tests for the X11 display-power state layer used by idle power-save:
 * the REV_X11_DPMS_FILE hook parser (pure) and the null/absent-connection
 * contracts of x11_display_active / x11_display_state_available.
 *
 * The full idle cycle (query -> hidden -> frozen clock -> resume) is driven
 * end-to-end under Xvfb by scripts/e2e.sh instead — it needs a live server.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

        if (failures == 0) {
                printf("x11idle: OK\n");
                return 0;
        }
        printf("x11idle: %d failure(s)\n", failures);
        return 1;
}
