#!/usr/bin/env python3
"""C6 patcher: autostart — Makefile CORE_SRC entry + revtool subcommand.

Both files are TAB-indented; byte-exact python splices.
"""
import sys

# ---- Makefile: add src/autostart.c -----------------------------------------
MK = "Makefile"
with open(MK, "rb") as f:
    mk = f.read().decode("utf-8")
OLD = "CORE_SRC = \\\n\tsrc/arena.c \\\n\tsrc/cli.c \\\n"
NEW = "CORE_SRC = \\\n\tsrc/arena.c \\\n\tsrc/autostart.c \\\n\tsrc/cli.c \\\n"
if "src/autostart.c" in mk:
    print("makefile already patched")
else:
    assert mk.count(OLD) == 1, "Makefile CORE_SRC anchor not found"
    mk = mk.replace(OLD, NEW, 1)
    with open(MK, "wb") as f:
        f.write(mk.encode("utf-8"))
    print("Makefile: src/autostart.c added to CORE_SRC")

# ---- revtool.c: usage + cmd_autostart + dispatch ---------------------------
RT = "src/revtool.c"
with open(RT, "rb") as f:
    rt = f.read().decode("utf-8")

if "cmd_autostart" in rt:
    print("revtool already patched")
    sys.exit(0)

# 1) usage text: add the autostart block after the ipc block
OLD_USAGE = (
    '"  revtool ipc [-s <sock>] <command...>\\n"\n'
    '"      sends one command to a running daemon\'s control socket and prints\\n"\n'
    '"      the reply (exit 0 on OK, 1 on ERR). Commands: PING | STATUS |\\n"\n'
    '"      THEME <path> [fade_ms] | PLAYLIST <p...> | NEXT [fade_ms] |\\n"\n'
    '"      PREV [fade_ms] | ROTATE <sec> | FADE <ms> | QUIT \u2014 see docs/IPC.md\\n");\n'
)
NEW_USAGE = OLD_USAGE[:-len('");\n')] + (
    '"  revtool autostart enable [args...]\\n"\n'
    '"      writes the XDG Autostart entry (XDG_CONFIG_HOME/autostart/\\n"\n'
    '"      reverie.desktop) launching reverie with the given arguments plus\\n"\n'
    '"      --exit-if-running, and remembers them in XDG_CONFIG_HOME/\\n"\n'
    '"      reverie/autostart.json. Without arguments: re-enable the\\n"\n'
    '"      remembered ones. The daemon NEVER writes these files \u2014 consent\\n"\n'
    '"      means running this command; see docs/AUTOSTART.md\\n"\n'
    '"  revtool autostart disable\\n"\n'
    '"      removes the autostart entry (the remembered arguments are kept)\\n"\n'
    '"  revtool autostart status\\n"\n'
    '"      shows enabled/disabled, both file paths, and the stored arguments\\n");\n'
)

# 2) the subcommand implementation before main()
OLD_MAIN = "int main(int argc, char **argv)\n{"
CMD = '''static int cmd_autostart(int argc, char **argv, int first)
{
\tchar err[512] = {0};
\tif (argc < first + 1) {
\t\tusage();
\t\treturn 1;
\t}
\tif (!strcmp(argv[first], "enable")) {
\t\tconst char *const *args = (const char *const *)&argv[first + 1];
\t\tint n = argc - first - 1;
\t\tif (n > 32) {
\t\t\tfprintf(stderr, "revtool: too many autostart arguments (max 32)\\n");
\t\t\treturn 1;
\t\t}
\t\tif (!rev_autostart_enable(args, n, err, sizeof err)) {
\t\t\tfprintf(stderr, "revtool: %s\\n", err);
\t\t\treturn 1;
\t\t}
\t\tRevAutostartStatus st;
\t\tif (rev_autostart_status(&st, err, sizeof err) && st.enabled)
\t\t\tprintf("autostart enabled\\n  exec: %s\\n  config: %s\\n",
\t\t\t       st.exec, st.config);
\t\telse
\t\t\tprintf("autostart enabled\\n");
\t\treturn 0;
\t}
\tif (!strcmp(argv[first], "disable")) {
\t\tif (!rev_autostart_disable(err, sizeof err)) {
\t\t\tfprintf(stderr, "revtool: %s\\n", err);
\t\t\treturn 1;
\t\t}
\t\tprintf("autostart disabled (remembered arguments kept)\\n");
\t\treturn 0;
\t}
\tif (!strcmp(argv[first], "status")) {
\t\tRevAutostartStatus st;
\t\tif (!rev_autostart_status(&st, err, sizeof err)) {
\t\t\tfprintf(stderr, "revtool: %s\\n", err);
\t\t\treturn 1;
\t\t}
\t\tprintf("enabled: %s\\n", st.enabled ? "yes" : "no");
\t\tprintf("desktop: %s\\n", st.desktop);
\t\tprintf("config:  %s\\n", st.config);
\t\tif (st.exec[0])
\t\t\tprintf("exec:    %s\\n", st.exec);
\t\tif (st.args[0])
\t\t\tprintf("args:    %s\\n", st.args);
\t\treturn 0;
\t}
\tfprintf(stderr, "revtool: unknown autostart command '%s' "
\t        "(enable | disable | status)\\n", argv[first]);
\treturn 1;
}

int main(int argc, char **argv)
{'''
assert rt.count(OLD_MAIN) == 1, "revtool main anchor not found"
rt = rt.replace(OLD_MAIN, CMD, 1)

# 3) dispatch: add the autostart branch next to ipc
OLD_DISP = '''\tif (strcmp(cmd, "ipc") == 0 && argc >= 3)
\t\treturn cmd_ipc(argc, argv);'''
NEW_DISP = '''\tif (strcmp(cmd, "ipc") == 0 && argc >= 3)
\t\treturn cmd_ipc(argc, argv);
\tif (strcmp(cmd, "autostart") == 0 && argc >= 3)
\t\treturn cmd_autostart(argc, argv, 2);'''
assert rt.count(OLD_DISP) == 1, "revtool dispatch anchor not found"
rt = rt.replace(OLD_DISP, NEW_DISP, 1)

# 4) include
OLD_INC = '#include "util.h"'
NEW_INC = '#include "autostart.h"\n#include "util.h"'
if NEW_INC not in rt:
    assert rt.count(OLD_INC) >= 1, "revtool include anchor not found"
    rt = rt.replace(OLD_INC, NEW_INC, 1)

with open(RT, "wb") as f:
    f.write(rt.encode("utf-8"))
print("revtool: autostart subcommand wired")
