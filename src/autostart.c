/* ============================================================================
 * Reverie Engine — autostart.c
 * XDG Autostart integration: one .desktop entry + one JSON config, both
 * under XDG_CONFIG_HOME. The Desktop Entry spec is honored where it
 * matters for session managers (Type/Name/Exec/Terminal and shell-style
 * quoting of arguments containing spaces); no distribution-specific
 * extensions beyond the optional GNOME phase hint.
 *
 * Scope decisions (documented in docs/AUTOSTART.md):
 *   - autostart is OPT-IN and user-controlled; the daemon never writes
 *     these files itself
 *   - the Exec line carries --exit-if-running so a session start yields
 *     to an already-running manual instance (the IPC probe)
 *   - no systemd unit is installed; a sample unit lives in the docs
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "autostart.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "util.h"

#define REV_AS_MAX_ARGS 32

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

static bool mkdir_p(const char *path, char *err, size_t errcap)
{
        char tmp[384];
        if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) {
                snprintf(err, errcap, "directory path too long");
                return false;
        }
        size_t len = strlen(tmp);
        if (tmp[len - 1] == '/')
                tmp[len - 1] = 0;
        for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') {
                        *p = 0;
                        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                                snprintf(err, errcap, "mkdir %s failed", tmp);
                                return false;
                        }
                        *p = '/';
                }
        }
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                snprintf(err, errcap, "mkdir %s failed", tmp);
                return false;
        }
        return true;
}

/* ---- JSON argument list (config) -------------------------------------------- */

/* Appends `s` as a JSON string to `buf` (bounded), with the standard
 * escapes. Returns false when it does not fit. */
static bool json_append_string(char *buf, size_t cap, const char *s)
{
        size_t n = strlen(buf);
        if (n + 2 >= cap)
                return false;
        buf[n++] = '"';
        for (const char *c = s; *c; c++) {
                const char *esc = NULL;
                switch (*c) {
                case '"':  esc = "\\\""; break;
                case '\\': esc = "\\\\"; break;
                case '\n': esc = "\\n"; break;
                case '\r': esc = "\\r"; break;
                case '\t': esc = "\\t"; break;
                default: break;
                }
                if (esc) {
                        if (n + 2 >= cap)
                                return false;
                        buf[n++] = esc[0];
                        buf[n++] = esc[1];
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

/* Reads the stored argument list out of the config file into `arena`.
 * Returns the argument count, or -1 with a message in err. */
static int config_read_args(char *argv_out[], int max,
                            char *arena, size_t arena_cap,
                            char *err, size_t errcap)
{
        char path[384];
        char dpath[384];
        if (!rev_autostart_paths(dpath, sizeof dpath, path, sizeof path,
                                 err, errcap))
                return -1;
        size_t len = 0;
        u8 *text = rev_read_file(path, &len);
        if (!text) {
                snprintf(err, errcap, "no stored arguments (run: revtool "
                                      "autostart enable <args...>)");
                return -1;
        }
        RArena *ar = NULL;
        RJVal *root = rev_json_parse((const char *)text, len, &ar,
                                     err, errcap);
        free(text);
        if (!root)
                return -1;
        const RJVal *arr = rev_json_get(root, "args");
        if (!arr || arr->kind != RJ_ARR) {
                snprintf(err, errcap, "config has no args array");
                arena_destroy(ar);
                return -1;
        }
        int n = rev_json_len(arr);
        if (n < 0)
                n = 0;
        if (n > max)
                n = max;
        size_t used = 0;
        for (int i = 0; i < n; i++) {
                const char *s = rev_json_str(arr->items[i], "");
                size_t l = strlen(s) + 1;
                if (used + l > arena_cap) {
                        snprintf(err, errcap, "stored arguments too long");
                        arena_destroy(ar);
                        return -1;
                }
                memcpy(arena + used, s, l);
                argv_out[i] = arena + used;
                used += l;
        }
        arena_destroy(ar);
        return n;
}

/* ---- the .desktop entry ------------------------------------------------------ */

/* Desktop-Entry Exec quoting: arguments containing spaces or quotes are
 * double-quoted with backslash escaping. The field-code characters % and
 * $ are rejected outright — a wallpaper argument is a path/flag, never
 * a specifier the session manager would expand. */
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
        char desktop[384], config[384];
        if (!rev_autostart_paths(desktop, sizeof desktop, config,
                                 sizeof config, err, errcap))
                return false;

        /* nargs == 0: restore the previously stored arguments */
        char *use[REV_AS_MAX_ARGS];
        static char arena[2048];
        if (nargs <= 0) {
                int n = config_read_args(use, REV_AS_MAX_ARGS, arena,
                                         sizeof arena, err, errcap);
                if (n <= 0)
                        return false;
                args = (const char *const *)use;
                nargs = n;
        }
        if (nargs > REV_AS_MAX_ARGS) {
                snprintf(err, errcap, "too many arguments (max %d)",
                         REV_AS_MAX_ARGS);
                return false;
        }

        char bin[380];
        if (!find_reverie(bin, sizeof bin)) {
                snprintf(err, errcap, "cannot locate the reverie binary");
                return false;
        }

        /* the JSON argument record */
        char json[1536];
        snprintf(json, sizeof json, "{\n  \"args\": [");
        for (int i = 0; i < nargs; i++) {
                if (i) {
                        size_t l = strlen(json);
                        if (l + 1 < sizeof json)
                                json[l] = ',', json[l + 1] = 0;
                }
                if (!json_append_string(json, sizeof json, args[i])) {
                        snprintf(err, errcap, "argument too long");
                        return false;
                }
        }
        strncat(json, "]\n}\n", sizeof json - strlen(json) - 1);

        /* the Exec line: <bin> <args...> --exit-if-running */
        char exec[900];
        int w = snprintf(exec, sizeof exec, "%s", bin);
        if (w < 0 || w >= (int)sizeof exec) {
                snprintf(err, errcap, "exec line too long");
                return false;
        }
        for (int i = 0; i < nargs; i++) {
                char quoted[512];
                if (!exec_quote_arg(quoted, sizeof quoted, args[i])) {
                        snprintf(err, errcap,
                                 "argument %d ('%.32s') contains characters "
                                 "the desktop-entry Exec line cannot carry "
                                 "(%% or $)", i + 1, args[i]);
                        return false;
                }
                int add = snprintf(exec + w, sizeof exec - (size_t)w,
                                   " %s", quoted);
                if (add < 0 || (size_t)add >= sizeof exec - (size_t)w) {
                        snprintf(err, errcap, "exec line too long");
                        return false;
                }
                w += add;
        }
        {
                int add = snprintf(exec + w, sizeof exec - (size_t)w,
                                   " --exit-if-running");
                if (add < 0 || (size_t)add >= sizeof exec - (size_t)w) {
                        snprintf(err, errcap, "exec line too long");
                        return false;
                }
        }

        /* both directories, then both files */
        char base[320];
        if (!xdg_config(base, sizeof base, err, errcap))
                return false;
        char dir1[384], dir2[384];
        snprintf(dir1, sizeof dir1, "%s/reverie", base);
        snprintf(dir2, sizeof dir2, "%s/autostart", base);
        if (!mkdir_p(dir1, err, errcap) || !mkdir_p(dir2, err, errcap))
                return false;

        FILE *f = fopen(config, "w");
        if (!f) {
                snprintf(err, errcap, "cannot write %s", config);
                return false;
        }
        fputs(json, f);
        fclose(f);
        chmod(config, 0644);

        f = fopen(desktop, "w");
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
        /* the config file stays: it is the remembered mode */
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
        snprintf(st->config, sizeof st->config, "%s", config);

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

        /* stored arguments (best effort — a missing config is not an
         * error for status) */
        {
                char *av[REV_AS_MAX_ARGS];
                static char arena[2048];
                char e2[128];
                int n = config_read_args(av, REV_AS_MAX_ARGS, arena,
                                         sizeof arena, e2, sizeof e2);
                if (n > 0) {
                        size_t used = 0;
                        for (int i = 0; i < n; i++) {
                                int w = snprintf(st->args + used,
                                                 sizeof st->args - used,
                                                 "%s%s", i ? " " : "", av[i]);
                                if (w < 0 ||
                                    (size_t)w >= sizeof st->args - used)
                                        break;
                                used += (size_t)w;
                        }
                }
        }
        return true;
}
