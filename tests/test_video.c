/* ============================================================================
 * Reverie Engine — test_video.c
 * Video wallpapers: decoder lifecycle, playback advance, looping, failure
 * modes. Generates its fixture with the ffmpeg CLI when available and
 * skips the decode section otherwise (the stub behavior is always tested).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rev_types.h"
#include "video.h"

static int fails = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fails++; \
		printf("FAIL: " __VA_ARGS__); \
		printf(" (%s:%d)\n", __FILE__, __LINE__); \
	} else { \
		printf("  ok: " __VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ---- forced-unavailable regression leg --------------------------------------- *
 * The first CI run (2026-10-15) exposed a contract bug: when the build's
 * vendored FFmpeg headers (7.1) met the runner's FFmpeg 6.1 libraries, the
 * runtime ABI guard refused video (correct) — but rev_video_open's refusal
 * text did not carry the "no video support" anchor the no-headers stub
 * build (and this test) expect. This leg forces the unavailable state
 * deterministically on ANY machine: an LD_LIBRARY_PATH directory of empty
 * shared objects shadows every soname the loader can try, the function
 * table load fails, and the re-exec'd child must receive the standard
 * refusal. Environment-independent — runs wherever cc exists. */
static int stub_refusal_child(void)
{
	char err[512] = {0};
	RVideoSource src = { .path = "/nonexistent/file.mp4" };
	RevVideo *v = rev_video_open(&src, true, err, sizeof err);
	bool ok = v == NULL && strstr(err, "no video support");
	printf("%s: forced-unavailable refusal carries \"no video support\" (%s)\n",
	       ok ? "  ok" : "FAIL", err);
	return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
	/* re-exec'd child of the stub-refusal leg below (shadowed sonames) */
	if (argc > 1 && !strcmp(argv[1], "--stub-refusal"))
		return stub_refusal_child();

	char err[512];

	/* availability is environment-dependent: libraries + ABI match */
	bool have_video = rev_video_available();
	printf("  ok: availability probed (have_video=%s)\n", have_video ? "yes" : "no");

	/* missing file: never a crash, always a message */
	{
		RVideoSource src = { .path = "/nonexistent/file.mp4" };
		if (have_video) {
			RevVideo *v = rev_video_open(&src, true, err, sizeof err);
			CHECK(v == NULL && err[0], "missing file refused (%s)", err);
		} else {
			RevVideo *v = rev_video_open(&src, true, err, sizeof err);
			CHECK(v == NULL && strstr(err, "no video support"),
			      "stub refuses with the build explanation");
		}
	}

	/* generate a fixture with the system ffmpeg CLI */
	char mp4[256];
	snprintf(mp4, sizeof mp4, "/tmp/revtest_%d.mp4", (int)getpid());
	char cmd[512];
	snprintf(cmd, sizeof cmd,
	         "ffmpeg -y -loglevel error -f lavfi -i testsrc2=size=320x180:rate=30 "
	         "-t 2 -pix_fmt yuv420p -c:v libx264 -preset ultrafast '%s' 2>/dev/null",
	         mp4);
	bool fixture = system(cmd) == 0;

	if (!fixture) {
		printf("  ok: no ffmpeg CLI - decode tests skipped\n");
	} else if (!have_video) {
		printf("  ok: fixture present but no libraries - decode tests skipped\n");
	} else {
		/* decode + dimensions + playback advance + loop + pause */
		RVideoSource src = { .path = mp4 };
		RevVideo *v = rev_video_open(&src, true, err, sizeof err);
		CHECK(v != NULL, "fixture opens (%s)", v ? "" : err);
		if (v) {
			const RVideoFrame *f = rev_video_display(v, 0.0);
			CHECK(f == NULL || f->w == 320, "dimensions sane");

			/* advance ~3 seconds in 1/30s steps at real-time pace
			 * (the decode thread needs wall-clock time to fill the
			 * queue; the daemon's frame pacing provides it) */
			int frames_seen = 0;
			u32 last_w = 0;
			for (int i = 0; i < 90; i++) {
				f = rev_video_display(v, 1.0 / 30.0);
				if (f && f->w) {
					frames_seen++;
					last_w = f->w;
				}
				usleep(1000);
			}
			CHECK(frames_seen > 30, "playback advances (%d display frames)",
			      frames_seen);
			CHECK(last_w == 320, "frame descriptor stays valid");

			/* loop past EOF: playhead 2s video + more steps still yields frames */
			int post_eof = 0;
			for (int i = 0; i < 90; i++) {
				f = rev_video_display(v, 1.0 / 30.0);
				if (f && f->w)
					post_eof++;
				usleep(1000);
			}
			CHECK(post_eof > 30, "looping continues past EOF (%d frames)",
			      post_eof);

			/* pause: dt=0 must not advance (frames still valid) */
			f = rev_video_display(v, 0.0);
			CHECK(f != NULL, "paused display still returns the held frame");

			rev_video_close(v);
			printf("  ok: clean close (queue lifetime bug regression covered "
			       "by the process not aborting here)\n");
		}

		/* memory source (packed-theme path): same file, AVIO over a buffer */
		{
			size_t len = 0;
			u8 *data = NULL;
			FILE *fp = fopen(mp4, "rb");
			if (fp) {
				fseek(fp, 0, SEEK_END);
				len = (size_t)ftell(fp);
				fseek(fp, 0, SEEK_SET);
				data = (u8 *)malloc(len);
				if (data && fread(data, 1, len, fp) != len)
					len = 0;
				fclose(fp);
			}
			CHECK(data && len > 1000, "fixture read for the memory source");
			if (data && len > 1000) {
				RVideoSource msrc = { .data = data, .len = len };
				RevVideo *mv = rev_video_open(&msrc, true, err, sizeof err);
				CHECK(mv != NULL, "memory source opens (AVIO) (%s)",
				      mv ? "" : err);
				if (mv) {
					int seen = 0;
					for (int i = 0; i < 60; i++) {
						const RVideoFrame *f = rev_video_display(mv, 1.0 / 30.0);
						if (f && f->w)
							seen++;
						usleep(1000);
					}
					CHECK(seen > 20, "memory source decodes (%d frames)", seen);
					rev_video_close(mv);
				}
				free(data);
			}
		}
	}


	/* stub-refusal leg — deterministic on every machine: shadow every soname
	 * the loader can try with an empty shared object, re-exec ourselves under
	 * that LD_LIBRARY_PATH, and require the standard refusal anchor */
	{
		char dir[] = "/tmp/revvidstubXXXXXX";
		char self[300];
		ssize_t nself = readlink("/proc/self/exe", self, sizeof self - 1);
		if (nself <= 0 || !mkdtemp(dir)) {
			CHECK(false, "stub leg setup (readlink/mkdtemp)");
		} else {
			char scmd[768];
			int rc;
			self[nself] = '\0';
			snprintf(scmd, sizeof scmd,
				 "cd '%s' && cc -shared -fPIC -o libstub.so -x c /dev/null 2>/dev/null && "
				 "for n in libavcodec.so.61 libavcodec.so.60 libavcodec.so.59 "
				 "libavcodec.so.58 libavcodec.so libavformat.so.61 libavformat.so.60 "
				 "libavformat.so.59 libavformat.so.58 libavformat.so libavutil.so.59 "
				 "libavutil.so.58 libavutil.so.57 libavutil.so.56 libavutil.so "
				 "libswscale.so.8 libswscale.so.7 libswscale.so.6 libswscale.so.5 "
				 "libswscale.so; do ln -sf libstub.so \"$n\"; done",
				 dir);
			rc = system(scmd);
			CHECK(rc == 0, "stub leg: loader sonames shadowed with empty .so");
			if (rc == 0) {
				snprintf(scmd, sizeof scmd,
					 "env LD_LIBRARY_PATH='%s' '%s' --stub-refusal >'%s/out' 2>&1",
					 dir, self, dir);
				rc = system(scmd);
				CHECK(rc == 0,
				      "forced-unavailable refusal carries \"no video support\"");
				if (rc != 0) {
					snprintf(scmd, sizeof scmd, "cat '%s/out' 2>/dev/null", dir);
					system(scmd); /* surface the child's diagnostics */
				}
			}
			snprintf(scmd, sizeof scmd, "rm -rf '%s'", dir);
			system(scmd);
		}
	}

	remove(mp4);
	if (fails) {
		printf("video: %d failure(s)\n", fails);
		return 1;
	}
	printf("video: OK\n");
	return 0;
}
