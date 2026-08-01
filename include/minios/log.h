#ifndef MINIOS_LOG_H
#define MINIOS_LOG_H

#include <stdarg.h>

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE
} log_level_t;

/* Internal primitive */
void minios_log_write(log_level_t level, const char *tag, const char *fmt, ...);

/* New structured logging macros */
#define LOGE(tag, fmt, ...) minios_log_write(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) minios_log_write(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) minios_log_write(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) minios_log_write(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOGT(tag, fmt, ...) minios_log_write(LOG_LEVEL_TRACE, tag, fmt, ##__VA_ARGS__)

/* Legacy fallback macros for untouched files */
#define klog(msg) LOGI("compat", "%s", msg)
#define klogf(fmt, ...) LOGI("compat", fmt, ##__VA_ARGS__)
#define klogf2(a, b) LOGI("compat", "%s %s", a, b)

#endif /* MINIOS_LOG_H */
