#!/bin/sh
# ============================================================================
# Reverie Engine — end-to-end verification under Xvfb.
#
# Runs the daemon for a few seconds per renderer tier and captures
# screenshots via the built-in --shot hook:
#   1. cpu          (always available — the engine's own compositor)
#   2. opengl       (requires libEGL + a GL vendor; llvmpipe counts)
#   3. vulkan       (requires libvulkan + an ICD; lavapipe counts)
#      - the GPU compute compositing tier, present via the platform SHM
#        path (default) and the WSI swapchain (REV_VK_PRESENT=wsi), with
#        PIXEL PARITY vs the CPU renderer on both paths (static scene,
#        byte-for-byte) and the GPU sims (fluid + ripple) forced on via
#        REV_VK_SIMS_SW=1 — the whole compute path stays regression
#        tested without hardware.
#
# GPU tiers are skipped gracefully when their libraries are not loadable.
# When the local test stack exists at ../gpulibs/root (llvmpipe + lavapipe,
# no root needed), its env is exported automatically.
#
# Usage: scripts/e2e.sh [scene.rev]     (default: themes/mountain-dusk/scene.rev)
# ============================================================================
cd "$(dirname "$0")/.." || exit 1

SCENE="${1:-themes/mountain-dusk/scene.rev}"
OUT=$(mktemp -d)
XDISPLAY=":97"
XVFB_PID=""
PASS=0; SKIP=0; FAIL=0

# Isolate the daemon's session memory for the whole run: every daemon
# launch below updates the last-launched record, and without this the
# test run would rewrite the developer's real
# ~/.local/state/reverie/last-session.json (their autostart --restore
# would then replay a TEST scene instead of their wallpaper).
export XDG_STATE_HOME="$OUT/xdg-state"

# ---- optional local GPU stack (llvmpipe EGL + lavapipe Vulkan) --------------
GPU_ROOT="${REV_GPU_ROOT:-$(cd .. && pwd)/gpulibs/root}"
GPU_ENV=""
if [ -d "$GPU_ROOT/usr/lib/x86_64-linux-gnu" ]; then
    L="$GPU_ROOT/usr/lib/x86_64-linux-gnu"
    GPU_ENV="LD_LIBRARY_PATH=$L:$L/dri"
    if [ -f "$GPU_ROOT/usr/share/glvnd/egl_vendor.d/50_mesa.json" ]; then
        GPU_ENV="$GPU_ENV __EGL_VENDOR_LIBRARY_FILENAMES=$GPU_ROOT/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    fi
    if [ -f "$GPU_ROOT/usr/share/vulkan/icd.d/lvp_icd.json" ]; then
        GPU_ENV="$GPU_ENV VK_ICD_FILENAMES=$GPU_ROOT/usr/share/vulkan/icd.d/lvp_icd.json"
    fi
fi

cleanup() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null
        wait "$XVFB_PID" 2>/dev/null
    fi
}
# A trapped INT/TERM must TERMINATE the script: dash resumes execution after
# a signal trap whose handler does not exit — an external signal mid-leg
# would otherwise leave the leg running against a killed Xvfb (observed: a
# sandbox supervisor TERMs long commands; the resumed leg then fails on a
# dead display instead of the script exiting cleanly). The plain EXIT trap
# keeps the script's own exit status.
on_signal() {
    cleanup
    exit 130
}
trap cleanup EXIT
trap on_signal INT TERM

echo "reverie e2e — scene: $SCENE  out: $OUT"

start_xvfb() {
    Xvfb "$XDISPLAY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1.5
}

stop_xvfb() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null
        wait "$XVFB_PID" 2>/dev/null
        XVFB_PID=""
    fi
}

# ---- CPU tier (always) -------------------------------------------------------
# timeout 14 (not 8): the FIRST leg after the build also pays the runner's
# cold start — a hosted run measured 6 s between scene load and X11 connect
# (page cache / JIT warmup), which ate the whole budget before the 3 s
# screenshot could happen. The assertion is unchanged: the shot must exist.
start_xvfb
DISPLAY="$XDISPLAY" timeout 14 ./build/reverie -R cpu --shot 3 "$OUT/cpu" "$SCENE" \
    > "$OUT/cpu.log" 2>&1 || true
if [ -f "$OUT/cpu.png" ]; then
    echo "    cpu:          PASS (screenshot captured)"
    PASS=$((PASS+1))
else
    echo "    cpu:          FAIL (no screenshot)"
    cat "$OUT/cpu.log"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Renderer auto-selection: order + vocabulary --------------------------
# auto with only software stacks (the llvmpipe/lavapipe test env) must land
# on the native CPU renderer; with BOTH software stacks admitted via the
# compatibility overrides, the probe ORDER decides — Vulkan leads auto, so
# software Vulkan beats software GL. A bad renderer name is a hard error,
# never a silent fall-back into auto.
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 6 ./build/reverie --shot 3 "$OUT/autoc" "$SCENE" \
    > "$OUT/autoc.log" 2>&1 || true
DISPLAY="$XDISPLAY" env $GPU_ENV REV_AUTO_VK=1 REV_AUTO_GL_SW=1 timeout 6 \
    ./build/reverie --shot 3 "$OUT/autov" "$SCENE" > "$OUT/autov.log" 2>&1 || true
stop_xvfb
if grep -q "renderer: cpu" "$OUT/autoc.log" 2>/dev/null; then
    echo "    auto(cpu):    PASS (software stacks stay out of auto)"
    PASS=$((PASS+1))
elif grep -qE "renderer: (vulkan|opengl) \(tier" "$OUT/autoc.log" 2>/dev/null; then
    echo "    auto(hw):     PASS (a hardware renderer was selected)"
    PASS=$((PASS+1))
else
    echo "    auto(cpu):    FAIL (auto produced no renderer)"
    cat "$OUT/autoc.log"
    FAIL=$((FAIL+1))
fi
if grep -q "renderer: vulkan" "$OUT/autov.log" 2>/dev/null; then
    echo "    auto(order):  PASS (Vulkan is probed before OpenGL)"
    PASS=$((PASS+1))
elif grep -q "renderer: opengl" "$OUT/autov.log" 2>/dev/null \
     && grep -q "Vulkan unavailable" "$OUT/autov.log" 2>/dev/null; then
    echo "    auto(order):  SKIP (no Vulkan implementation on this system)"
    SKIP=$((SKIP+1))
elif grep -q "renderer: opengl" "$OUT/autov.log" 2>/dev/null; then
    echo "    auto(order):  FAIL (OpenGL won although Vulkan was available)"
    cat "$OUT/autov.log"
    FAIL=$((FAIL+1))
else
    echo "    auto(order):  SKIP (no GPU stacks on this system)"
    SKIP=$((SKIP+1))
fi
./build/reverie -R bogus "$SCENE" > "$OUT/badr.log" 2>&1
if [ $? -ne 0 ] && grep -q "bad --renderer 'bogus'" "$OUT/badr.log" 2>/dev/null; then
    echo "    renderer(bad):PASS (unknown renderer name rejected)"
    PASS=$((PASS+1))
else
    echo "    renderer(bad):FAIL (unknown renderer slipped into auto)"
    FAIL=$((FAIL+1))
fi

# ---- OpenGL tier -------------------------------------------------------------
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R opengl --shot 3 "$OUT/gl" "$SCENE" \
    > "$OUT/gl.log" 2>&1 || true
if grep -q "renderer: opengl" "$OUT/gl.log" 2>/dev/null; then
    echo "    opengl:       PASS (renderer active$( [ -f "$OUT/gl.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
else
    echo "    opengl:       SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- OpenGL tier: GPU fluid + ripple solvers (forced via the llvmpipe hook) --
# REV_GL_FLUID_SW=1 runs the shader-based sims even on software GL so the
# whole GPU sim path (fluid splats/Jacobi/advection/tonemap + ripple wave
# equation/refraction composite) stays regression-tested without hardware.
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 timeout 10 \
    ./build/reverie -R opengl --shot 5 "$OUT/glf" themes/fluid-aurora/scene.rev \
    > "$OUT/glf.log" 2>&1 || true
if grep -q "fluid + ripple solvers on GPU" "$OUT/glf.log" 2>/dev/null \
   && grep -q "simulations: GPU" "$OUT/glf.log" 2>/dev/null; then
    echo "    opengl(fluid): PASS (GPU sim active$( [ -f "$OUT/glf.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
elif grep -q "renderer: opengl" "$OUT/glf.log" 2>/dev/null; then
    echo "    opengl(fluid): FAIL (GPU sim did not activate)"
    cat "$OUT/glf.log"
    FAIL=$((FAIL+1))
else
    echo "    opengl(fluid): SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- OpenGL tier: GPU ripple (the scene that used to be locked out of ------
# GPU compositing entirely — refraction composites against the accum FBO)
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 REV_FAKE_POINTER=0.1 timeout 10 \
    ./build/reverie -R opengl --shot 6 "$OUT/rip" themes/ripple-lake/scene.rev \
    > "$OUT/rip.log" 2>&1 || true
if grep -q "simulations: GPU" "$OUT/rip.log" 2>/dev/null \
   && ! grep -q "scene needs CPU compositing" "$OUT/rip.log" 2>/dev/null; then
    echo "    opengl(ripple): PASS (GPU compositing + GPU sim$( [ -f "$OUT/rip.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
elif grep -q "renderer: opengl" "$OUT/rip.log" 2>/dev/null; then
    echo "    opengl(ripple): FAIL (ripple scene fell back to CPU compositing)"
    cat "$OUT/rip.log"
    FAIL=$((FAIL+1))
else
    echo "    opengl(ripple): SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Ripple CLICK placement regression (2026-10) ------------------------------
# The GPU ripple solver computed its Laplacian from UN-injected neighbors
# while subtracting 4x the injected center height: clicks launched at
# nh ~= 1.04h instead of 2h — a weak displaced smear, not a wave. These
# tests fire synthetic clicks (REV_FAKE_CLICK) on the GL tier and verify
# the disturbance lands ON the click, plus a CPU-tier cross-check.
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R cpu --shot 1 "$OUT/rc-base" \
    themes/ripple-lake > "$OUT/rc-base.log" 2>&1 || true
DISPLAY="$XDISPLAY" env $GPU_ENV REV_FAKE_CLICK=0.5,0.5 timeout 8 ./build/reverie -R cpu \
    --shot 1 "$OUT/rc-cpu" themes/ripple-lake > "$OUT/rc-cpu.log" 2>&1 || true
if [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-cpu.png" ]; then
    if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-cpu.png" 0.5 0.5 2>/dev/null; then
        echo "    ripple(click,cpu):  PASS (splash lands on the click)"
        PASS=$((PASS+1))
    else
        echo "    ripple(click,cpu):  FAIL (CPU splash misplaced)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    ripple(click,cpu):  SKIP (no CPU screenshots)"
    SKIP=$((SKIP+1))
fi
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 REV_FAKE_CLICK=0.5,0.5 timeout 10 \
    ./build/reverie -R opengl --shot 1 "$OUT/rc-gl" themes/ripple-lake \
    > "$OUT/rc-gl.log" 2>&1 || true
if grep -q "simulations: GPU" "$OUT/rc-gl.log" 2>/dev/null \
   && [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-gl.png" ]; then
    if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-gl.png" 0.5 0.5 2>/dev/null; then
        echo "    ripple(click,gl):   PASS (GPU splash lands on the click)"
        PASS=$((PASS+1))
    else
        echo "    ripple(click,gl):   FAIL (GPU splash misplaced — solver regression)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    ripple(click,gl):   SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
# corner click on the GL tier: placement near the frame corner, no crash
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 REV_FAKE_CLICK=0.15,0.85 timeout 10 \
    ./build/reverie -R opengl --shot 1 "$OUT/rc-corner" themes/ripple-lake \
    > "$OUT/rc-corner.log" 2>&1 || true
if grep -q "simulations: GPU" "$OUT/rc-corner.log" 2>/dev/null \
   && [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-corner.png" ]; then
    if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-corner.png" 0.15 0.85 2>/dev/null; then
        echo "    ripple(click,corner): PASS (corner splash lands correctly)"
        PASS=$((PASS+1))
    else
        echo "    ripple(click,corner): FAIL (corner splash misplaced)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    ripple(click,corner): SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
# click AFTER the water fully calms (the 2026-10 "dead clicks" report): the
# old wave update was anti-damped — one click grew until the field hit
# inf/NaN, the screen glitched, and every later click was swallowed. Fire a
# center click, let the lake fully calm (~19 s at this 640x400 grid), then
# click again at a different spot: the second splash must land on target.
# REV_FAKE_CLICK's @t syntax fires each click at its own timestamp.
for TIER in cpu opengl; do
    if [ "$TIER" = opengl ]; then
        TIENV="$GPU_ENV REV_GL_FLUID_SW=1"
    else
        TIENV="$GPU_ENV"
    fi
    DISPLAY="$XDISPLAY" env $TIENV REV_FAKE_CLICK="0.5,0.5@0.5;0.3,0.7@19" timeout 45 \
        ./build/reverie -R $TIER --shot 19.5 "$OUT/rc-late-$TIER" themes/ripple-lake \
        > "$OUT/rc-late-$TIER.log" 2>&1 || true
    OK=1
    [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-late-$TIER.png" ] || OK=0
    if [ "$TIER" = opengl ] && ! grep -q "simulations: GPU" "$OUT/rc-late-$TIER.log" 2>/dev/null; then
        OK=0
    fi
    if [ $OK -eq 1 ]; then
        if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-late-$TIER.png" 0.3 0.7 2>/dev/null; then
            echo "    ripple(click-after-calm,$TIER): PASS (second splash lands on target)"
            PASS=$((PASS+1))
        else
            echo "    ripple(click-after-calm,$TIER): FAIL (second click dead or misplaced)"
            FAIL=$((FAIL+1))
        fi
    else
        echo "    ripple(click-after-calm,$TIER): SKIP (no screenshots / no GL vendor)"
        SKIP=$((SKIP+1))
    fi
done
stop_xvfb

# ---- Particles CLICK placement regression (2026-10) ---------------------------
# Field report: snowfall / ember-drift bursts appeared MIRRORED — "move the
# mouse up and click, the effect appears down." Root cause: VS_PARTICLES
# mapped device-y into GL's y-up clip space, but this renderer's layer-target
# convention is TOP-DOWN (uploads store row 0 = image top, FS_GRADIENT
# evaluates v_screen as top-down pixel rows, the present pass flips v once
# for the whole frame) — so the composite mirrored every GPU point-sprite
# layer vertically. The ripple click legs never caught it: uniform ambient
# particles make a flip invisible; only the localized burst reveals it.
# The fixtures (tests/fixtures/click-probe-*.rev) run the real presets with
# spawn=pointer — zero ambient emission, so the ONLY inter-run diff is the
# click burst itself. Asymmetric click (0.25,0.25): a mirror lands ~ (0.75,
# 0.75), far outside the 6% centroid tolerance.
start_xvfb
for PROBE in snow embers aether; do
    FIX="tests/fixtures/click-probe-$PROBE.rev"
    for TIER in cpu opengl vulkan; do
        if [ "$TIER" = opengl ]; then
            TIENV="$GPU_ENV REV_GL_FLUID_SW=1"
            NEED="particles: GPU point sprites"
        elif [ "$TIER" = vulkan ]; then
            TIENV="$GPU_ENV REV_VK_SIMS_SW=1"
            NEED="particles: GPU point sprites"
        else
            TIENV="$GPU_ENV"
            NEED="renderer: cpu"
        fi
        DISPLAY="$XDISPLAY" env $TIENV timeout 8 ./build/reverie -R $TIER \
            --shot 0.5 "$OUT/pcp-$PROBE-$TIER-base" "$FIX" \
            > "$OUT/pcp-$PROBE-$TIER-base.log" 2>&1 || true
        DISPLAY="$XDISPLAY" env $TIENV REV_FAKE_CLICK=0.25,0.25 timeout 8 \
            ./build/reverie -R $TIER --shot 0.5 "$OUT/pcp-$PROBE-$TIER" "$FIX" \
            > "$OUT/pcp-$PROBE-$TIER.log" 2>&1 || true
        OK=1
        [ -f "$OUT/pcp-$PROBE-$TIER-base.png" ] && [ -f "$OUT/pcp-$PROBE-$TIER.png" ] || OK=0
        grep -q "$NEED" "$OUT/pcp-$PROBE-$TIER.log" 2>/dev/null || OK=0
        if [ $OK -eq 1 ]; then
            if python3 scripts/click_cmp.py "$OUT/pcp-$PROBE-$TIER-base.png" \
                    "$OUT/pcp-$PROBE-$TIER.png" 0.25 0.25 0.05 2>/dev/null; then
                echo "    particles(click,$TIER/$PROBE): PASS (burst lands on the click)"
                PASS=$((PASS+1))
            else
                echo "    particles(click,$TIER/$PROBE): FAIL (burst misplaced — mirrored layer)"
                FAIL=$((FAIL+1))
            fi
        else
            echo "    particles(click,$TIER/$PROBE): SKIP (tier unavailable / no shots)"
            SKIP=$((SKIP+1))
        fi
    done
done
stop_xvfb

# ---- Themes: folder + packed + hostile rejection -----------------------------
start_xvfb
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 3 "$OUT/theme" themes/ripple-lake \
    > "$OUT/theme.log" 2>&1 || true
if [ -f "$OUT/theme.png" ]; then
    echo "    theme(folder): PASS (screenshot captured)"
    PASS=$((PASS+1))
else
    echo "    theme(folder): FAIL (no screenshot)"
    cat "$OUT/theme.log"
    FAIL=$((FAIL+1))
fi
# packed theme — no extraction, byte-identical output expected
./build/revtool theme pack themes/ripple-lake -o "$OUT/ripple-lake.revpack" \
    > /dev/null 2>&1
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 3 "$OUT/themep" \
    "$OUT/ripple-lake.revpack" > "$OUT/themep.log" 2>&1 || true
if [ -f "$OUT/themep.png" ] && cmp -s "$OUT/themep.png" "$OUT/theme.png"; then
    echo "    theme(packed): PASS (byte-identical to the folder render)"
    PASS=$((PASS+1))
else
    echo "    theme(packed): FAIL (missing or differing screenshot)"
    cat "$OUT/themep.log"
    FAIL=$((FAIL+1))
fi
# the three new themes (nebula-drift / aurora-veil / light-lattice):
# offline render with pixel sanity (real content, not a flat frame) AND
# per-theme packed parity — each theme is an independent, distributable unit
for NEWT in nebula-drift aurora-veil light-lattice aether-rise; do
    ./build/reverie "themes/$NEWT" -o -s 480x270 -O "$OUT/nt_$NEWT.png" \
        > "$OUT/nt_$NEWT.log" 2>&1 || true
    ./build/revtool theme pack "themes/$NEWT" -o "$OUT/$NEWT.revpack" \
        > /dev/null 2>&1
    ./build/reverie "$OUT/$NEWT.revpack" -o -s 480x270 -O "$OUT/ntp_$NEWT.png" \
        > "$OUT/ntp_$NEWT.log" 2>&1 || true
    if [ -f "$OUT/nt_$NEWT.png" ] && cmp -s "$OUT/nt_$NEWT.png" "$OUT/ntp_$NEWT.png" \
       && python3 -c "
import struct, zlib, sys
d = open('$OUT/nt_$NEWT.png','rb').read(); pos, idat = 8, b''
while pos < len(d):
    ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
    if typ == b'IDAT': idat += d[pos:pos+ln]
    pos += ln + 4
raw = zlib.decompress(idat); stride = 480*4+1
cols = set()
for y in range(0, 270, 6):
    for x in range(0, 480, 6):
        o = y*stride + 1 + x*4
        cols.add((raw[o], raw[o+1], raw[o+2]))
sys.exit(0 if len(cols) > 40 else 1)
" 2>/dev/null; then
        echo "    theme(new-$NEWT): PASS (renders with content, packed parity)"
        PASS=$((PASS+1))
    else
        echo "    theme(new-$NEWT): FAIL (flat render or pack mismatch)"
        cat "$OUT/nt_$NEWT.log"
        FAIL=$((FAIL+1))
    fi
done
# corrupt pack: must exit with an error, never crash
head -c 200 "$OUT/ripple-lake.revpack" > "$OUT/corrupt.revpack"
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu "$OUT/corrupt.revpack" \
    > "$OUT/corrupt.log" 2>&1
if [ $? -ne 0 ] && grep -qE "not a \.revpack|pack truncated|CRC mismatch" "$OUT/corrupt.log"; then
    echo "    theme(corrupt): PASS (rejected with a clear error)"
    PASS=$((PASS+1))
else
    echo "    theme(corrupt): FAIL (accepted a corrupt pack?)"
    cat "$OUT/corrupt.log"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Video wallpapers (needs the ffmpeg CLI to synthesize a fixture) --------
if command -v ffmpeg >/dev/null 2>&1; then
    VDIR=$(mktemp -d)
    ffmpeg -y -loglevel error -f lavfi -i "testsrc2=size=320x180:rate=30" \
        -t 4 -pix_fmt yuv420p -c:v libx264 -preset ultrafast "$VDIR/loop.mp4"
    mkdir -p "$VDIR/assets"
    mv "$VDIR/loop.mp4" "$VDIR/assets/loop.mp4"
    printf 'scene { layer bg { type = video src = "assets/loop.mp4" loop = true } }\n' > "$VDIR/scene.rev"
    printf '{ "id": "vtest", "name": "vtest", "scene": "scene.rev" }\n' > "$VDIR/theme.json"
    # can this build decode at all? stub builds (ENABLE_FFMPEG=0) and
    # ABI-refused builds reject the media probe by design — the scene-video
    # legs then SKIP instead of failing on undecodable layers
    if ./build/reverie --video "$VDIR/assets/loop.mp4" -o -s 8x8 \
            -O "$OUT/vavail.png" >/dev/null 2>&1; then
    # CPU tier: sws path
    start_xvfb
    DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 3 "$OUT/vid" "$VDIR" \
        > "$OUT/vid.log" 2>&1 || true
    # GL tier: plane upload + shader YUV->RGB
    DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R opengl --shot 3 \
        "$OUT/vidgl" "$VDIR" > "$OUT/vidgl.log" 2>&1 || true
    stop_xvfb
    if [ -f "$OUT/vid.png" ] && grep -q "video: 320x180" "$OUT/vid.log" 2>/dev/null; then
        echo "    video(cpu):   PASS (decoded + rendered$( [ -f "$OUT/vidgl.png" ] && echo '; GL tier too' ))"
        PASS=$((PASS+1))
    else
        echo "    video(cpu):   FAIL (no decoded frame)"
        cat "$OUT/vid.log"
        FAIL=$((FAIL+1))
    fi
    if grep -q "video: GPU planes + shader color convert" "$OUT/vidgl.log" 2>/dev/null \
       && [ -f "$OUT/vidgl.png" ]; then
        echo "    video(gl):    PASS (GPU plane path active)"
        PASS=$((PASS+1))
    elif grep -q "renderer: opengl" "$OUT/vidgl.log" 2>/dev/null; then
        echo "    video(gl):    FAIL (GPU video path did not activate)"
        cat "$OUT/vidgl.log"
        FAIL=$((FAIL+1))
    else
        echo "    video(gl):    SKIP (no libEGL / GL vendor on this system)"
        SKIP=$((SKIP+1))
    fi
    else
        echo "    video(cpu):   SKIP (video support refused in this build)"
        echo "    video(gl):    SKIP (video support refused in this build)"
        SKIP=$((SKIP+2))
    fi
    rm -rf "$VDIR"
else
    echo "    video:        SKIP (no ffmpeg CLI for the fixture)"
    SKIP=$((SKIP+1))
fi

# ---- Media mode (--video): CLI contract, placement, playback, IPC ----------
# Independent of the scene-video legs above: the GIF fixture is checked in
# (tests/fixtures/gif_anim_transparent.gif), so these run wherever the
# FFmpeg libraries are usable — and the refusal path is verified where
# they are not (stub builds).
GIF_FIX="tests/fixtures/gif_anim_transparent.gif"

# CLI contract first: content-mode conflicts are deterministic, display-free
MCOK=1
./build/reverie --video x.mp4 --scene y.rev >/dev/null 2>&1 && MCOK=0
./build/reverie --scene y.rev --video x.mp4 >/dev/null 2>&1 && MCOK=0
./build/reverie --video x.mp4 positional.rev >/dev/null 2>&1 && MCOK=0
./build/reverie --scale fill >/dev/null 2>&1 && MCOK=0
./build/reverie --no-loop >/dev/null 2>&1 && MCOK=0
./build/reverie --video x.mp4 --scale bogus >/dev/null 2>&1 && MCOK=0
# a forgotten path (--video --scale stretch) must name the real problem —
# a missing value — not mis-report a content-mode conflict
./build/reverie --video --scale stretch >/dev/null 2>&1 && MCOK=0
./build/reverie --video --scale stretch 2>&1 | grep -q "needs a value" || MCOK=0
# media flags are position independent: --scale before --video must
# parse — the failure below is the missing file (or the stub refusal),
# never the 'applies to' parse error
./build/reverie --scale fit --video nosuchmedia.mp4 -o > "$OUT/mord.log" 2>&1 || true
grep -q "applies to" "$OUT/mord.log" && MCOK=0
if [ "$MCOK" = 1 ]; then
    echo "    media(cli):   PASS (content-mode conflicts rejected)"
    PASS=$((PASS+1))
else
    echo "    media(cli):   FAIL (a conflicting command line was accepted)"
    FAIL=$((FAIL+1))
fi

# probe: does this build's video stack open the GIF at all?
VMEDIA=probe
if ./build/reverie --video "$GIF_FIX" -o -s 8x8 -O "$OUT/vprobe.png" \
        > "$OUT/vprobe.log" 2>&1; then
    VMEDIA=ok
else
    # refused (stub build / unusable libraries): must be a CLEAN refusal
    if grep -q "no video support\|cannot open" "$OUT/vprobe.log" 2>/dev/null; then
        VMEDIA=refused
    else
        VMEDIA=broken
    fi
fi

if [ "$VMEDIA" = "refused" ]; then
    echo "    media:        SKIP (video support refused in this build)"
    SKIP=$((SKIP+1))
elif [ "$VMEDIA" = "broken" ]; then
    echo "    media:        FAIL (probe crashed instead of refusing)"
    cat "$OUT/vprobe.log"
    FAIL=$((FAIL+1))
else
    # offline placement: --once with the transparent GIF frame 0 must be
    # fully opaque black (transparency composites over the black bg)
    ./build/reverie --video "$GIF_FIX" -o -s 160x120 -O "$OUT/monce.png" \
        > "$OUT/monce.log" 2>&1
    if python3 -c "
import struct, zlib, sys
d = open('$OUT/monce.png','rb').read(); pos, idat = 8, b''
while pos < len(d):
    ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
    if typ == b'IDAT': idat += d[pos:pos+ln]
    pos += ln + 4
raw = zlib.decompress(idat); stride = 160*4+1
ok = True
for y in range(0, 120, 7):
    for x in range(0, 160, 33):
        if tuple(raw[y*stride+1+x*4:y*stride+5+x*4]) != (0,0,0,255): ok = False
sys.exit(0 if ok else 1)
" 2>/dev/null; then
        echo "    media(once):  PASS (transparent gif frame -> opaque black)"
        PASS=$((PASS+1))
    else
        echo "    media(once):  FAIL (transparent texels did not composite black)"
        FAIL=$((FAIL+1))
    fi

    # offline placement with --scale fit: letterbox bars must be black and
    # the video area must carry the solid fixture color
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -y -loglevel error -f lavfi -i "color=c=0x0080FF:s=400x100:r=10" \
            -t 1 -pix_fmt yuv420p -c:v libx264 -preset ultrafast \
            "$OUT/solid.mp4" 2>/dev/null
        ./build/reverie --video "$OUT/solid.mp4" --scale fit -o -s 200x100 \
            -O "$OUT/mfit.png" > "$OUT/mfit.log" 2>&1
        if python3 -c "
import struct, zlib, sys
d = open('$OUT/mfit.png','rb').read(); pos, idat = 8, b''
while pos < len(d):
    ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
    if typ == b'IDAT': idat += d[pos:pos+ln]
    pos += ln + 4
raw = zlib.decompress(idat); stride = 200*4+1
def px(y, x): return tuple(raw[y*stride+1+x*4:y*stride+5+x*4])
bar = px(2, 100) == (0,0,0,255) and px(97, 100) == (0,0,0,255)
mid = px(50, 100)
col = abs(mid[0]) < 12 and 110 < mid[1] < 145 and abs(mid[2]-255) < 12
sys.exit(0 if bar and col else 1)
" 2>/dev/null; then
            echo "    media(fit):   PASS (letterbox bars black, video placed)"
            PASS=$((PASS+1))
        else
            echo "    media(fit):   FAIL (--scale fit placement wrong)"
            FAIL=$((FAIL+1))
        fi
        rm -f "$OUT/solid.mp4"
    else
        echo "    media(fit):   SKIP (no ffmpeg CLI for the solid fixture)"
        SKIP=$((SKIP+1))
    fi

    # live playback under Xvfb: CPU tier — the red frame (t in [0.5,1.0))
    # must reach the compositor; --fps 10 keeps the shot timing inside it
    start_xvfb
    DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --fps 10 \
        --video "$GIF_FIX" --shot 0.75 "$OUT/mtv" > "$OUT/mtv.log" 2>&1 || true
    stop_xvfb
    if grep -q "video mode" "$OUT/mtv.log" 2>/dev/null \
       && python3 -c "
import struct, zlib, sys
d = open('$OUT/mtv.png','rb').read(); pos, idat = 8, b''
while pos < len(d):
    ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
    if typ == b'IDAT': idat += d[pos:pos+ln]
    pos += ln + 4
w = struct.unpack('>I', d[16:20])[0]; h = struct.unpack('>I', d[20:24])[0]
raw = zlib.decompress(idat); stride = w*4+1
r = g = b = n = 0
for y in range(0, h, 11):
    for x in range(0, w, 47):
        p = raw[y*stride+1+x*4:y*stride+5+x*4]
        r += p[0]; g += p[1]; b += p[2]; n += 1
# red frame: strong red dominance (allow slight compositor attenuation)
sys.exit(0 if n and r/n > 180 and g/n < 70 and b/n < 70 else 1)
" 2>/dev/null; then
        echo "    media(cpu):   PASS (gif playback reaches the display)"
        PASS=$((PASS+1))
    else
        echo "    media(cpu):   FAIL (no visible red frame during playback)"
        cat "$OUT/mtv.log"
        FAIL=$((FAIL+1))
    fi

    # GL tier: the RGBA pixel-layer path (same red-frame evidence)
    start_xvfb
    DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R opengl --fps 10 \
        --video "$GIF_FIX" --shot 0.75 "$OUT/mtvgl" > "$OUT/mtvgl.log" 2>&1 || true
    stop_xvfb
    if grep -q "renderer: opengl" "$OUT/mtvgl.log" 2>/dev/null \
       && [ -f "$OUT/mtvgl.png" ]; then
        echo "    media(gl):    PASS (GPU pixel-layer playback)"
        PASS=$((PASS+1))
    elif grep -q "renderer: opengl" "$OUT/mtvgl.log" 2>/dev/null; then
        echo "    media(gl):    FAIL (GL tier produced no frame)"
        cat "$OUT/mtvgl.log"
        FAIL=$((FAIL+1))
    else
        echo "    media(gl):    SKIP (no libEGL / GL vendor on this system)"
        SKIP=$((SKIP+1))
    fi

    # media-mode IPC: PING/STATUS (mode=video)/QUIT through the socket
    start_xvfb
    MSOCK="$OUT/media.sock"
    DISPLAY="$XDISPLAY" REV_IPC_SOCK="$MSOCK" timeout 16 ./build/reverie -R cpu \
        --video "$GIF_FIX" > "$OUT/mipc.log" 2>&1 &
    MPID=$!
    sleep 2
    ./build/revtool ipc -s "$MSOCK" PING > "$OUT/mipc1.out" 2>&1
    ./build/revtool ipc -s "$MSOCK" STATUS > "$OUT/mipc2.out" 2>&1
    ./build/revtool ipc -s "$MSOCK" "THEME themes/ripple-lake" > "$OUT/mipc3.out" 2>&1
    ./build/revtool ipc -s "$MSOCK" QUIT > "$OUT/mipc4.out" 2>&1
    sleep 1
    MIP=1
    grep -q "OK PONG" "$OUT/mipc1.out" 2>/dev/null || MIP=0
    grep -q "mode=video" "$OUT/mipc2.out" 2>/dev/null || MIP=0
    grep -q "ERR" "$OUT/mipc3.out" 2>/dev/null || MIP=0
    grep -q "shutting down" "$OUT/mipc.log" 2>/dev/null || MIP=0
    kill -0 $MPID 2>/dev/null && MIP=0
    if [ "$MIP" = 1 ]; then
        echo "    media(ipc):   PASS (ping/status mode=video/theme-refused/quit)"
        PASS=$((PASS+1))
    else
        echo "    media(ipc):   FAIL (media control session incomplete)"
        cat "$OUT/mipc.log" "$OUT/mipc1.out" "$OUT/mipc2.out" \
            "$OUT/mipc3.out" "$OUT/mipc4.out" 2>/dev/null
        kill $MPID 2>/dev/null; wait $MPID 2>/dev/null
        FAIL=$((FAIL+1))
    fi
    stop_xvfb
fi

# ---- Static media mode (--image): native PNG, no FFmpeg anywhere ------------
# These legs run in EVERY build configuration: the decoder is reverie's own
# (src/png.c), so they double as the end-to-end exercise of the native codec.
IMG_FIX="tests/fixtures/png_rgb8_40x25_i1.png"   # 40x25 opaque RGB

# CLI contract: content-mode conflicts, video-only flags, restore exclusivity
ICOK=1
./build/reverie --image a.png --scene b.rev >/dev/null 2>&1 && ICOK=0
./build/reverie --scene b.rev --image a.png >/dev/null 2>&1 && ICOK=0
./build/reverie --image a.png --video b.mp4 >/dev/null 2>&1 && ICOK=0
./build/reverie --image a.png positional.rev >/dev/null 2>&1 && ICOK=0
./build/reverie --image a.png --image b.png >/dev/null 2>&1 && ICOK=0
./build/reverie --image a.png --no-loop >/dev/null 2>&1 && ICOK=0
./build/reverie --restore --image a.png >/dev/null 2>&1 && ICOK=0
# a forgotten path (--image --scale fit) must name the real problem
./build/reverie --image --scale fit >/dev/null 2>&1 && ICOK=0
./build/reverie --image --scale fit 2>&1 | grep -q "needs a value" || ICOK=0
# --scale before --image parses (position independent): the only failure
# below is the missing file, never the 'applies to' parse error
./build/reverie --scale fit --image nosuch.png -o > "$OUT/iord.log" 2>&1 || true
grep -q "applies to" "$OUT/iord.log" && ICOK=0
# a non-PNG must refuse cleanly through the native decoder's own message
./build/reverie --image tests/fixtures/deflate_trunc.zlib -o > "$OUT/ibad.log" 2>&1 && ICOK=0
grep -q "PNG:" "$OUT/ibad.log" 2>/dev/null || ICOK=0
if [ "$ICOK" = 1 ]; then
    echo "    image(cli):   PASS (conflicts + video-only flags + decoder refusal)"
    PASS=$((PASS+1))
else
    echo "    image(cli):   FAIL (a bad --image command line was accepted)"
    FAIL=$((FAIL+1))
fi

# offline placement, all four --scale modes (100x80 viewport, 40x25 source):
# fill covers, fit letterboxes, center centers, stretch fills — judged on
# the black-bar geometry, exactly the evidence the media legs use
for MODE in fill fit center stretch; do
    ./build/reverie --image "$IMG_FIX" --scale $MODE -o -s 100x80 \
        -O "$OUT/img_$MODE.png" > "$OUT/img_$MODE.log" 2>&1
done
if python3 -c "
import struct, zlib, sys
def bars(path):
    d = open(path,'rb').read(); pos, idat = 8, b''
    while pos < len(d):
        ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
        if typ == b'IDAT': idat += d[pos:pos+ln]
        pos += ln + 4
    raw = zlib.decompress(idat); W, H, stride = 100, 80, 401
    def black(y, x): return tuple(raw[y*stride+1+x*4:y*stride+5+x*4]) == (0,0,0,255)
    cols = any(all(black(y,x) for y in range(H)) for x in range(W))
    rows = any(all(black(y,x) for x in range(W)) for y in range(H))
    return cols, rows
f = bars('$OUT/img_fill.png')
t = bars('$OUT/img_fit.png')
c = bars('$OUT/img_center.png')
s = bars('$OUT/img_stretch.png')
ok = (not f[0] and not f[1] and          # fill: cover-crop, no bars
      not t[0] and t[1] and              # fit: letterbox rows only
      c[0] and c[1] and                  # center: bars on both axes
      not s[0] and not s[1])             # stretch: fills everything
sys.exit(0 if ok else 1)
" 2>/dev/null; then
    echo "    image(place): PASS (fill covers, fit letterboxes, center centers, stretch fills)"
    PASS=$((PASS+1))
else
    echo "    image(place): FAIL (--scale placement geometry wrong)"
    FAIL=$((FAIL+1))
fi

# GL tier: the RGBA pixel-layer path with the upload-once content id
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R opengl \
    --image "$IMG_FIX" --shot 1 "$OUT/igl" > "$OUT/igl.log" 2>&1 || true
stop_xvfb
if grep -q "renderer: opengl" "$OUT/igl.log" 2>/dev/null \
   && [ -f "$OUT/igl.png" ]; then
    echo "    image(gl):    PASS (GPU pixel-layer static image)"
    PASS=$((PASS+1))
elif grep -q "renderer: opengl" "$OUT/igl.log" 2>/dev/null; then
    echo "    image(gl):    FAIL (GL tier produced no frame)"
    cat "$OUT/igl.log"
    FAIL=$((FAIL+1))
else
    echo "    image(gl):    SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi

# live session under Xvfb: STATUS reports mode=image, the launch lands in the
# session memory, and --restore replays it as an image (the autostart story)
start_xvfb
ISOCK="$OUT/img.sock"
ISTATE=$(mktemp -d)
DISPLAY="$XDISPLAY" REV_IPC_SOCK="$ISOCK" XDG_STATE_HOME="$ISTATE" \
    timeout 16 ./build/reverie -R cpu --image "$IMG_FIX" \
    > "$OUT/iipc.log" 2>&1 &
IPID=$!
sleep 2
./build/revtool ipc -s "$ISOCK" STATUS > "$OUT/iipc1.out" 2>&1
./build/revtool ipc -s "$ISOCK" QUIT > "$OUT/iipc2.out" 2>&1
sleep 1
IREC="$ISTATE/reverie/last-session.json"
IIP=1
grep -q "mode=image" "$OUT/iipc1.out" 2>/dev/null || IIP=0
grep -q '"--image"' "$IREC" 2>/dev/null || IIP=0
kill -0 $IPID 2>/dev/null && IIP=0
# the restore: same XDG state, no explicit content — the record drives it
XDG_STATE_HOME="$ISTATE" ./build/reverie --restore -o -s 100x80 \
    -O "$OUT/irest.png" > "$OUT/irest.log" 2>&1 || IIP=0
grep -q "image mode" "$OUT/irest.log" 2>/dev/null || IIP=0
if [ "$IIP" = 1 ]; then
    echo "    image(live):  PASS (status mode=image, session recorded, --restore replays it)"
    PASS=$((PASS+1))
else
    echo "    image(live):  FAIL (live image session incomplete)"
    cat "$OUT/iipc.log" "$OUT/iipc1.out" "$OUT/irest.log" 2>/dev/null
    kill $IPID 2>/dev/null; wait $IPID 2>/dev/null
    FAIL=$((FAIL+1))
fi
rm -rf "$ISTATE"
stop_xvfb

# ---- Image-layer placement: sprites at authored rects ----------------------
# The zero-code authoring vocabulary this session added: pos/size/scale/
# anchor composite smaller images into the frame (offline render + probes).
cp tests/fixtures/png_rgb8_40x25_i1.png "$OUT/sprite.png"
cat > "$OUT/place.rev" <<'EOF'
scene "Placement" {
    fps = 30
    layer bg { type = solid  color = #101010 }
    layer corner {
        type = image
        src = "sprite.png"
        pos = (0.25, 0.75)
        anchor = topleft
        scale = 1
    }
    layer zoom {
        type = image
        src = "sprite.png"
        pos = (0.95, 0.1)
        anchor = topright
        scale = 1.5
    }
    layer box {
        type = image
        src = "sprite.png"
        pos = (0.5, 0.55)
        size = (0.25, 0.25)
        fit = fit
    }
}
EOF
./build/reverie "$OUT/place.rev" -o -s 160x120 -O "$OUT/place.png" \
    > "$OUT/place.log" 2>&1
if python3 -c "
import struct, zlib, sys
d = open('$OUT/place.png','rb').read(); pos, idat = 8, b''
while pos < len(d):
    ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
    if typ == b'IDAT': idat += d[pos:pos+ln]
    pos += ln + 4
raw = zlib.decompress(idat); W, H, stride = 160, 120, 160*4+1
def px(x, y):
    r = raw[y*stride+1:y*stride+1+stride-1]
    return tuple(r[x*4:x*4+4])
def near_bg(x, y):
    r, g, b = px(x, y)[:3]
    return abs(r-16) <= 6 and abs(g-16) <= 6 and abs(b-16) <= 6
def diff_count(x0, y0, x1, y1):
    return sum(1 for y in range(y0, y1) for x in range(x0, x1)
               if not near_bg(x, y))
ok = True
# corner sprite: 40x25 native at (40, 90); topleft texel is #0055aa
r, g, b = px(40, 90)[:3]
ok = ok and abs(r-0) <= 4 and abs(g-85) <= 4 and abs(b-170) <= 4
ok = ok and diff_count(41, 91, 79, 114) > 38*23*0.8
ok = ok and near_bg(38, 100) and near_bg(85, 100) and near_bg(41, 88)
# zoom sprite: 60x38 at top-right, resampled — dense non-bg content
ok = ok and diff_count(95, 15, 150, 48) > 55*33*0.75
# box layer: rect 40x30 centered at (80,66) -> [60,100)x[51,81); the 40x25
# source contains to 40x25 -> 2px letterbox bars inside the rect
ok = ok and diff_count(62, 51, 98, 53) == 0
ok = ok and diff_count(62, 55, 98, 76) > 36*21*0.75
sys.exit(0 if ok else 1)
" 2>/dev/null; then
    echo "    scene(place): PASS (pos/anchor/scale/size rects land where authored)"
    PASS=$((PASS+1))
else
    echo "    scene(place): FAIL (placed sprites missing or misplaced)"
    cat "$OUT/place.log" 2>/dev/null
    FAIL=$((FAIL+1))
fi

# ---- X11 Expose: static content snaps back on workspace switches ----------
# openbox switches desktops by unmapping the other desktop's windows; the
# server repaints the newly exposed wallpaper regions with the window
# background (black) and sends Expose. A 60 fps video repaints within a
# frame anyway; a static --image paced at 2 fps left the patch black for up
# to half a second (2026-10 field report). The backend now re-presents the
# last frame from its own SHM segment the moment the Expose arrives.
# reverie's --shot cannot observe the gap (it reads the wallpaper window's
# private content AFTER that frame's present), so the occluder grabs the
# ROOT pixels — what the user's eyes see. --fps 1 widens the gap to a full
# second so the sample lands deterministically inside it.
start_xvfb
cc -O2 -o "$OUT/occlude" scripts/occlude_x11.c 2>/dev/null
if [ -x "$OUT/occlude" ]; then
    DISPLAY="$XDISPLAY" timeout 6 ./build/reverie -R cpu --image "$IMG_FIX" \
        --fps 1 > "$OUT/expose.log" 2>&1 &
    EPID=$!
    sleep 1.5
    DISPLAY="$XDISPLAY" "$OUT/occlude" 300 400 150 "$OUT/expose.raw" \
        > "$OUT/expose.dim" 2> "$OUT/expose.occl"
    wait $EPID 2>/dev/null || true
    if python3 -c "
import sys
tok = open('$OUT/expose.dim').read().split()
if len(tok) < 4:
    sys.exit(1)
W, H = int(tok[1]), int(tok[2])
raw = open('$OUT/expose.raw','rb').read()
stride = W * 4
half = W // 2
def bright(frac):
    n = tot = 0
    for y in range(0, H, 3):
        row = raw[y*stride : y*stride + half*4] if frac == 'l' \
              else raw[y*stride + half*4 : (y+1)*stride]
        for b in row[::7]:
            tot += 1
            if b > 12:
                n += 1
    return n / max(tot, 1)
# left = the occluded-then-exposed half; right = never covered (control)
sys.exit(0 if bright('l') > 0.10 and bright('r') > 0.10 else 1)
" 2>/dev/null; then
        echo "    x11(expose):  PASS (exposed region repainted within 150ms)"
        PASS=$((PASS+1))
    else
        echo "    x11(expose):  FAIL (exposed region stayed black — repaint missing)"
        cat "$OUT/expose.log" "$OUT/expose.occl" 2>/dev/null
        FAIL=$((FAIL+1))
    fi
else
    echo "    x11(expose):  FAIL (occlude helper did not compile)"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Vulkan tier: GPU compute compositing, platform present -----------------
# The tier composites every layer as a compute kernel; the finished frame is
# copied out and presented via wl_shm/MIT-SHM.
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 10 ./build/reverie -R vulkan --shot 3 "$OUT/vk" "$SCENE" \
    > "$OUT/vk.log" 2>&1 || true
if grep -q "renderer: vulkan" "$OUT/vk.log" 2>/dev/null; then
    ok=1
    grep -q "GPU COMPUTE COMPOSITING" "$OUT/vk.log" 2>/dev/null || ok=0
    grep -q "platform SHM path" "$OUT/vk.log" 2>/dev/null || ok=0
    if [ "$ok" = 1 ]; then
        echo "    vulkan(shm):  PASS (GPU compute compositing active)"
        PASS=$((PASS+1))
    else
        echo "    vulkan(shm):  FAIL (compute compositing / present-path log missing)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    vulkan(shm):  SKIP (no libvulkan / ICD on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Vulkan tier: GPU compositing pixel parity (static scene) ---------------
# A static scene removes animation timing from the comparison, so the
# compute kernels' output must match the CPU compositor BYTE FOR BYTE.
if grep -q "renderer: vulkan" "$OUT/vk.log" 2>/dev/null; then
    start_xvfb
    STATIC=$(mktemp -d)
    sed 's/fps = 60/fps = 5/; s/parallax = 0.14//; s/parallax = 0.05//; s/opacity = 0.8/opacity = 1.0/' \
        "$SCENE" > "$STATIC/static.rev" 2>/dev/null || cp "$SCENE" "$STATIC/static.rev"
    # assets are referenced relative to the scene file
    SCENE_DIR=$(dirname "$SCENE")
    if [ -d "$SCENE_DIR/assets" ]; then
        cp -r "$SCENE_DIR/assets" "$STATIC/" 2>/dev/null || true
    fi
    DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 2 "$OUT/stcpu" "$STATIC/static.rev" \
        > "$OUT/stcpu.log" 2>&1 || true
    DISPLAY="$XDISPLAY" env $GPU_ENV timeout 10 ./build/reverie -R vulkan --shot 2 "$OUT/stvk" "$STATIC/static.rev" \
        > "$OUT/stvk.log" 2>&1 || true
    DISPLAY="$XDISPLAY" env $GPU_ENV REV_VK_PRESENT=wsi timeout 10 ./build/reverie -R vulkan --shot 2 "$OUT/stvw" "$STATIC/static.rev" \
        > "$OUT/stvw.log" 2>&1 || true
    if [ -f "$OUT/stvk.png" ] && [ -f "$OUT/stcpu.png" ]; then
        if python3 scripts/rev_shot_cmp.py "$OUT/stvk.png" "$OUT/stcpu.png" 2 8 >/dev/null 2>&1; then
            echo "    vulkan(cmp):  PASS (platform path pixels match CPU tier)"
            PASS=$((PASS+1))
        else
            echo "    vulkan(cmp):  FAIL (compute compositing diverged from CPU tier)"
            python3 scripts/rev_shot_cmp.py "$OUT/stvk.png" "$OUT/stcpu.png" 2 8 || true
            FAIL=$((FAIL+1))
        fi
    else
        echo "    vulkan(cmp):  SKIP (screenshot missing)"
        SKIP=$((SKIP+1))
    fi
    if [ -f "$OUT/stvw.png" ] && [ -f "$OUT/stcpu.png" ]; then
        if python3 scripts/rev_shot_cmp.py "$OUT/stvw.png" "$OUT/stcpu.png" 2 8 >/dev/null 2>&1; then
            echo "    vulkan(wcmp): PASS (WSI blit path pixels match CPU tier)"
            PASS=$((PASS+1))
        else
            echo "    vulkan(wcmp): FAIL (WSI blit path diverged from CPU tier)"
            python3 scripts/rev_shot_cmp.py "$OUT/stvw.png" "$OUT/stcpu.png" 2 8 || true
            FAIL=$((FAIL+1))
        fi
    else
        echo "    vulkan(wcmp): SKIP (screenshot missing)"
        SKIP=$((SKIP+1))
    fi
    rm -rf "$STATIC"
    stop_xvfb
fi

# ---- GPU gradient evaluation parity (static scene, GPU paths forced on) -----
# wants_grads is false on software devices, so every parity comparison above
# rasterizes gradients with the CPU LUT — the shader-evaluated gradient
# arithmetic (FS_GRADIENT / gradient.comp, incl. the ordered dither offset)
# is never pixel-compared there. This leg forces it on (REV_VK_SIMS_SW /
# REV_GL_FLUID_SW both flip wants_grads on software devices) and compares
# against the CPU raster on a dark multi-stop linear+radial scene — the
# configuration where the dither units bug (a 255x over-amplified Bayer
# offset: dots instead of smooth ramps on every hardware tier) slipped
# through all other comparisons.
if grep -q "renderer: vulkan" "$OUT/vk.log" 2>/dev/null \
   || grep -q "renderer: opengl" "$OUT/gl.log" 2>/dev/null; then
    start_xvfb
    GRAD=$(mktemp -d)
    cat > "$GRAD/grad.rev" <<'EOF'
/* static gradient parity fixture — dark ramps + alpha, no anims */
scene "gpu-grad parity" {
    fps = 5
    layer lin {
        type = gradient
        shape = linear
        angle = 37
        stops = [#04060f, (#1a2c4a 0.5), (#2c4f6e 1.0)]
    }
    layer rad {
        type = gradient
        shape = radial
        stops = [(#00000000 0.0), (#5a3d6e66 0.55), (#0a0a2a 1.0)]
        blend = screen
    }
}
EOF
    DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 2 "$OUT/ggcpu" "$GRAD/grad.rev" \
        > "$OUT/ggcpu.log" 2>&1 || true
    if grep -q "renderer: vulkan" "$OUT/vk.log" 2>/dev/null; then
        DISPLAY="$XDISPLAY" env $GPU_ENV REV_VK_SIMS_SW=1 timeout 10 \
            ./build/reverie -R vulkan --shot 2 "$OUT/ggvk" "$GRAD/grad.rev" \
            > "$OUT/ggvk.log" 2>&1 || true
        if [ -f "$OUT/ggvk.png" ] && [ -f "$OUT/ggcpu.png" ]; then
            if python3 scripts/rev_shot_cmp.py "$OUT/ggvk.png" "$OUT/ggcpu.png" 2 8 >/dev/null 2>&1; then
                echo "    vulkan(ggrad): PASS (shader-evaluated gradient matches CPU raster)"
                PASS=$((PASS+1))
            else
                echo "    vulkan(ggrad): FAIL (GPU gradient evaluation diverged from CPU raster)"
                python3 scripts/rev_shot_cmp.py "$OUT/ggvk.png" "$OUT/ggcpu.png" 2 8 || true
                FAIL=$((FAIL+1))
            fi
        else
            echo "    vulkan(ggrad): SKIP (screenshot missing)"
            SKIP=$((SKIP+1))
        fi
    fi
    if grep -q "renderer: opengl" "$OUT/gl.log" 2>/dev/null; then
        DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 timeout 10 \
            ./build/reverie -R opengl --shot 2 "$OUT/gggl" "$GRAD/grad.rev" \
            > "$OUT/gggl.log" 2>&1 || true
        if [ -f "$OUT/gggl.png" ] && [ -f "$OUT/ggcpu.png" ]; then
            if python3 scripts/rev_shot_cmp.py "$OUT/gggl.png" "$OUT/ggcpu.png" 2 8 >/dev/null 2>&1; then
                echo "    opengl(ggrad): PASS (shader-evaluated gradient matches CPU raster)"
                PASS=$((PASS+1))
            else
                echo "    opengl(ggrad): FAIL (GPU gradient evaluation diverged from CPU raster)"
                python3 scripts/rev_shot_cmp.py "$OUT/gggl.png" "$OUT/ggcpu.png" 2 8 || true
                FAIL=$((FAIL+1))
            fi
        else
            echo "    opengl(ggrad): SKIP (screenshot missing)"
            SKIP=$((SKIP+1))
        fi
    fi
    rm -rf "$GRAD"
    stop_xvfb
fi

# ---- Vulkan tier: GPU sims (forced on software devices via REV_VK_SIMS_SW) --
# The whole GPU sim path (fluid splats/Jacobi/advection/tonemap + ripple wave
# equation/refraction composite) stays regression-tested on lavapipe.
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV REV_VK_SIMS_SW=1 timeout 12 \
    ./build/reverie -R vulkan --shot 5 "$OUT/vkf" themes/fluid-aurora/scene.rev \
    > "$OUT/vkf.log" 2>&1 || true
if grep -q "fluid + ripple solvers on GPU" "$OUT/vkf.log" 2>/dev/null \
   && grep -q "simulations: GPU" "$OUT/vkf.log" 2>/dev/null; then
    echo "    vulkan(fluid): PASS (GPU sim active$( [ -f "$OUT/vkf.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
elif grep -q "renderer: vulkan" "$OUT/vkf.log" 2>/dev/null; then
    echo "    vulkan(fluid): FAIL (GPU sim did not activate)"
    cat "$OUT/vkf.log"
    FAIL=$((FAIL+1))
else
    echo "    vulkan(fluid): SKIP (no libvulkan / ICD on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Vulkan tier: GPU ripple + click parity (the field-reported scene) -------
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV REV_VK_SIMS_SW=1 REV_FAKE_POINTER=0.1 REV_FAKE_CLICK="0.5,0.5@2" \
    timeout 12 ./build/reverie -R vulkan --shot 2.6 "$OUT/ripv" themes/ripple-lake/scene.rev \
    > "$OUT/ripv.log" 2>&1 || true
if grep -q "fluid + ripple solvers on GPU" "$OUT/ripv.log" 2>/dev/null \
   && [ -f "$OUT/ripv.png" ]; then
    echo "    vulkan(ripple): PASS (GPU wave equation + refraction active)"
    PASS=$((PASS+1))
else
    if grep -q "renderer: vulkan" "$OUT/ripv.log" 2>/dev/null; then
        echo "    vulkan(ripple): FAIL (GPU ripple did not activate)"
        cat "$OUT/ripv.log"
        FAIL=$((FAIL+1))
    else
        echo "    vulkan(ripple): SKIP (no libvulkan / ICD on this system)"
        SKIP=$((SKIP+1))
    fi
fi
stop_xvfb

# ---- X11 idle power-save: real DPMS transitions (if Xvfb has DPMS) ----------
# dpmsctl is a raw-wire stand-in for `xset dpms force` (absent in minimal
# test environments). Force off -> the daemon must pause rendering; force
# on -> it must resume. Servers without the extension SKIP (the deterministic
# file-hook leg below carries the coverage there).
start_xvfb
cc -O2 -o "$OUT/dpmsctl" scripts/dpmsctl.c 2>/dev/null || true
DPMSCTL_OK=0
if [ -x "$OUT/dpmsctl" ]; then
    DISPLAY="$XDISPLAY" "$OUT/dpmsctl" info > "$OUT/dpmsctl.log" 2>&1 \
        && DPMSCTL_OK=1
fi
if [ "$DPMSCTL_OK" = 1 ]; then
    DISPLAY="$XDISPLAY" timeout 10 ./build/reverie -R cpu "$SCENE" \
        > "$OUT/dpms.log" 2>&1 &
    DPID=$!
    sleep 2
    DISPLAY="$XDISPLAY" "$OUT/dpmsctl" force off >> "$OUT/dpmsctl.log" 2>&1
    sleep 2.5
    DISPLAY="$XDISPLAY" "$OUT/dpmsctl" force on >> "$OUT/dpmsctl.log" 2>&1
    sleep 2
    kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
    if grep -q "idle: no surface" "$OUT/dpms.log" 2>/dev/null \
       && grep -q "idle: surface visible again" "$OUT/dpms.log" 2>/dev/null; then
        echo "    x11(dpms):    PASS (display off pauses rendering; on resumes)"
        PASS=$((PASS+1))
    else
        echo "    x11(dpms):    FAIL (idle cycle did not follow DPMS transitions)"
        cat "$OUT/dpms.log"
        cat "$OUT/dpmsctl.log"
        FAIL=$((FAIL+1))
    fi
else
    echo "    x11(dpms):    SKIP (Xvfb without the DPMS extension)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- X11 idle power-save: deterministic file-hook cycle + idle watchdog ------
# REV_X11_DPMS_FILE drives the same query path a real power transition takes,
# on any server. REV_IDLE_AUDIT_S=2 shrinks the watchdog so the audit burst
# (full-rate rendering while hidden, escapes a wedged detector) is observable.
start_xvfb
DPMSF="$OUT/dpms-state"
echo on > "$DPMSF"
DISPLAY="$XDISPLAY" REV_X11_DPMS_FILE="$DPMSF" REV_IDLE_AUDIT_S=2 \
    timeout 12 ./build/reverie -R cpu "$SCENE" > "$OUT/dpmsf.log" 2>&1 &
DPID=$!
sleep 2
echo off > "$DPMSF"
sleep 3              # hidden: idle pause + (t>=2s) watchdog audit burst
echo on > "$DPMSF"
sleep 2
kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
ok=1
grep -q "idle: no surface" "$OUT/dpmsf.log" 2>/dev/null || ok=0
grep -q "idle watchdog" "$OUT/dpmsf.log" 2>/dev/null || ok=0
grep -q "idle: surface visible again" "$OUT/dpmsf.log" 2>/dev/null || ok=0
if [ "$ok" = 1 ]; then
    echo "    x11(idle):    PASS (hook-driven pause + watchdog audit + resume)"
    PASS=$((PASS+1))
else
    echo "    x11(idle):    FAIL (idle cycle incomplete)"
    cat "$OUT/dpmsf.log"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- X11 auth: MIT-MAGIC-COOKIE-1 against an enforcing server ----------------
# The 2026-10 field report ("server refused connection: Invalid" under
# Xorg/XLibre + Openbox): real sessions — startx, display managers, XLibre —
# require a cookie and the backend must find it in $XAUTHORITY. CI's plain
# Xvfb never enforces auth, which is exactly how a broken parser shipped.
# This leg runs a second server WITH -auth (authority fixture in the real
# big-endian interleaved format — the same bytes a live Xorg accepts), and
# its twin asserts the server's refusal reason surfaces verbatim when the
# credentials are missing.
AUTHDISPLAY=":98"
AUTHF="$OUT/Xauthority"
printf '\377\377\000\000\000\00298\000\022MIT-MAGIC-COOKIE-1\000\020\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017' \
    > "$AUTHF"
Xvfb "$AUTHDISPLAY" -screen 0 1280x800x24 -nolisten tcp -auth "$AUTHF" \
    >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5
DISPLAY="$AUTHDISPLAY" XAUTHORITY="$AUTHF" timeout 14 ./build/reverie -R cpu \
    --shot 3 "$OUT/auth" "$SCENE" > "$OUT/auth.log" 2>&1 || true
if [ -f "$OUT/auth.png" ]; then
    echo "    x11(auth):    PASS (cookie honored by an enforcing server)"
    PASS=$((PASS+1))
else
    echo "    x11(auth):    FAIL (refused by the auth server)"
    cat "$OUT/auth.log"
    FAIL=$((FAIL+1))
fi

# missing credentials: the refusal must carry the server's own reason —
# "Authorization required, but no authorization protocol specified" — not a
# truncated fragment (the field report's "Invalid )" was a misparsed reason)
: > "$OUT/empty-auth"
DISPLAY="$AUTHDISPLAY" XAUTHORITY="$OUT/empty-auth" timeout 10 \
    ./build/reverie -R cpu "$SCENE" > "$OUT/noauth.log" 2>&1 || true
if grep -q "no authorization protocol specified" "$OUT/noauth.log" 2>/dev/null; then
    echo "    x11(authmsg): PASS (server refusal reason surfaced verbatim)"
    PASS=$((PASS+1))
else
    echo "    x11(authmsg): FAIL (refusal reason wrong or missing)"
    cat "$OUT/noauth.log"
    FAIL=$((FAIL+1))
fi
kill "$XVFB_PID" 2>/dev/null
wait "$XVFB_PID" 2>/dev/null
XVFB_PID=""

# ---- Output hotplug: add + resize + remove (deterministic mon-file hook) ---
# REV_X11_MON_FILE drives the same re-enumeration path a real RandR screen
# change takes (x11_monitors is re-queried by the reconciler). One monitor
# becomes two at t=1.5s — AND the survivor changes size in the same rewrite
# (add + in-place resize in one pass). The --shot at t=4s must capture BOTH
# windows (FILE.i.png per active window when two or more are captured).
start_xvfb
MONF="$OUT/mon.list"
printf '0 0 1280 800\n' > "$MONF"
DISPLAY="$XDISPLAY" REV_X11_MON_FILE="$MONF" REV_HOTPLUG_S=1 \
    timeout 7 ./build/reverie -R cpu --shot 4 "$OUT/hp" "$SCENE" \
    > "$OUT/hp-add.log" 2>&1 &
DPID=$!
sleep 1.5
printf '0 0 1000 700\n1000 0 280 700\n' > "$MONF"
sleep 4                       # reconcile (1s cadence) + render + shot at t=4
kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
ok=1
grep -q "hotplug: monitor added" "$OUT/hp-add.log" 2>/dev/null || ok=0
grep -q "geometry now 1000x700" "$OUT/hp-add.log" 2>/dev/null || ok=0
[ -f "$OUT/hp.0.png" ] || ok=0
[ -f "$OUT/hp.1.png" ] || ok=0
if [ "$ok" = 1 ]; then
    echo "    x11(hp-add):  PASS (second output gained a window + survivor resized)"
    PASS=$((PASS+1))
else
    echo "    x11(hp-add):  FAIL (added monitor did not gain a rendering window)"
    cat "$OUT/hp-add.log"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Output hotplug: monitor removed mid-run ----------------------------------
# Two outputs become one at t=1.5s. The window is released, the slot
# tombstoned, and the shot at t=4s must be the SINGLE-window form (plain
# FILE.png — indexed names appear only while 2+ windows are captured).
start_xvfb
MONF="$OUT/mon.list"
printf '0 0 640 800\n640 0 640 800\n' > "$MONF"
DISPLAY="$XDISPLAY" REV_X11_MON_FILE="$MONF" REV_HOTPLUG_S=1 \
    timeout 7 ./build/reverie -R cpu --shot 4 "$OUT/hr" "$SCENE" \
    > "$OUT/hp-rem.log" 2>&1 &
DPID=$!
sleep 1.5
printf '0 0 640 800\n' > "$MONF"
sleep 4
kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
ok=1
grep -q "hotplug: monitor gone" "$OUT/hp-rem.log" 2>/dev/null || ok=0
[ -f "$OUT/hr.png" ] || ok=0
[ -f "$OUT/hr.0.png" ] && ok=0
[ -f "$OUT/hr.1.png" ] && ok=0
if [ "$ok" = 1 ]; then
    echo "    x11(hp-rem):  PASS (removed output's window released cleanly)"
    PASS=$((PASS+1))
else
    echo "    x11(hp-rem):  FAIL (removal did not reconcile the window set)"
    cat "$OUT/hp-rem.log"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Output hotplug on the OpenGL tier (EGL surface created mid-run) ----------
# Same add scenario on the GL renderer: the reconciler must build the new
# window's EGL surface at runtime (gl_add_window). Skipped like the other
# GL legs when no EGL stack is present.
start_xvfb
MONF="$OUT/mon.list"
printf '0 0 1280 800\n' > "$MONF"
DISPLAY="$XDISPLAY" env $GPU_ENV REV_X11_MON_FILE="$MONF" REV_HOTPLUG_S=1 \
    timeout 7 ./build/reverie -R opengl --shot 4 "$OUT/hpg" "$SCENE" \
    > "$OUT/hp-gl.log" 2>&1 &
DPID=$!
sleep 1.5
printf '0 0 640 800\n640 0 640 800\n' > "$MONF"
sleep 4
kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
if grep -q "renderer: opengl" "$OUT/hp-gl.log" 2>/dev/null; then
    ok=1
    grep -q "hotplug: monitor added" "$OUT/hp-gl.log" 2>/dev/null || ok=0
    [ -f "$OUT/hpg.1.png" ] || ok=0
    if [ "$ok" = 1 ]; then
        echo "    gl(hp-add):   PASS (runtime EGL surface on the new window)"
        PASS=$((PASS+1))
    else
        echo "    gl(hp-add):   FAIL (GL renderer missed the added monitor)"
        cat "$OUT/hp-gl.log"
        FAIL=$((FAIL+1))
    fi
else
    echo "    gl(hp-add):   SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Runtime IPC: theme switching, playlists, QUIT under a live daemon ------
# The daemon serves its control socket; revtool ipc drives a full session:
# PING/STATUS, a faded THEME switch, PLAYLIST + NEXT, and a clean QUIT that
# must shut the daemon down before the timeout kills it.
start_xvfb
IPCSOCK="$OUT/rev.sock"
DISPLAY="$XDISPLAY" REV_IPC_SOCK="$IPCSOCK" REV_FADE_MS=400 \
    timeout 24 ./build/reverie -R cpu --shot 10 "$OUT/ipc" "$SCENE" \
    > "$OUT/ipc.log" 2>&1 &
DPID=$!
sleep 2
./build/revtool ipc -s "$IPCSOCK" PING > "$OUT/ipc1.out" 2>&1
./build/revtool ipc -s "$IPCSOCK" STATUS > "$OUT/ipc2.out" 2>&1
./build/revtool ipc -s "$IPCSOCK" "THEME themes/animated-gradients 600" > "$OUT/ipc3.out" 2>&1
sleep 2                       # fade out+in (600ms each way)
./build/revtool ipc -s "$IPCSOCK" "PLAYLIST themes/ripple-lake themes/snowfall" > "$OUT/ipc4.out" 2>&1
./build/revtool ipc -s "$IPCSOCK" NEXT > "$OUT/ipc5.out" 2>&1
sleep 2
./build/revtool ipc -s "$IPCSOCK" "THEME /nonexistent/theme" > "$OUT/ipc6.out" 2>&1
sleep 4                       # screenshot at t=10s
./build/revtool ipc -s "$IPCSOCK" QUIT > "$OUT/ipc7.out" 2>&1
sleep 1
IPC_OK=1
grep -q "OK PONG" "$OUT/ipc1.out" 2>/dev/null || IPC_OK=0
grep -q "renderer=cpu" "$OUT/ipc2.out" 2>/dev/null || IPC_OK=0
grep -q "OK switched" "$OUT/ipc3.out" 2>/dev/null || IPC_OK=0
grep -q "ERR " "$OUT/ipc6.out" 2>/dev/null || IPC_OK=0     # bad theme -> ERR, daemon lives
grep -q "ipc: control socket" "$OUT/ipc.log" 2>/dev/null || IPC_OK=0
grep -q "ipc: theme 'themes/animated-gradients' active" "$OUT/ipc.log" 2>/dev/null || IPC_OK=0
grep -q "ipc: theme 'themes/snowfall' active" "$OUT/ipc.log" 2>/dev/null || IPC_OK=0
grep -q "shutting down" "$OUT/ipc.log" 2>/dev/null || IPC_OK=0
[ -f "$OUT/ipc.png" ] || IPC_OK=0
# QUIT must have ended the daemon before the timeout (process gone, clean log)
kill -0 $DPID 2>/dev/null && IPC_OK=0
# socket file cleaned up on shutdown
[ -S "$IPCSOCK" ] && IPC_OK=0
# per-user permission bits on the live socket were checked at start; re-verify
# via the log line only (the file is already unlinked after QUIT)
# session memory follows the RUNTIME switches: the record must hold the last
# switched-to theme (snowfall, absolute), not the boot scene (mountain-dusk)
grep -q "themes/snowfall" "$XDG_STATE_HOME/reverie/last-session.json" \
    2>/dev/null || IPC_OK=0
grep -q "mountain-dusk" "$XDG_STATE_HOME/reverie/last-session.json" \
    2>/dev/null && IPC_OK=0
if [ "$IPC_OK" = 1 ]; then
    echo "    ipc:          PASS (ping/status/faded switch/playlist/next/bad-theme/QUIT)"
    PASS=$((PASS+1))
else
    echo "    ipc:          FAIL (control session incomplete)"
    cat "$OUT/ipc.log" "$OUT/ipc1.out" "$OUT/ipc2.out" "$OUT/ipc3.out" \
        "$OUT/ipc4.out" "$OUT/ipc5.out" "$OUT/ipc6.out" "$OUT/ipc7.out" 2>/dev/null
    kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Hot reload: in-place scene edit while the daemon runs ------------------
# The watcher must notice an edit that never moves the directory's own
# mtime (snapshot walk over the theme files), reload without a restart,
# and STATUS must keep naming the theme afterwards. That last clause is
# the regression pin: the reload used to pass sf.path straight back into
# scene_load, whose tail snprintf(sf->path, ..., "%s", path) overlapped —
# glibc destroyed the string, and every STATUS after the first reload
# printed an empty theme= (fixed with a path snapshot inside scene_load).
start_xvfb
HRSOCK="$OUT/hr.sock"
DISPLAY="$XDISPLAY" REV_IPC_SOCK="$HRSOCK" \
    timeout 18 ./build/reverie -R cpu themes/snowfall \
    > "$OUT/hr.log" 2>&1 &
HRPID=$!
sleep 2
./build/revtool ipc -s "$HRSOCK" STATUS > "$OUT/hr1.out" 2>&1
touch themes/snowfall/scene.rev          # in-place edit: content intact, mtime moves
sleep 2
./build/revtool ipc -s "$HRSOCK" STATUS > "$OUT/hr2.out" 2>&1
./build/revtool ipc -s "$HRSOCK" QUIT > "$OUT/hr3.out" 2>&1
sleep 1
HR_OK=1
grep -q "theme=themes/snowfall" "$OUT/hr1.out" 2>/dev/null || HR_OK=0
grep -q "reload ok" "$OUT/hr.log" 2>/dev/null || HR_OK=0
grep -q "reload failed" "$OUT/hr.log" 2>/dev/null && HR_OK=0
grep -q "theme=themes/snowfall" "$OUT/hr2.out" 2>/dev/null || HR_OK=0
kill -0 $HRPID 2>/dev/null && HR_OK=0
if [ "$HR_OK" = 1 ]; then
    echo "    hotreload:    PASS (in-place edit reloads; STATUS theme= survives)"
    PASS=$((PASS+1))
else
    echo "    hotreload:    FAIL (reload missed or STATUS theme= empty after reload)"
    cat "$OUT/hr.log" "$OUT/hr1.out" "$OUT/hr2.out" 2>/dev/null
    kill $HRPID 2>/dev/null; wait $HRPID 2>/dev/null
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Autostart: entry lifecycle + duplicate-instance guard -----------------
# revtool autostart runs against a private XDG_CONFIG_HOME/XDG_STATE_HOME:
# enable (NO arguments — the --restore contract) writes the single .desktop
# entry whose Exec line is "<bin> --restore --exit-if-running"; the argument
# memory of the old mechanism is gone (enable with arguments is refused with
# the redirect, disable cleans a leftover autostart.json). status reports
# what --restore would replay. The guard itself runs LIVE below: a daemon
# owns the socket, a second launch with --exit-if-running must yield cleanly.
{
    AROOT="$OUT/xdg-home"
    ASTATE="$OUT/as-state"
    mkdir -p "$AROOT" "$ASTATE"
    AOK=1
    XDG_CONFIG_HOME="$AROOT" XDG_STATE_HOME="$ASTATE" \
        ./build/revtool autostart enable >/dev/null 2>&1 || AOK=0
    [ -f "$AROOT/autostart/reverie.desktop" ] || AOK=0
    grep -q "Exec=.*reverie.*--restore --exit-if-running" \
        "$AROOT/autostart/reverie.desktop" 2>/dev/null || AOK=0
    grep -q -- "--scene\|--video" \
        "$AROOT/autostart/reverie.desktop" 2>/dev/null && AOK=0
    [ -f "$AROOT/reverie/autostart.json" ] && AOK=0
    XDG_CONFIG_HOME="$AROOT" XDG_STATE_HOME="$ASTATE" \
        ./build/revtool autostart enable --scene "$SCENE" \
        >/dev/null 2>&1 && AOK=0
    XDG_CONFIG_HOME="$AROOT" XDG_STATE_HOME="$ASTATE" \
        ./build/revtool autostart status 2>/dev/null \
        | grep -q "enabled: yes" || AOK=0
    XDG_CONFIG_HOME="$AROOT" XDG_STATE_HOME="$ASTATE" \
        ./build/revtool autostart status 2>/dev/null \
        | grep -q "nothing recorded yet" || AOK=0
    # a planted record shows up as what --restore would replay
    mkdir -p "$ASTATE/reverie"
    printf '{"version":1,"argv":["--scene","/tmp/planted-theme"]}' \
        > "$ASTATE/reverie/last-session.json"
    XDG_CONFIG_HOME="$AROOT" XDG_STATE_HOME="$ASTATE" \
        ./build/revtool autostart status 2>/dev/null \
        | grep -q -- "restores: --scene /tmp/planted-theme" || AOK=0
    # a leftover from the OLD mechanism (argument memory) must be cleaned
    # (mkdir: enable no longer creates $XDG_CONFIG_HOME/reverie — only the
    # old mechanism did, so the legacy file has to be planted by hand)
    mkdir -p "$AROOT/reverie"
    printf '{"args":["--scene","old.rev"]}' > "$AROOT/reverie/autostart.json"
    [ -f "$AROOT/reverie/autostart.json" ] || AOK=0
    XDG_CONFIG_HOME="$AROOT" XDG_STATE_HOME="$ASTATE" \
        ./build/revtool autostart disable >/dev/null 2>&1 || AOK=0
    [ -f "$AROOT/autostart/reverie.desktop" ] && AOK=0
    [ -f "$AROOT/reverie/autostart.json" ] && AOK=0
    if [ "$AOK" = 1 ]; then
        echo "    autostart(cfg):PASS (enable/status/disable, --restore exec, legacy memory cleaned)"
        PASS=$((PASS+1))
    else
        echo "    autostart(cfg):FAIL (entry lifecycle broken)"
        FAIL=$((FAIL+1))
    fi
}

start_xvfb
DUPSOCK="$OUT/dup.sock"
DISPLAY="$XDISPLAY" REV_IPC_SOCK="$DUPSOCK" timeout 12 ./build/reverie -R cpu "$SCENE" \
    > "$OUT/dup.log" 2>&1 &
DPID=$!
sleep 2
DISPLAY="$XDISPLAY" REV_IPC_SOCK="$DUPSOCK" timeout 8 ./build/reverie -R cpu "$SCENE" \
    --exit-if-running > "$OUT/dup2.log" 2>&1
DRC=$?
kill $DPID 2>/dev/null; wait $DPID 2>/dev/null
stop_xvfb
if [ "$DRC" -eq 0 ] && grep -q "another reverie daemon owns the control socket" \
        "$OUT/dup2.log" 2>/dev/null; then
    echo "    autostart(dup):PASS (second launch yields to the live daemon)"
    PASS=$((PASS+1))
else
    echo "    autostart(dup):FAIL (--exit-if-running guard, exit=$DRC)"
    cat "$OUT/dup2.log"
    FAIL=$((FAIL+1))
fi

# ---- --restore: the last-launched memory, live -------------------------------
# 1. restore(record): a daemon launch writes the record with the ABSOLUTE
#    content path (the relative CLI path must not leak in — a login session
#    has a different cwd)
# 2. restore(replay): `reverie --restore` with no path loads that exact
#    scene and reaches the running state
# 3. restore(empty): a clean, named failure when nothing was ever launched
# 4. restore(conflict): --restore refuses an explicit path on the same line
start_xvfb
RSOK=1
RST="$OUT/restore-state"
mkdir -p "$RST"
DISPLAY="$XDISPLAY" XDG_STATE_HOME="$RST" timeout 6 ./build/reverie -R cpu \
    "$SCENE" > "$OUT/res1.log" 2>&1 || true
[ -f "$RST/reverie/last-session.json" ] || RSOK=0
grep -q -- "\"$PWD/$SCENE\"" \
    "$RST/reverie/last-session.json" 2>/dev/null || RSOK=0
DISPLAY="$XDISPLAY" XDG_STATE_HOME="$RST" timeout 6 ./build/reverie -R cpu \
    --restore > "$OUT/res2.log" 2>&1 || true
grep -q "restore: replaying the last session" "$OUT/res2.log" 2>/dev/null || RSOK=0
grep -q "scene '$PWD/$SCENE'" "$OUT/res2.log" 2>/dev/null || RSOK=0
grep -q "x11: DISPLAY is not set" "$OUT/res2.log" 2>/dev/null && RSOK=0
EMPTY="$OUT/restore-empty"
mkdir -p "$EMPTY"
XDG_STATE_HOME="$EMPTY" ./build/reverie --restore > "$OUT/res3.log" 2>&1
RRC=$?
[ "$RRC" -ne 0 ] || RSOK=0
grep -q "nothing to restore" "$OUT/res3.log" 2>/dev/null || RSOK=0
XDG_STATE_HOME="$RST" ./build/reverie --restore --scene "$SCENE" \
    > "$OUT/res4.log" 2>&1
RRC2=$?
[ "$RRC2" -ne 0 ] || RSOK=0
grep -q "cannot be combined with" "$OUT/res4.log" 2>/dev/null || RSOK=0
stop_xvfb
if [ "$RSOK" = 1 ]; then
    echo "    restore:       PASS (record absolute / replay / empty refusal / conflict)"
    PASS=$((PASS+1))
else
    echo "    restore:       FAIL (session memory broken)"
    cat "$OUT/res1.log" "$OUT/res2.log" "$OUT/res3.log" "$OUT/res4.log" \
        2>/dev/null
    FAIL=$((FAIL+1))
fi

echo "==================================================="
echo "e2e: $PASS passed, $SKIP skipped (no driver), $FAIL failed"
echo "logs: $OUT"
[ "$FAIL" -eq 0 ]
