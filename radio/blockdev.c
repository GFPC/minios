#define _GNU_SOURCE
#include "blockdev.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char resolved[256];

static void md(const char *p)
{
    mkdir(p, 0755);
}

static void klog(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[160];
        int n = snprintf(b, sizeof(b), "<6>blockdev: %s\n", s);
        if (n > 0)
            (void)write(fd, b, n);
        close(fd);
    }
}

static void klogf(const char *fmt, const char *a, const char *b)
{
    char msg[120];
    snprintf(msg, sizeof(msg), fmt, a, b);
    klog(msg);
}

static int read_partname(const char *part_dir, char *name, size_t namesz)
{
    char path[320];
    int fd;

    snprintf(path, sizeof(path), "%s/uevent", part_dir);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    name[0] = '\0';
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl++ = '\0';
        if (!strncmp(line, "PARTNAME=", 9)) {
            snprintf(name, namesz, "%s", line + 9);
            return 0;
        }
        line = nl;
    }
    return -1;
}

static int link_part(const char *part_dir, const char *partname, const char *devnode)
{
    char by[128];
    char boot[160];
    struct stat st;

    if (stat(devnode, &st) != 0)
        return -1;

    snprintf(by, sizeof(by), "/dev/block/by-name/%s", partname);
    unlink(by);
    if (symlink(devnode, by) != 0)
        return -1;

    snprintf(boot, sizeof(boot), "/dev/block/bootdevice/by-name/%s", partname);
    unlink(boot);
    if (symlink(devnode, boot) != 0)
        return -1;

    klogf("by-name %s -> %s", partname, devnode);
    return 0;
}

int blockdev_ensure_by_name(void)
{
    DIR *d;
    struct dirent *e;
    char name[64];
    int linked = 0;

    md("/dev/block");
    md("/dev/block/by-name");
    md("/dev/block/bootdevice");
    md("/dev/block/bootdevice/by-name");

    d = opendir("/sys/block/mmcblk0");
    if (!d)
        return 0;

    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "mmcblk0p", 8) != 0)
            continue;
        char part_dir[128];
        char devnode[64];
        snprintf(part_dir, sizeof(part_dir), "/sys/block/mmcblk0/%s", e->d_name);
        if (read_partname(part_dir, name, sizeof(name)) != 0 || !name[0])
            continue;
        snprintf(devnode, sizeof(devnode), "/dev/%s", e->d_name);
        if (link_part(part_dir, name, devnode) == 0)
            linked++;
    }
    closedir(d);
    return linked;
}

const char *blockdev_by_name(const char *part)
{
    static const char *fallback[] = {
        "/dev/block/by-name/",
        "/dev/block/bootdevice/by-name/",
        NULL
    };

    if (!part || !part[0])
        return NULL;

    for (int i = 0; fallback[i]; i++) {
        snprintf(resolved, sizeof(resolved), "%s%s", fallback[i], part);
        if (access(resolved, F_OK) == 0)
            return resolved;
    }
    return NULL;
}
