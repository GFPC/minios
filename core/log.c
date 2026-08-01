#define _GNU_SOURCE
#include "minios/log.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

void minios_log_write(log_level_t level, const char *tag, const char *fmt, ...)
{
    static int kmsg_fd = -1;
    char buf[512];
    char lvl_char;
    const char *lvl_str;
    
    switch (level) {
        case LOG_LEVEL_ERROR: lvl_char = '3'; lvl_str = "E"; break; // ERR
        case LOG_LEVEL_WARN:  lvl_char = '4'; lvl_str = "W"; break; // WARN
        case LOG_LEVEL_INFO:  lvl_char = '6'; lvl_str = "I"; break; // INFO
        case LOG_LEVEL_DEBUG: lvl_char = '7'; lvl_str = "D"; break; // DEBUG
        case LOG_LEVEL_TRACE: lvl_char = '7'; lvl_str = "T"; break; // DEBUG in kmsg
        default:              lvl_char = '6'; lvl_str = "I"; break;
    }

    /* Format message */
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Construct final kmsg line: <X>minios[TAG]: L/ message */
    int n = snprintf(buf, sizeof(buf), "<%c>minios[%s]: %s/ %s\n", 
                     lvl_char, tag ? tag : "sys", lvl_str, msg);

    if (kmsg_fd < 0) {
        kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    }
    
    if (kmsg_fd >= 0) {
        write(kmsg_fd, buf, (size_t)(n > 0 ? n : 0));
        /* We leave kmsg_fd open across calls since we don't track state well,
         * or we can just close it to avoid fd leak if this is called rarely,
         * but for performance let's just open/close for now unless we cache it properly. 
         * Actually, since it's just /dev/kmsg, caching requires a static int. */
    }
}

/* Backwards compatibility for old object files that haven't recompiled or still call these explicitly */
#undef klog
#undef klogf
void klog(const char *msg)
{
    LOGI("compat", "%s", msg);
}

void klogf(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOGI("compat", "%s", buf);
}
