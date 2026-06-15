#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include "minios/log.h"

void klog(const char *msg)
{
    int fd = -1;
    if (fd < 0) fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[280];
        int n = snprintf(buf, sizeof(buf), "<6>minios: %s\n", msg);
        write(fd, buf, (n > 0 ? n : 0));
    }
}
void klogf(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog(buf);
}
