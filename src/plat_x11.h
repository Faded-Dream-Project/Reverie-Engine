/* ============================================================================
 * Reverie Engine — plat_x11.h
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_PLAT_X11_H
#define REV_PLAT_X11_H

#include "platform.h"

RPlatform *plat_x11_create(void);

/* Window visual id (for EGL config matching); 0 on failure. */
u32 plat_x11_window_visual(RPlatform *p, int win);

#endif /* REV_PLAT_X11_H */
