#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "minios/watchdog.h"

int wdt_fd = -1;
int wdt_msm_disabled = 0;
/* Separate from wdt_msm_disabled: tracks whether we've already attempted
 * (and, on this hardware, always failed) the SCM_SVC_SEC_WDOG_DIS call at
 * least once. wdt_pet() is called extremely frequently — every iteration
 * of essentially every wait/poll loop in the whole codebase, easily
 * hundreds of times per second during boot. Without this, a permanently
 * failing SCM call (confirmed: it never succeeds on this board/kernel/TZ
 * combination) turned every single wdt_pet() call into a fresh recursive
 * /sys/devices/platform tree scan plus a failing SCM call — real,
 * continuous CPU/log overhead across the whole system's runtime, not just
 * the cosmetic "scm_call failed .../Failed to deactivate secure wdog"
 * kmsg spam it produces (see MEMORY.md §4.5aj/al/am). Try once, remember
 * the outcome either way, never retry. */
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

static int wdt_mknod_from_sysfs(const char *sys_dev, const char *dev_path)
{
    char buf[32];
    unsigned maj, min;
    int fd;

    if (access(dev_path, F_OK) == 0)
        return 0;
    fd = open(sys_dev, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, buf, sizeof(buf) - 1) <= 0) {
        close(fd);
        return -1;
    }
    close(fd);
    buf[sizeof(buf) - 1] = '\0';
    if (sscanf(buf, "%u:%u", &maj, &min) != 2)
        return -1;
    if (mknod(dev_path, S_IFCHR | 0600, makedev(maj, min)) != 0)
        return -1;
    return 0;
}

void wdt_pet(void)
{
    if (!wdt_msm_disabled && !wdt_msm_disable_tried) {
        wdt_msm_disable_tried = 1;
        if (msm_wdt_disable_sysfs() == 0) {
            wdt_msm_disabled = 1;
            wdt_klog("msm watchdog disabled (late)");
        } else {
            wdt_klog("msm watchdog disable failed, not retrying");
        }
    }
    if (wdt_fd >= 0)
        ioctl(wdt_fd, WDIOC_KEEPALIVE, 0);
}

void wdt_open(void)
{
    static const char *paths[] = {
        "/dev/watchdog0", "/dev/watchdog", NULL
    };
    static const char *sys_devs[] = {
        "/sys/class/misc/watchdog/dev",
        "/sys/class/watchdog/watchdog0/dev",
        NULL
    };
    int t = 300;
    int i;

    wdt_msm_disable_tried = 1;
    if (msm_wdt_disable_sysfs() == 0) {
        wdt_msm_disabled = 1;
        wdt_klog("msm watchdog disabled via sysfs");
    }

    for (i = 0; sys_devs[i]; i++)
        wdt_mknod_from_sysfs(sys_devs[i], "/dev/watchdog0");

    for (i = 0; paths[i]; i++) {
        wdt_fd = open(paths[i], O_WRONLY | O_CLOEXEC);
        if (wdt_fd >= 0)
            break;
    }
    if (wdt_fd >= 0) {
        ioctl(wdt_fd, WDIOC_SETTIMEOUT, &t);
        wdt_pet();
        wdt_klog("linux watchdog open OK");
    } else {
        char msg[72];
        snprintf(msg, sizeof(msg), "linux wdt open fail msm_dis=%d",
                 wdt_msm_disabled);
        wdt_klog(msg);
    }
}
