#!/bin/sh
# run-egl-probe.sh — starts weston headless (same recipe as e2e-wayland.sh),
# builds + runs scripts/egl_wayland_probe.c against it, prints results.
cd "$(dirname "$0")/.." || exit 1
GPU_ROOT="${REV_GPU_ROOT:-$(cd .. && pwd)/gpulibs/root}"
WESTON="$GPU_ROOT/usr/bin/weston"
LIB="$GPU_ROOT/usr/lib/x86_64-linux-gnu"
[ -x "$WESTON" ] || { echo "SKIP (no weston stack)"; exit 0; }

OUT=$(mktemp -d)
XDG=$(mktemp -d); chmod 700 "$XDG"

cat > "$OUT/libexec_shim.c" <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char *wet_get_libexec_path(const char *name)
{
    char path[512];
    const char *root = getenv("REV_GPU_ROOT");
    snprintf(path, sizeof path, "%s/usr/libexec/%s",
             (root && *root) ? root : "/nonexistent", name);
    return strdup(path);
}
EOF
cc -shared -fPIC -o "$OUT/libexec_shim.so" "$OUT/libexec_shim.c" || exit 1

REV_GPU_ROOT="$GPU_ROOT" \
LD_LIBRARY_PATH="$LIB:$LIB/libweston-14:$LIB/weston" \
XDG_RUNTIME_DIR="$XDG" \
XDG_DATA_HOME="$GPU_ROOT/usr/share" \
LD_PRELOAD="$OUT/libexec_shim.so" \
WESTON_MODULE_MAP="headless-backend.so=$LIB/libweston-14/headless-backend.so;desktop-shell.so=$LIB/weston/desktop-shell.so" \
"$WESTON" --backend=headless-backend.so --socket=probe-0 --idle-time=0 \
    > "$OUT/weston.log" 2>&1 &
WPID=$!
i=0
while [ $i -lt 40 ]; do
    [ -S "$XDG/probe-0" ] && break
    sleep 0.25; i=$((i+1))
done
if [ ! -S "$XDG/probe-0" ]; then
    echo "egl-probe: weston did not start"; cat "$OUT/weston.log"; exit 1
fi

cc -O2 -o "$OUT/egl_probe" scripts/egl_wayland_probe.c -ldl || { kill $WPID; exit 1; }
EGLV="$GPU_ROOT/usr/share/glvnd/egl_vendor.d/50_mesa.json"
[ -f "$EGLV" ] && export __EGL_VENDOR_LIBRARY_FILENAMES="$EGLV"
export LD_LIBRARY_PATH="$LIB:$LIB/dri"
XDG_RUNTIME_DIR="$XDG" WAYLAND_DISPLAY=probe-0 "$OUT/egl_probe"
RC=$?

kill $WPID 2>/dev/null
rm -rf "$OUT" "$XDG"
exit $RC
