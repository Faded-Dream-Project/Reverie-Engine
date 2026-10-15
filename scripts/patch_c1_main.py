#!/usr/bin/env python3
"""C1 patcher: replace main()'s inline argument loop with rev_cli_parse().

Line-anchored splice on src/main.c (space-indented file — safe for tools).
Anchors verified before writing; refuses to patch a moved target.
"""
import sys

PATH = "src/main.c"

with open(PATH, "rb") as f:
    lines = f.read().decode("utf-8").split("\n")

# lines[] is 0-based; file line N == lines[N-1]
START = 580          # 'int main(int argc, char **argv)'
END = 696            # closing '}' of the backend env fallback

assert lines[START - 1] == "int main(int argc, char **argv)", lines[START - 1]
assert lines[END - 1] == "        }", repr(lines[END - 1])
assert "REV_BACKEND" in lines[END - 4], lines[END - 4]
assert "for (int i = 1; i < argc; i++) {" in lines[593], lines[593]

NEW = """int main(int argc, char **argv)
{
        char err[512] = { 0 };
        RevArgs a;
        if (!rev_cli_parse(argc, argv, &a, err, sizeof err)) {
                fprintf(stderr, "reverie: %s\\n", err);
                if (a.show_usage)
                        usage();
                return 1;
        }
        if (a.want_help) {
                usage();
                return 0;
        }
        if (a.verbosity > 0)
                rev_log_set_level(REV_LOG_DEBUG);
        else if (a.verbosity < 0)
                rev_log_set_level(REV_LOG_ERROR);

        /* locals carried from the historical parser so the rest of main()
         * stays untouched; cli.c now owns parsing + validation */
        const char *scene_path = a.scene_path;
        const char *renderer_pref = a.renderer_pref;
        const char *backend_pref = a.backend_pref;
        int fps_override = a.fps_override;
        int only_monitor = a.only_monitor;
        bool once = a.once, list = a.list;
        f64 shot_after = a.shot_after;
        const char *shot_file = a.shot_file;
        u32 out_w = a.out_w, out_h = a.out_h;
        const char *out_png = a.out_png;

        if (!renderer_pref) {
                renderer_pref = getenv("REV_RENDERER");
                if (!renderer_pref || !*renderer_pref)
                        renderer_pref = "auto";
        }
        if (!backend_pref) {
                backend_pref = getenv("REV_BACKEND");
                if (!backend_pref || !*backend_pref)
                        backend_pref = "auto";
        }

        /* --exit-if-running: the duplicate-instance guard the autostart
         * entry carries. Probe the per-user control socket BEFORE any
         * heavy setup — a live daemon means this launch yields cleanly
         * instead of stacking a second set of wallpaper windows. (The
         * probe needs the IPC socket; REV_IPC=0 removes the probe point.) */
        if (a.exit_if_running && ipc_daemon_live()) {
                LOGI("another reverie daemon owns the control socket — "
                     "exiting (--exit-if-running)");
                return 0;
        }""".split("\n")

out = lines[:START - 1] + NEW + lines[END:]

with open(PATH, "wb") as f:
    f.write("\n".join(out).encode("utf-8"))
print("patched: main() prologue replaced (%d -> %d lines)"
      % (END - START + 1, len(NEW)))
