#define _GNU_SOURCE
#include <unistd.h>
#include "minios/boot_display.h"
#include "minios/log.h"

int boot_display_try_once(void)
{
    if (access("/dev/dri/card0", F_OK) != 0)
        return -1;
    return boot_display_run_kms();
}

int boot_display_run_kms(void)
{
    /* kms_paint owns DRM */

    const char *paths[] = { "/sbin/kms_paint", "/kms_paint", NULL };
    const char *bin = NULL;
    for (int i = 0; paths[i]; i++)
        if (access(paths[i], X_OK) == 0) { bin = paths[i]; break; }
    if (!bin) { klog("kms_paint: missing"); return -1; }

    klog("display: kms_paint start");
    pid_t p = fork();
    if (p == 0) {
        execl(bin, "kms_paint", "--hold", "--ui", NULL);
        _exit(127);
    }
    if (p < 0) return -1;
    klog("display: kms_paint hold started");
    return 0;
}

void boot_display_start_async(void)
{
    const char *paths[] = { "/sbin/kms_paint", "/kms_paint", NULL };
    const char *bin = NULL;
    for (int i = 0; paths[i]; i++)
        if (access(paths[i], X_OK) == 0) { bin = paths[i]; break; }
    if (!bin)
        return;
    pid_t p = fork();
    if (p == 0) {
        execl(bin, "kms_paint", "--quick", "--hold", "--ui", NULL);
        _exit(127);
    }
    if (p > 0)
        klog("display: kms_paint async (hold)");
}
