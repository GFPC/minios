#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "minios/watchdog.h"

int wdt_fd = -1;
void wdt_pet(void)
{
    if (wdt_fd >= 0) ioctl(wdt_fd, WDIOC_KEEPALIVE, 0);
}
void wdt_open(void)
{
    int t = 300;
    wdt_fd = open("/dev/watchdog0", O_WRONLY | O_CLOEXEC);
    if (wdt_fd < 0) wdt_fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    if (wdt_fd >= 0) ioctl(wdt_fd, WDIOC_SETTIMEOUT, &t);
}
