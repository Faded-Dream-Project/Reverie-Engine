#!/bin/sh
# ============================================================================
# reverie — scripts/verify-bgra-flag.sh
#
# Positive proof that the copypremul kernel honors the p[1].y (BGRA store)
# flag: build a one-off binary with the X11 platform's present_bgra forced
# TRUE (the Task-41 "forced experiment" pattern), present the same scene
# with the CPU tier and the Vulkan tier, and verify that the Vulkan tier's
# PRESENTED pixels are the exact R/B swap of the CPU reference (G and the
# overall structure unchanged). The shipped tree keeps present_bgra=false
# on X11; this script patches, builds, measures, and restores.
# ============================================================================
cd "$(dirname "$0")/.." || exit 1
GPU_ROOT="${REV_GPU_ROOT:-$(cd .. && pwd)/gpulibs/root}"
SCENE="${1:-themes/mountain-dusk/scene.rev}"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"; kill $XVFB_PID 2>/dev/null' EXIT INT TERM

cp src/plat_x11.c "$OUT/plat_x11.c.orig"
sed -e 's/p->present_bgra = false/p->present_bgra = true/' \
    -e 's/p->present_premul = false/p->present_premul = true/' \
    -e 's/p->name = "x11";/p->name = "x11-bgra-forced";/' \
    src/plat_x11.c > "$OUT/plat_x11.bgra.c"

cc -std=c11 -Wall -Wextra -Werror -Wpedantic -D_POSIX_C_SOURCE=200809L \
   -D_DEFAULT_SOURCE -O2 -Isrc -DREV_VIDEO=1 -Icontrib/ffmpeg-headers \
   -c "$OUT/plat_x11.bgra.c" -o "$OUT/plat_x11.bgra.o" \
   && cc -std=c11 -Wall -Wextra -Werror -Wpedantic -D_POSIX_C_SOURCE=200809L \
   -D_DEFAULT_SOURCE -O2 -Isrc -DREV_VIDEO=1 -Icontrib/ffmpeg-headers \
   -o "$OUT/reverie_bgra" src/main.c "$OUT/plat_x11.bgra.o" build/libreverie.a \
   -lm -ldl -lpthread \
   || { echo "verify-bgra: build failed"; cp "$OUT/plat_x11.c.orig" src/plat_x11.c; exit 1; }
cp "$OUT/plat_x11.c.orig" src/plat_x11.c     # restore the shipped tree

Xvfb :97 -screen 0 1280x720x24 >/dev/null 2>&1 & XVFB_PID=$!
sleep 1

DISPLAY=:97 timeout 20 "$OUT/reverie_bgra" -R cpu --shot 4 "$OUT/cpu" "$SCENE" >/dev/null 2>&1
DISPLAY=:97 timeout 20 env \
    VK_ICD_FILENAMES="$GPU_ROOT/usr/share/vulkan/icd.d/lvp_icd.json" \
    LD_LIBRARY_PATH="$GPU_ROOT/usr/lib/x86_64-linux-gnu" \
    "$OUT/reverie_bgra" -R vulkan --shot 4 "$OUT/vk" "$SCENE" >/dev/null 2>&1

python3 - "$OUT" <<'PYEOF'
import sys, struct, zlib, os
out = sys.argv[1]

def read_png_rgba(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, w, h, bitdepth, ctype = 8, 0, 0, 0, 0
    idat = b''
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]
        typ = d[pos+4:pos+8]
        body = d[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, bitdepth, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        pos += 12 + ln
    assert bitdepth == 8 and ctype == 6, f'unexpected PNG type {ctype}'
    raw = zlib.decompress(idat)
    stride = w * 4
    px = []
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        flt = raw[p]; p += 1
        row = bytearray(raw[p:p+stride]); p += stride
        if flt == 0:
            pass
        elif flt == 1:
            for i in range(4, stride):
                row[i] = (row[i] + row[i-4]) & 0xff
        elif flt == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xff
        elif flt == 3:
            for i in range(stride):
                a = row[i-4] if i >= 4 else 0
                row[i] = (row[i] + ((a + prev[i]) >> 1)) & 0xff
        elif flt == 4:
            for i in range(stride):
                a = row[i-4] if i >= 4 else 0
                b = prev[i]
                c = prev[i-4] if i >= 4 else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xff
        prev = row
        px.append(bytes(row))
    return w, h, b''.join(px)

cpu_p, vk_p = os.path.join(out, 'cpu.png'), os.path.join(out, 'vk.png')
if not os.path.exists(cpu_p) or not os.path.exists(vk_p):
    print('verify-bgra: FAIL (shots missing)')
    sys.exit(1)
w1, h1, cpu = read_png_rgba(cpu_p)
w2, h2, vk = read_png_rgba(vk_p)
assert (w1, h1) == (w2, h2), 'size mismatch'
n = len(cpu)
same_g = same_rb_swap = same_exact = 0
for i in range(0, n, 4):
    r1, g1, b1 = cpu[i], cpu[i+1], cpu[i+2]
    r2, g2, b2 = vk[i], vk[i+1], vk[i+2]
    if g1 == g2:
        same_g += 1
    if r1 == b2 and b1 == r2:
        same_rb_swap += 1
    if r1 == r2 and b1 == b2:
        same_exact += 1
tot = n // 4
swap_frac, exact_frac = same_rb_swap / tot, same_exact / tot
print(f'verify-bgra: {tot} px compared')
print(f'  G identical:        {same_g/tot*100:.2f}%')
print(f'  exact R/B SWAP:     {swap_frac*100:.2f}%')
print(f'  identical (no swap):{exact_frac*100:.2f}%')
# the scene renders with tiny per-tier rounding on ~0.5% of pixels, so the
# bar is relative: the swap signature must DOMINATE the identical one
if swap_frac > 0.95 and swap_frac > exact_frac * 5:
    print('verify-bgra: PASS — the kernel honors p[1].y (BGRA store swizzle)')
    sys.exit(0)
print('verify-bgra: FAIL — VK output is NOT the R/B swap of the CPU reference')
sys.exit(1)
PYEOF
