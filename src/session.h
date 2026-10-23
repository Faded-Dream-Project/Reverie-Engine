/* ============================================================================
 * Reverie Engine — session.h
 * The "last launched wallpaper" memory behind `reverie --restore`.
 *
 * The daemon records what it launched — a theme folder, a .rev scene, a
 * .revpack, a --video file, or a --image file — into $XDG_STATE_HOME/reverie/last-session.json
 * (default ~/.local/state/reverie/last-session.json). The record is the
 * WALLPAPER-DEFINING subset of the command line with the content path made
 * absolute; tool flags (--once/--out/--shot/...) are never recorded, and a
 * --once run does not update the memory at all (a screenshot is not a
 * wallpaper launch). Runtime theme switches (IPC THEME, playlist NEXT/PREV)
 * update the content path and keep everything else.
 *
 * `reverie --restore` replays that record through the REAL argument parser:
 * a tampered or corrupt file cannot inject anything the parser would not
 * accept on a real command line, and a record containing --restore itself
 * is rejected (no replay loops). Explicit flags on the restoring invocation
 * win over the recorded ones; the content always comes from the record.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_SESSION_H
#define REV_SESSION_H

#include "cli.h"

/* Hard limits of the stored record. A wallpaper command line is a handful
 * of flags and one path; anything beyond these is a corrupt or hostile
 * file, not a session. */
#define REV_SESSION_MAX_ARGS 24
#define REV_SESSION_MAX_JSON 8192
#define REV_SESSION_MAX_ARENA 2048

typedef struct {
        char *argv[REV_SESSION_MAX_ARGS]; /* tokens, pointers into `line` */
        int   argc;
        char  line[REV_SESSION_MAX_ARENA]; /* NUL-separated token arena   */
} RevSession;

/* Loads the record into `s` (tokens reference s->line — the caller owns
 * `s` and must keep it alive as long as the argv pointers are used). */
bool rev_session_load(RevSession *s, char *err, size_t errcap);

/* The record's location: $XDG_STATE_HOME/reverie/last-session.json, or
 * ~/.local/state/reverie/last-session.json. Errors (with a message) when
 * neither XDG_STATE_HOME nor HOME is set — there is nowhere to remember. */
bool rev_session_path(char *buf, size_t cap, char *err, size_t errcap);

/* Records the wallpaper-defining subset of a parsed command line (the
 * content path made absolute; tool flags are not representable here at
 * all). --scale is recorded for BOTH media modes (video and image — it
 * is placement, shared vocabulary); --no-loop only for --video, where it
 * means something. Best-effort at the call site: on failure the daemon
 * keeps running and the caller decides how loudly to warn. */
bool rev_session_record(const RevArgs *a, char *err, size_t errcap);

/* Updates ONLY the content of the existing record to `scene_path` (made
 * absolute): the runtime-switch hook. Other recorded flags survive; when
 * the record was a --video one, the media-only flags (--scale/--no-loop)
 * are dropped because the new content is a scene. With no existing record
 * a minimal one is created. */
bool rev_session_record_content(const char *scene_path, char *err, size_t errcap);

/* The --restore implementation: the record must ALREADY be loaded into
 * `s` (rev_session_load — the caller keeps `s` alive because the merged
 * content pointers in `*a` reference s->line). Re-parses the record
 * through rev_cli_parse (rejecting --restore inside the record and records
 * with no content) and merges the result into `a`:
 *   - scene/video/image/scale/no-loop/fps/monitor/renderer/backend come from the
 *     record UNLESS the current invocation set them explicitly;
 *   - tool flags (--once/--out/--shot/--list...) are untouched — they are
 *     properties of THIS invocation, not of the wallpaper;
 *   - a->restore is cleared (the restore is consumed).
 * On false, `err` says why (no record / unreadable / corrupt / the merged
 * line is invalid, e.g. --scale against a scene record). */
bool rev_session_apply(RevArgs *a, RevSession *s, char *err, size_t errcap);

#endif /* REV_SESSION_H */
