#!/usr/bin/env python3
"""C3 patcher: adds rev_scale_place() (media scaling math) to geom.c/h.

geom.c is TAB-indented — byte-exact python splice, no tab normalization.
"""

import sys

GEOM_H = "src/geom.h"
GEOM_C = "src/geom.c"

# ---- geom.h: declaration after the transform block ------------------------
with open(GEOM_H, "rb") as f:
    h = f.read().decode("utf-8")

ANCHOR_H = "RMat rmat_skew_x(f64 radians);\nRMat rmat_skew_y(f64 radians);\n"
DECL_H = ANCHOR_H + """
/* ---- media scaling (the --video modes) ---------------------------------
 * Placement of an iw x ih source inside a W x H viewport, in DEVICE
 * pixels. fill covers and center-crops (negative offsets are legal and
 * mean "cropped by the viewport"); fit contains and centers (the bars
 * around it are the caller's clear color); stretch ignores aspect;
 * center keeps native size, centered, cropping symmetrically when the
 * source exceeds the viewport. Kept separate from the scene layer `fit`
 * vocabulary on purpose: scenes own their coordinate semantics.
 * ------------------------------------------------------------------------*/
typedef struct {
\tu32 w, h;            /* placed size                                  */
\ti32 dx, dy;          /* top-left offset into the viewport (may be <0) */
} RScaleRect;

RScaleRect rev_scale_place(int mode, u32 W, u32 H, u32 iw, u32 ih);
"""
assert h.count(ANCHOR_H) == 1, "geom.h anchor not found"
h = h.replace(ANCHOR_H, DECL_H, 1)
with open(GEOM_H, "wb") as f:
    f.write(h.encode("utf-8"))

# ---- geom.c: implementation appended at the end ---------------------------
IMPL_C = """
/* ---- media scaling (the --video modes) ------------------------------------ */

RScaleRect rev_scale_place(int mode, u32 W, u32 H, u32 iw, u32 ih)
{
\tRScaleRect r;
\tmemset(&r, 0, sizeof r);
\tif (!W || !H || !iw || !ih)
\t\treturn r;
\tconst f64 fw = (f64)W, fh = (f64)H;
\tconst f64 sw = (f64)iw, sh = (f64)ih;
\tswitch (mode) {
\tcase REV_SCALE_STRETCH:
\t\tr.w = W;
\t\tr.h = H;
\t\tr.dx = 0;
\t\tr.dy = 0;
\t\tbreak;
\tcase REV_SCALE_CENTER:
\t\tr.w = iw;
\t\tr.h = ih;
\t\tr.dx = (i32)((fw - sw) / 2.0);
\t\tr.dy = (i32)((fh - sh) / 2.0);
\t\tbreak;
\tcase REV_SCALE_FIT:
\tdefault: { /* REV_SCALE_FILL */
\t\tf64 s = (mode == REV_SCALE_FIT)
\t\t            ? (fw / sw < fh / sh ? fw / sw : fh / sh)
\t\t            : (fw / sw > fh / sh ? fw / sw : fh / sh);
\t\tu32 pw = (u32)(sw * s + 0.5);
\t\tu32 ph = (u32)(sh * s + 0.5);
\t\tif (pw < 1) pw = 1;
\t\tif (ph < 1) ph = 1;
\t\tr.w = pw;
\t\tr.h = ph;
\t\tr.dx = (i32)((fw - (f64)pw) / 2.0);
\t\tr.dy = (i32)((fh - (f64)ph) / 2.0);
\t\tbreak;
\t}
\t}
\treturn r;
}
"""

with open(GEOM_C, "rb") as f:
    c = f.read().decode("utf-8")

if "rev_scale_place" in c:
    sys.exit("geom.c already patched")

# include cli.h for the REV_SCALE_* enum (mode arrives as int)
OLD_INC = "#include <string.h>\n"
NEW_INC = '#include <string.h>\n\n#include "cli.h"\n'
assert c.count(OLD_INC) >= 1, "geom.c include anchor not found"
c = c.replace(OLD_INC, NEW_INC, 1)

c = c.rstrip("\n") + "\n" + IMPL_C
with open(GEOM_C, "wb") as f:
    f.write(c.encode("utf-8"))
print("patched: rev_scale_place added to geom.c/h")
