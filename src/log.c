/* ============================================================================
 * Reverie Engine — log.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int g_level = REV_LOG_INFO;

void rev_log_set_level(int lvl) { g_level = lvl; }
int  rev_log_level(void)        { return g_level; }

static const char *tag_of(int lvl)
{
	switch (lvl) {
	case REV_LOG_DEBUG: return "debug";
	case REV_LOG_INFO:  return "info ";
	case REV_LOG_WARN:  return "warn ";
	default:            return "error";
	}
}

void rev_log_msg(int lvl, const char *fmt, ...)
{
	if (lvl < g_level)
		return;

	char stamp[32];
	time_t t = time(NULL);
	struct tm tmv;
	localtime_r(&t, &tmv);
	strftime(stamp, sizeof stamp, "%H:%M:%S", &tmv);

	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[%s %s] reverie: ", stamp, tag_of(lvl));
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
