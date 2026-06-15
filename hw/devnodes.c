#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/watchdog.h"

void mknod_from_sysfs(const char *sys_name, const char *dev_path, mode_t mode)
{
    char sp[128], buf[32];
    if (access(dev_path, F_OK) == 0)
        return;
    snprintf(sp, sizeof(sp), "/sys/class/msm_usb_bridge/%s/dev", sys_name);
    int fd = open(sp, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    unsigned maj = 0, min = 0;
    if (sscanf(buf, "%u:%u", &maj, &min) != 2)
        return;
    mknod(dev_path, mode, makedev(maj, min));
    klogf("COM: mknod %s (%u:%u)", dev_path, maj, min);
}

void devnodes_ensure_cser(void)
{
    mknod_from_sysfs("at_usb3", "/dev/at_usb3", S_IFCHR | 0660);
    mknod_from_sysfs("at_usb0", "/dev/at_usb0", S_IFCHR | 0660);
    mknod_from_sysfs("at_usb1", "/dev/at_usb1", S_IFCHR | 0660);
    mknod_from_sysfs("at_usb2", "/dev/at_usb2", S_IFCHR | 0660);
}

void mknod_from_class(const char *sys_dev, const char *dev_path, mode_t mode)
{
    char buf[32];
    if (access(dev_path, F_OK) == 0)
        return;
    int fd = open(sys_dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    unsigned maj = 0, min = 0;
    if (sscanf(buf, "%u:%u", &maj, &min) != 2)
        return;
    mknod(dev_path, mode, makedev(maj, min));
    klogf("drm: mknod %s (%u:%u)", dev_path, maj, min);
}

void devnodes_ensure_drm(void)
{
    sysfs_mkdir("/dev/dri");
    for (int i = 0; i < 300; i++) {
        if (access("/sys/class/drm/card0/dev", F_OK) == 0)
            break;
        usleep(100000);
        wdt_pet();
    }
    mknod_from_class("/sys/class/drm/card0/dev", "/dev/dri/card0", S_IFCHR | 0666);
    if (access("/sys/class/drm/renderD128/dev", F_OK) == 0)
        mknod_from_class("/sys/class/drm/renderD128/dev",
                         "/dev/dri/renderD128", S_IFCHR | 0666);
}
