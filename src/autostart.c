/* ============================================================================
 * Reverie Engine — autostart.c
 * XDG Autostart integration: ONE .desktop entry under XDG_CONFIG_HOME whose
 * Exec line is `<reverie> --restore --exit-if-running`. Autostart does not
 * remember anything itself — the daemon's session record (session.h) is the
 * memory, so "start on login" and "what was last launched" can never drift
 * apart. The Desktop Entry spec is honored where it matters for session
 * managers (Type/Name/Exec/Terminal); no distribution-specific extensions
 * beyond the optional GNOME phase hint.
 *
 * Scope decisions (documented in docs/AUTOSTART.md):
 *   - autostart is OPT-IN and user-controlled; the daemon never writes
 *     these files itself
 *   - the Exec line carries --exit-if-running so a session start yields
 *     to an already-running manual instance (the IPC probe)
 *   - the entry carries --restore, never a pinned path: re-running the
 *     wallpaper you last launched is one `revtool autostart enable` away
 *     with zero arguments to remember
 *   - no systemd unit is installed; a sample unit lives in the docs
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "autostart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "session.h"
#include "util.h"

/* ---- path resolution ------------------------------------------------------- */

static bool xdg_config(char *buf, size_t cap, char *err, size_t errcap)
{
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) {
                if (snprintf(buf, cap, "%s", xdg) >= (int)cap)
                        goto too_long;
                return true;
        }
        const char *home = getenv("HOME");
        if (!home || !*home) {
                snprintf(err, errcap, "no XDG_CONFIG_HOME and no HOME — "
                                      "nowhere to put the autostart entry");
                return false;
        }
        if (snprintf(buf, cap, "%s/.config", home) >= (int)cap)
                goto too_long;
        return true;
too_long:
        snprintf(err, errcap, "configuration path too long");
        return false;
}

bool rev_autostart_paths(char *desktop, size_t dcap,
                         char *config, size_t ccap,
                         char *err, size_t errcap)
{
        char base[320];
        if (!xdg_config(base, sizeof base, err, errcap))
                return false;
        if (snprintf(desktop, dcap, "%s/autostart/reverie.desktop", base)
                    >= (int)dcap ||
            snprintf(config, ccap, "%s/reverie/autostart.json", base)
                    >= (int)ccap) {
                snprintf(err, errcap, "autostart path too long");
                return false;
        }
        return true;
}

/* ---- the .desktop entry ------------------------------------------------------ */

/* Desktop-Entry Exec quoting: the Exec value is a fixed pair of flags plus
 * the binary path, and only the BINARY can carry spaces (an install under
 * "C:\Program Files"-style paths). Quoting stays spec-shaped: double
 * quotes with backslash escaping. */
static bool exec_quote_arg(char *buf, size_t cap, const char *arg)
{
        bool need_quote = false;
        for (const char *c = arg; *c; c++) {
                if (*c == ' ' || *c == '\t' || *c == '"' || *c == '\'' ||
                    *c == '\\' || *c == '>' || *c == '<')
                        need_quote = true;
                if (*c == '%' || *c == '$')
                        return false;    /* reserved: refuse, never expand */
        }
        if (!need_quote)
                return snprintf(buf, cap, "%s", arg) < (int)cap;
        size_t n = 0;
        if (cap < 3)
                return false;
        buf[n++] = '"';
        for (const char *c = arg; *c; c++) {
                if (*c == '"' || *c == '\\') {
                        if (n + 2 >= cap)
                                return false;
                        buf[n++] = '\\';
                        buf[n++] = *c;
                } else {
                        if (n + 1 >= cap)
                                return false;
                        buf[n++] = *c;
                }
        }
        if (n + 2 >= cap)
                return false;
        buf[n++] = '"';
        buf[n] = 0;
        return true;
}

static bool find_reverie(char *out, size_t cap)
{
        char self[380];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n > 0) {
                self[n] = 0;
                char *slash = strrchr(self, '/');
                if (slash) {
                        int w = snprintf(out, cap, "%.*s/reverie",
                                         (int)(slash - self), self);
                        if (w > 0 && w < (int)cap && access(out, X_OK) == 0)
                                return true;
                }
        }
        /* fall back to the session PATH at login time */
        return snprintf(out, cap, "reverie") < (int)cap;
}

/* ---- enable / disable / status ---------------------------------------------- */

bool rev_autostart_enable(const char *const *args, int nargs,
                          char *err, size_t errcap)
{
        (void)args;   /* the rejection below is nargs-only: the redirect
                         message never echoes what it refuses to store */
        if (nargs > 0) {
                snprintf(err, errcap,
                         "autostart restores the last session and stores no "
                         "arguments — launch reverie once with your theme or "
                         "video, then run 'revtool autostart enable' with no "
                         "arguments");
                return false;
        }
        char desktop[384], config[384];
        if (!rev_autostart_paths(desktop, sizeof desktop, config,
                                 sizeof config, err, errcap))
                return false;

        char bin[380];
        if (!find_reverie(bin, sizeof bin)) {
                snprintf(err, errcap, "cannot locate the reverie binary");
                return false;
        }
        char qbin[400];
        if (!exec_quote_arg(qbin, sizeof qbin, bin)) {
                snprintf(err, errcap, "binary path '%s' contains characters "
                                      "the desktop-entry Exec line cannot "
                                      "carry (%% or $)", bin);
                return false;
        }

        /* the Exec line: <bin> --restore --exit-if-running. --restore is
         * the whole point (replay the last session); --exit-if-running is
         * the duplicate-instance guard. */
        char exec[512];
        int w = snprintf(exec, sizeof exec, "%s --restore --exit-if-running",
                         qbin);
        if (w < 0 || (size_t)w >= sizeof exec) {
                snprintf(err, errcap, "exec line too long");
                return false;
        }

        char base[320];
        if (!xdg_config(base, sizeof base, err, errcap))
                return false;
        char dir[384];
        snprintf(dir, sizeof dir, "%s/autostart", base);
        if (!rev_mkdir_p(dir)) {
                snprintf(err, errcap, "cannot create %s", dir);
                return false;
        }

        FILE *f = fopen(desktop, "w");
        if (!f) {
                snprintf(err, errcap, "cannot write %s", desktop);
                return false;
        }
        fprintf(f,
                "[Desktop Entry]\n"
                "Type=Application\n"
                "Name=Reverie\n"
                "Comment=Interactive wallpaper engine\n"
                "Exec=%s\n"
                "Terminal=false\n"
                "X-GNOME-Autostart-Phase=Applications\n",
                exec);
        fclose(f);
        chmod(desktop, 0644);
        return true;
}

bool rev_autostart_disable(char *err, size_t errcap)
{
        char desktop[384], config[384];
        if (!rev_autostart_paths(desktop, sizeof desktop, config,
                                 sizeof config, err, errcap))
                return false;
        if (unlink(desktop) != 0) {
                snprintf(err, errcap, "no autostart entry to remove (%s)",
                         desktop);
                return false;
        }
        /* the legacy argument-memory file from the old mechanism: gone for
         * good when the user turns autostart off (it is not consulted by
         * anything anymore; leaving it would be a lie on disk) */
        (void)unlink(config);
        return true;
}

bool rev_autostart_status(RevAutostartStatus *st, char *err, size_t errcap)
{
        memset(st, 0, sizeof *st);
        char desktop[384], config[384];
        if (!rev_autostart_paths(desktop, sizeof desktop, config,
                                 sizeof config, err, errcap))
                return false;
        snprintf(st->desktop, sizeof st->desktop, "%s", desktop);

        FILE *f = fopen(desktop, "r");
        if (f) {
                st->enabled = true;
                char line[768];
                while (fgets(line, sizeof line, f)) {
                        if (!strncmp(line, "Exec=", 5)) {
                                line[strcspn(line, "\r\n")] = 0;
                                snprintf(st->exec, sizeof st->exec,
                                         "%.*s", (int)sizeof st->exec - 1,
                                         line + 5);
                        }
                }
                fclose(f);
        }

        /* what --restore would replay right now (best effort — a missing
         * record is not an error, it is the honest answer "nothing yet") */
        RevSession s;
        char e2[192];
        if (rev_session_load(&s, e2, sizeof e2)) {
                size_t used = 0;
                for (int i = 0; i < s.argc; i++) {
                        int w = snprintf(st->restores + used,
                                         sizeof st->restores - used,
                                         "%s%s", i ? " " : "", s.argv[i]);
                        if (w < 0 ||
                            (size_t)w >= sizeof st->restores - used)
                                break;
                        used += (size_t)w;
                }
        }
        return true;
}
