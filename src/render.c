/* ============================================================================
 * Reverie Engine — render.c
 * Renderer chain selection. Probes in order (vulkan -> opengl -> cpu),
 * init()ing each candidate against the live platform; the first success
 * wins. Every failure is logged with its reason so `--renderer auto`
 * decisions are auditable in the daemon log.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "render.h"
#include "render_cpu.h"
#include "render_gl.h"
#include "render_vk.h"

typedef RRenderer *(*RevRendererCtor)(void);

static bool try_init(RRenderer *r, RPlatform *p, char *why, size_t cap)
{
	char err[256] = { 0 };
	if (r->init(r, p, err, sizeof err))
		return true;
	snprintf(why, cap, "%s: %s", r->name, err[0] ? err : "init failed");
	return false;
}

RRenderer *renderer_create(const char *pref, RPlatform *p, char *err, size_t errcap)
{
	const bool want_vk = !pref || !strcmp(pref, "auto") || !strcmp(pref, "vulkan");
	const bool want_gl = !pref || !strcmp(pref, "auto") || !strcmp(pref, "opengl") || !strcmp(pref, "gl");
	const bool want_cpu = true;

	char why[256] = { 0 };
	RRenderer *r = NULL;

	if (want_vk) {
		r = render_vk_create();
		if (r && try_init(r, p, why, sizeof why)) {
			LOGI("renderer: %s (tier %d)", r->name, r->tier);
			return r;
		}
		renderer_destroy(r);
		r = NULL;
		LOGI("renderer: vulkan unavailable (%s)", why);
	}

	if (want_gl) {
		r = render_gl_create();
		if (r && try_init(r, p, why, sizeof why)) {
			LOGI("renderer: %s (tier %d)", r->name, r->tier);
			return r;
		}
		renderer_destroy(r);
		r = NULL;
		LOGI("renderer: opengl unavailable (%s)", why);
	}

	if (want_cpu) {
		r = render_cpu_create();
		if (r && try_init(r, p, why, sizeof why)) {
			LOGI("renderer: %s (tier %d)", r->name, r->tier);
			return r;
		}
		renderer_destroy(r);
	}

	if (err && errcap)
		snprintf(err, errcap, "no renderer could be initialized (%s)", why);
	return NULL;
}

void renderer_destroy(RRenderer *r)
{
	if (!r)
		return;
	if (r->fini)
		r->fini(r);    /* frees ctx */
	free(r);
}
