/* ============================================================================
 * Reverie Engine — platform.c
 * Backend selection: auto (Wayland first when WAYLAND_DISPLAY is set, else
 * X11 when DISPLAY is set, else a wayland-0 socket probe), or explicit.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "plat_x11.h"
#include "plat_wayland.h"

static bool wayland_socket_present(void)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	const char *name = getenv("WAYLAND_DISPLAY");
	if (!dir || !*dir)
		return false;
	if (!name || !*name)
		name = "wayland-0";
	if (strchr(name, '/'))
		return access(name, F_OK) == 0;
	char path[512];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	return access(path, F_OK) == 0;
}

RPlatform *platform_create(const char *pref, char *err, size_t errcap)
{
	bool want_x11 = false, want_wl = false;

	if (pref && strcmp(pref, "x11") == 0) {
		want_x11 = true;
	} else if (pref && strcmp(pref, "wayland") == 0) {
		want_wl = true;
	} else {
		/* auto */
		const char *wd = getenv("WAYLAND_DISPLAY");
		const char *xd = getenv("DISPLAY");
		if (wd && *wd)
			want_wl = true;
		else if (xd && *xd)
			want_x11 = true;
		else if (wayland_socket_present())
			want_wl = true;
		else
			want_x11 = true;
	}

	if (want_wl) {
		RPlatform *p = plat_wayland_create();
		if (p && p->init(p, err, errcap))
			return p;
		platform_destroy(p);
		if (pref && strcmp(pref, "wayland") == 0)
			return NULL;    /* explicit request: no silent fallback */
		LOGW("wayland unavailable (%s); falling back to X11",
		     err && *err ? err : "connect failed");
	}

	if (want_x11 || !(pref && strcmp(pref, "wayland") == 0)) {
		RPlatform *p = plat_x11_create();
		if (p && p->init(p, err, errcap))
			return p;
		platform_destroy(p);
		return NULL;
	}

	if (err && errcap)
		snprintf(err, errcap, "no usable display backend");
	return NULL;
}

void platform_destroy(RPlatform *p)
{
	if (!p)
		return;
	if (p->fini)
		p->fini(p);    /* also frees ctx */
	free(p);
}
