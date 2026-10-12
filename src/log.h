/* ============================================================================
 * Reverie Engine — log.h
 * Tiny leveled logger to stderr with timestamps. Compile-time strippable via
 * REV_LOG_NONE.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_LOG_H
#define REV_LOG_H

enum {
	REV_LOG_DEBUG = 0,
	REV_LOG_INFO  = 1,
	REV_LOG_WARN  = 2,
	REV_LOG_ERROR = 3
};

void rev_log_set_level(int lvl);
int  rev_log_level(void);
void rev_log_msg(int lvl, const char *fmt, ...);

#ifdef REV_LOG_NONE
#  define LOGD(...) ((void)0)
#  define LOGI(...) ((void)0)
#  define LOGW(...) ((void)0)
#  define LOGE(...) ((void)0)
#else
#  define LOGD(...) rev_log_msg(REV_LOG_DEBUG, __VA_ARGS__)
#  define LOGI(...) rev_log_msg(REV_LOG_INFO,  __VA_ARGS__)
#  define LOGW(...) rev_log_msg(REV_LOG_WARN,  __VA_ARGS__)
#  define LOGE(...) rev_log_msg(REV_LOG_ERROR, __VA_ARGS__)
#endif

#endif /* REV_LOG_H */
