#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "minios/watchdog.h"

/* This board's msm_watchdog (f017000.qcom,wdt) can be *asked* to disable via
 * its sysfs "disable" attribute, but the request bottoms out in an
 * SCM_SVC_SEC_WDOG_DIS call that TrustZone permanently refuses on this
 * board/kernel/TZ combination -- confirmed live: the write reaches the
 * attribute (kernel logs "Failed to deactivate secure wdog" / "scm_call
 * failed func id 0x42000107" each time), it's not a missing-attribute or
 * dead-code situation. There's nothing more to do from HLOS about the SCM
 * failure itself; wdt_pet() just needs to not keep retrying it (it's called
 * on essentially every wait/poll loop in the codebase, hundreds of times a
 * second during boot -- retrying would mean a fresh recursive
 * /sys/devices/platform scan plus a doomed SCM call on every single one).
 *
 * The generic Linux watchdog char-device path (/dev/watchdog0, WDIOC_*) was
 * removed from this file: CONFIG_WATCHDOG is not set in this kernel build
 * (confirmed: kernel/.config), so that whole framework -- and every path
 * under /sys/class/{misc,watchdog}/watchdog* -- never exists to begin with;
 * it was silently-dead code, not a working fallback. */
int wdt_msm_disabled = 0;
static int wdt_msm_disable_tried = 0;

static void wdt_klog(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[192];
        int n = snprintf(b, sizeof(b), "<6>minios: %s\n", s);
        if (n > 0)
            write(fd, b, (size_t)n);
        close(fd);
    }
}

static int wdt_try_write(const char *path, const char *val)
{
    int fd;
    ssize_t n;

    fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    n = write(fd, val, strlen(val));
    close(fd);
    return n > 0 ? 0 : -1;
}

static int wdt_try_disable_path(const char *base)
{
    char path[320];

    snprintf(path, sizeof(path), "%s/disable", base);
    if (wdt_try_write(path, "1") == 0)
        return 0;
    return -1;
}

static int wdt_scan_dir_for_disable(const char *dirpath, int depth)
{
    DIR *d;
    struct dirent *e;
    char sub[512];

    if (depth > 4 || wdt_msm_disabled)
        return 0;

    d = opendir(dirpath);
    if (!d)
        return -1;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (strstr(e->d_name, "qcom,wdt") == NULL &&
            strstr(e->d_name, "wdog") == NULL &&
            strstr(e->d_name, "watchdog") == NULL) {
            if (depth < 4 && e->d_type == DT_DIR) {
                snprintf(sub, sizeof(sub), "%s/%s", dirpath, e->d_name);
                if (wdt_scan_dir_for_disable(sub, depth + 1) == 0) {
                    closedir(d);
                    return 0;
                }
            }
            continue;
        }
        snprintf(sub, sizeof(sub), "%s/%s", dirpath, e->d_name);
        if (wdt_try_disable_path(sub) == 0) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static int msm_wdt_disable_sysfs(void)
{
    static const char *bases[] = {
        "/sys/devices/platform/soc/f017000.qcom,wdt",
        "/sys/devices/platform/f017000.qcom,wdt",
        NULL
    };
    static const char *scan_roots[] = {
        "/sys/bus/platform/devices",
        "/sys/devices/platform/soc",
        "/sys/devices/platform",
        NULL
    };
    int i;

    if (wdt_msm_disabled)
        return 0;

    for (i = 0; bases[i]; i++) {
        if (wdt_try_disable_path(bases[i]) == 0)
            return 0;
    }

    for (i = 0; scan_roots[i]; i++) {
        if (wdt_scan_dir_for_disable(scan_roots[i], 0) == 0)
            return 0;
    }
    return -1;
}

void wdt_pet(void)
{
    if (wdt_msm_disabled || wdt_msm_disable_tried)
        return;
    wdt_msm_disable_tried = 1;
    if (msm_wdt_disable_sysfs() == 0) {
        wdt_msm_disabled = 1;
        wdt_klog("msm watchdog disabled (late)");
    } else {
        wdt_klog("msm watchdog disable failed (TZ refuses SCM_SVC_SEC_WDOG_DIS), not retrying");
    }
}

void wdt_open(void)
{
    wdt_msm_disable_tried = 1;
    if (msm_wdt_disable_sysfs() == 0) {
        wdt_msm_disabled = 1;
        wdt_klog("msm watchdog disabled via sysfs");
    } else {
        wdt_klog("msm watchdog disable failed (TZ refuses SCM_SVC_SEC_WDOG_DIS)");
    }
}
