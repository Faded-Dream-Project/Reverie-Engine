#!/usr/bin/env python3
"""C2 patcher: adds GIF/RGB-family legs to tests/test_video.c.

test_video.c is TAB-indented — the edit tool would rewrite every tab to
spaces, so this patcher does byte-exact anchored inserts instead.
"""

PATH = "tests/test_video.c"

with open(PATH, "rb") as f:
    src = f.read().decode("utf-8")

# ---- 1) h264 plane-path guard: track saw_rgb in the playback loop --------
OLD1 = """\t\t\tint frames_seen = 0;
\t\t\tu32 last_w = 0;
\t\t\tfor (int i = 0; i < 90; i++) {
\t\t\t\tf = rev_video_display(v, 1.0 / 30.0);
\t\t\t\tif (f && f->w) {
\t\t\t\t\tframes_seen++;
\t\t\t\t\tlast_w = f->w;
\t\t\t\t}
\t\t\t\tusleep(1000);
\t\t\t}
\t\t\tCHECK(frames_seen > 30, "playback advances (%d display frames)",
\t\t\t      frames_seen);
"""
NEW1 = """\t\t\tint frames_seen = 0;
\t\t\tu32 last_w = 0;
\t\t\tbool saw_rgb = false;
\t\t\tfor (int i = 0; i < 90; i++) {
\t\t\t\tf = rev_video_display(v, 1.0 / 30.0);
\t\t\t\tif (f && f->w) {
\t\t\t\t\tframes_seen++;
\t\t\t\t\tlast_w = f->w;
\t\t\t\t\tif (f->rgb)
\t\t\t\t\t\tsaw_rgb = true;
\t\t\t\t}
\t\t\t\tusleep(1000);
\t\t\t}
\t\t\tCHECK(frames_seen > 30, "playback advances (%d display frames)",
\t\t\t      frames_seen);
\t\t\tCHECK(!saw_rgb, "h264 fixture stays on the YUV plane path");
"""
assert src.count(OLD1) == 1, "anchor 1 (h264 loop) not found"
src = src.replace(OLD1, NEW1, 1)

# ---- 2) GIF legs: insert between the mp4 section and the stub-refusal ----
ANCHOR2 = """\t\t\t\tfree(data);
\t\t\t}
\t\t}
\t}


\t/* stub-refusal leg — deterministic on every machine: shadow every soname"""
INSERT2 = """\t\t\t\tfree(data);
\t\t\t}
\t\t}
\t}

\t/* ---- RGB-family sources: animated GIF, transparency, stride --------- *
\t * Fixture: a hand-crafted 80x60 two-frame GIF — frame 0 fully
\t * transparent, frame 1 opaque red, 0.5 s delays (regenerate with
\t * scripts/mk_gif_fixture.py). Real-world decoders may hand us PAL8,
\t * BGRA, or RGBA depending on the FFmpeg build — normalize_rgb must
\t * turn every one of those into packed engine-order RGBA with the
\t * transparency intact. */
\t{
\t\tconst char *gif = "tests/fixtures/gif_anim_transparent.gif";
\t\tif (access(gif, R_OK) != 0) {
\t\t\tCHECK(false, "gif fixture present (scripts/mk_gif_fixture.py)");
\t\t} else if (!have_video) {
\t\t\tprintf("  ok: gif fixture present, no libraries - gif legs skipped\\n");
\t\t} else {
\t\t\tRVideoSource src = { .path = gif };
\t\t\tRevVideo *v = rev_video_open(&src, true, err, sizeof err);
\t\t\tCHECK(v != NULL, "gif opens (%s)", v ? "" : err);
\t\t\tif (v) {
\t\t\t\t/* frame 0: the transparent one, on the rgb path */
\t\t\t\tconst RVideoFrame *f = NULL;
\t\t\t\tfor (int i = 0; i < 40 && !f; i++) {
\t\t\t\t\tf = rev_video_display(v, 0.016);
\t\t\t\t\tusleep(2000);
\t\t\t\t}
\t\t\t\tCHECK(f != NULL && f->rgb && f->w == 80 && f->h == 60,
\t\t\t\t      "gif frame arrives as rgb (%ux%u)",
\t\t\t\t      f ? f->w : 0, f ? f->h : 0);
\t\t\t\tif (f && f->rgb) {
\t\t\t\t\tstatic u32 px[80 * 60];
\t\t\t\t\tCHECK(rev_video_to_rgba(f, px, 80, 60),
\t\t\t\t\t      "gif converts to rgba");
\t\t\t\t\tbool all_trans = true;
\t\t\t\t\tfor (int i = 0; i < 80 * 60; i++)
\t\t\t\t\t\tif (px[i] & 0xFF000000u)
\t\t\t\t\t\t\tall_trans = false;
\t\t\t\t\tCHECK(all_trans, "gif frame 0 is fully transparent");
\t\t\t\t}

\t\t\t\t/* frame 1: opaque red — pts must honor the 0.5 s delays, and
\t\t\t\t * the channel order must be the engine's (0xFF0000FF) */
\t\t\t\tstatic u32 px2[80 * 60];
\t\t\t\tbool red = false;
\t\t\t\tbool all_red = false;
\t\t\t\tfor (int i = 0; i < 100 && !red; i++) {
\t\t\t\t\tf = rev_video_display(v, 0.016);
\t\t\t\t\tif (f && f->rgb && rev_video_to_rgba(f, px2, 80, 60) &&
\t\t\t\t\t    (px2[80 * 30 + 40] & 0xFF000000u) != 0) {
\t\t\t\t\t\tred = px2[80 * 30 + 40] == 0xFF0000FFu;
\t\t\t\t\t\tall_red = true;
\t\t\t\t\t\tfor (int k = 0; k < 80 * 60; k++)
\t\t\t\t\t\t\tif (px2[k] != 0xFF0000FFu)
\t\t\t\t\t\t\t\tall_red = false;
\t\t\t\t\t}
\t\t\t\t\tusleep(2000);
\t\t\t\t}
\t\t\t\tCHECK(red && all_red,
\t\t\t\t      "gif frame 1 is opaque red in engine byte order");

\t\t\t\t/* stride placement: convert into an offset rect of a wider
\t\t\t\t * buffer — the letterbox path depends on it */
\t\t\t\tif (f && f->rgb) {
\t\t\t\t\tstatic u32 wide[100 * 60];
\t\t\t\t\tmemset(wide, 0xCD, sizeof wide);
\t\t\t\t\tbool ok = rev_video_to_rgba_at(f, wide + 5, 100, 80, 60);
\t\t\t\t\tCHECK(ok && wide[0] == 0xCDCDCDCDu,
\t\t\t\t\t      "to_rgba_at leaves the left margin untouched");
\t\t\t\t\tCHECK(ok && wide[100 * 59 + 5] == 0xFF0000FFu,
\t\t\t\t\t      "to_rgba_at writes with dst_stride rows");
\t\t\t\t}

\t\t\t\t/* looping: the gif demuxer has no seek support — the reopen
\t\t\t\t * fallback must wrap playback back to frame 0 */
\t\t\t\tint seen = 0;
\t\t\t\tfor (int i = 0; i < 120; i++) {
\t\t\t\t\tconst RVideoFrame *lf = rev_video_display(v, 0.016);
\t\t\t\t\tif (lf && lf->w)
\t\t\t\t\t\tseen++;
\t\t\t\t\tusleep(2000);
\t\t\t\t}
\t\t\t\tCHECK(seen > 60, "gif loops past EOF (%d frames)", seen);

\t\t\t\trev_video_close(v);
\t\t\t\tprintf("  ok: gif clean close\\n");
\t\t\t}
\t\t}
\t}


\t/* stub-refusal leg — deterministic on every machine: shadow every soname"""

assert src.count(ANCHOR2) == 1, "anchor 2 (insertion point) not found"
src = src.replace(ANCHOR2, INSERT2, 1)

with open(PATH, "wb") as f:
    f.write(src.encode("utf-8"))
print("patched: h264 plane guard + gif legs added (%d tabs intact: %d)"
      % (src.count("\t"), src.count("\t")))
