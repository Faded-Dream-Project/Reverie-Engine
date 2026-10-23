/* ============================================================================
 * Reverie Engine — autostart.h
 * User-controlled XDG Autostart integration (see autostart.c and
 * docs/AUTOSTART.md). Deliberately small: ONE .desktop entry in
 * $XDG_CONFIG_HOME/autostart whose Exec line is
 *     <reverie binary> --restore --exit-if-running
 * — nothing else. There is no argument memory here anymore: the daemon
 * itself remembers what was last launched (session.h), and the autostart
 * entry simply replays it. Nothing is written unless the user runs
 * `revtool autostart enable`; the daemon never touches these files.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_AUTOSTART_H
#define REV_AUTOSTART_H

#include "rev_types.h"

typedef struct {
        bool enabled;            /* the .desktop entry exists             */
        char desktop[384];       /* .desktop path                         */
        char exec[512];          /* the Exec= line ("" when disabled)     */
        char restores[512];      /* what --restore would replay right now
                                    (the session record, joined; "" when
                                    nothing is recorded)                  */
} RevAutostartStatus;

/* Resolves the .desktop location and the LEGACY config path (the old
 * argument-memory file, $XDG_CONFIG_HOME/reverie/autostart.json — only
 * read to clean it up on disable). Honors XDG_CONFIG_HOME, falls back to
 * $HOME/.config; error when neither exists. Pure path resolution — no
 * filesystem writes. */
bool rev_autostart_paths(char *desktop, size_t dcap,
                         char *config, size_t ccap,
                         char *err, size_t errcap);

/* Writes the .desktop entry: Exec = <reverie binary> --restore
 * --exit-if-running. The arguments MUST be empty (nargs == 0) — autostart
 * restores the last session instead of pinning one; a non-empty list is
 * a hard error with the redirect message, not a silent ignore. The binary
 * is the revtool sibling of /proc/self/exe, falling back to the plain
 * name resolved by the session PATH. */
bool rev_autostart_enable(const char *const *args, int nargs,
                          char *err, size_t errcap);

/* Removes the .desktop entry (autostart off) and, when present, the
 * legacy autostart.json from the old argument-memory mechanism. */
bool rev_autostart_disable(char *err, size_t errcap);

/* Fills `st` from the filesystem: enabled state, the .desktop path, the
 * Exec line, and (best-effort) what the entry would restore right now. */
bool rev_autostart_status(RevAutostartStatus *st, char *err, size_t errcap);

#endif /* REV_AUTOSTART_H */
