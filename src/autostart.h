/* ============================================================================
 * Reverie Engine — autostart.h
 * User-controlled XDG Autostart integration (see autostart.c and
 * docs/AUTOSTART.md). Deliberately small: a .desktop entry in
 * $XDG_CONFIG_HOME/autostart plus a JSON record of the daemon arguments
 * in $XDG_CONFIG_HOME/reverie/autostart.json. Nothing is written unless
 * the user runs `revtool autostart enable ...`; the daemon itself never
 * touches these files.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_AUTOSTART_H
#define REV_AUTOSTART_H

#include "rev_types.h"

typedef struct {
        bool enabled;            /* the .desktop entry exists             */
        char desktop[384];       /* .desktop path                         */
        char config[384];        /* config path                           */
        char exec[512];          /* the Exec= line ("" when disabled)     */
        char args[512];          /* stored argument list ("" when none)   */
} RevAutostartStatus;

/* Resolves the two file locations (honors XDG_CONFIG_HOME, falls back to
 * $HOME/.config; error when neither exists). Pure path resolution — no
 * filesystem writes. */
bool rev_autostart_paths(char *desktop, size_t dcap,
                         char *config, size_t ccap,
                         char *err, size_t errcap);

/* Writes the config (the argument list, JSON) and the .desktop entry
 * whose Exec line launches the reverie binary with those arguments plus
 * --exit-if-running (the duplicate-instance guard: a login session that
 * starts while a manual instance already runs must not stack a second
 * wallpaper). The binary is the revtool sibling of /proc/self/exe,
 * falling back to the plain name resolved by the session PATH.
 * nargs == 0 restores the last stored arguments when a config already
 * exists, and errors when it does not. */
bool rev_autostart_enable(const char *const *args, int nargs,
                          char *err, size_t errcap);

/* Removes the .desktop entry (autostart off). The config file is kept —
 * it is the remembered mode/settings, re-used by the next enable. */
bool rev_autostart_disable(char *err, size_t errcap);

/* Fills `st` from the filesystem: enabled state, both paths, the Exec
 * line, and the stored arguments. */
bool rev_autostart_status(RevAutostartStatus *st, char *err, size_t errcap);

#endif /* REV_AUTOSTART_H */
