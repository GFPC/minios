#define _GNU_SOURCE
#include "blockdev.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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

static void klogf2(const char *a, const char *b)
{
    char msg[120];
    snprintf(msg, sizeof(msg), "%s %s", a, b);
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

static int read_dev_major_minor(const char *path, unsigned *maj, unsigned *min)
{
    int fd = open(path, O_RDONLY);
    char buf[32];
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return sscanf(buf, "%u:%u", maj, min) == 2 ? 0 : -1;
}

static int mknod_if_missing(const char *devpath, mode_t mode, unsigned maj, unsigned min)
{
    struct stat st;

    if (stat(devpath, &st) == 0)
        return 0;
    if (mknod(devpath, mode, makedev(maj, min)) != 0)
        return -1;
    klogf2("mknod", devpath);
    return 1;
}

static int ensure_one_block_dev(const char *blk_name)
{
    char dev_path[64];
    char sys_dev[128];
    unsigned maj, min;
    int created = 0;

    snprintf(sys_dev, sizeof(sys_dev), "/sys/block/%s/dev", blk_name);
    if (read_dev_major_minor(sys_dev, &maj, &min) != 0)
        return 0;

    snprintf(dev_path, sizeof(dev_path), "/dev/%s", blk_name);
    if (mknod_if_missing(dev_path, S_IFBLK | 0660, maj, min) > 0)
        created++;

    {
        char part_dir[128];
        DIR *pd;

        snprintf(part_dir, sizeof(part_dir), "/sys/block/%s", blk_name);
        pd = opendir(part_dir);
        if (!pd)
            return created;
        struct dirent *pe;
        while ((pe = readdir(pd))) {
            if (strncmp(pe->d_name, blk_name, strlen(blk_name)) != 0)
                continue;
            if (strcmp(pe->d_name, blk_name) == 0)
                continue;
            snprintf(sys_dev, sizeof(sys_dev), "/sys/block/%s/%s/dev",
                     blk_name, pe->d_name);
            if (read_dev_major_minor(sys_dev, &maj, &min) != 0)
                continue;
            snprintf(dev_path, sizeof(dev_path), "/dev/%s", pe->d_name);
            if (mknod_if_missing(dev_path, S_IFBLK | 0660, maj, min) > 0)
                created++;
        }
        closedir(pd);
    }
    return created;
}

int blockdev_ensure_devnodes(void)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int created = 0;

    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "mmcblk", 6) != 0)
            continue;
        created += ensure_one_block_dev(e->d_name);
    }
    closedir(d);
    return created;
}

int blockdev_wait_mmc(int sec)
{
    for (int i = 0; i < sec * 2; i++) {
        DIR *d = opendir("/sys/block");
        struct dirent *e;
        int found = 0;

        if (d) {
            while ((e = readdir(d))) {
                if (!strncmp(e->d_name, "mmcblk", 6)) {
                    found = 1;
                    break;
                }
            }
            closedir(d);
        }
        if (found)
            return 0;
        usleep(500000);
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

    klogf2("by-name", partname);
    return 0;
}

static int scan_mmc(const char *blk)
{
    char block_dir[128];
    DIR *d;
    struct dirent *e;
    char name[64];
    int linked = 0;

    snprintf(block_dir, sizeof(block_dir), "/sys/block/%s", blk);
    d = opendir(block_dir);
    if (!d)
        return 0;

    while ((e = readdir(d))) {
        if (strncmp(e->d_name, blk, strlen(blk)) != 0)
            continue;
        if (!strcmp(e->d_name, blk))
            continue;
        char part_dir[160];
        char devnode[64];
        snprintf(part_dir, sizeof(part_dir), "/sys/block/%s/%s", blk, e->d_name);
        if (read_partname(part_dir, name, sizeof(name)) != 0 || !name[0])
            continue;
        snprintf(devnode, sizeof(devnode), "/dev/%s", e->d_name);
        if (link_part(part_dir, name, devnode) == 0)
            linked++;
    }
    closedir(d);
    return linked;
}

int blockdev_ensure_by_name(void)
{
    DIR *d;
    struct dirent *e;
    int linked = 0;

    md("/dev/block");
    md("/dev/block/by-name");
    md("/dev/block/bootdevice");
    md("/dev/block/bootdevice/by-name");

    blockdev_ensure_devnodes();

    d = opendir("/sys/block");
    if (!d)
        return 0;

    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "mmcblk", 6) != 0)
            continue;
        linked += scan_mmc(e->d_name);
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

int blockdev_format_diag(char *buf, size_t bufsz)
{
    int n = 0;
    DIR *d;
    struct dirent *e;
    int have_mmc = 0;

    d = opendir("/sys/block");
    if (d) {
        while ((e = readdir(d))) {
            if (!strncmp(e->d_name, "mmcblk", 6)) {
                have_mmc = 1;
                break;
            }
        }
        closedir(d);
    }

    n += snprintf(buf + n, bufsz - (size_t)n, "mmc=%s linked=%d devnodes=%d\r\n",
                  have_mmc ? "yes" : "no",
                  blockdev_ensure_by_name(),
                  blockdev_ensure_devnodes());

    d = opendir("/dev/block/by-name");
    if (d) {
        n += snprintf(buf + n, bufsz - (size_t)n, "by-name:\r\n");
        while ((e = readdir(d)) && n < (int)bufsz - 64) {
            if (e->d_name[0] == '.')
                continue;
            char target[256];
            char linkpath[128];
            ssize_t tl;

            snprintf(linkpath, sizeof(linkpath), "/dev/block/by-name/%s", e->d_name);
            tl = readlink(linkpath, target, sizeof(target) - 1);
            if (tl > 0) {
                target[tl] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "  %s -> %s\r\n",
                              e->d_name, target);
            }
        }
        closedir(d);
    } else {
        n += snprintf(buf + n, bufsz - (size_t)n, "by-name: missing\r\n");
    }

    {
        int fd = open("/proc/partitions", O_RDONLY);
        char pbuf[512];
        if (fd >= 0) {
            ssize_t r = read(fd, pbuf, sizeof(pbuf) - 1);
            close(fd);
            if (r > 0) {
                pbuf[r] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "partitions:\r\n");
                for (char *line = pbuf; line && *line && n < (int)bufsz - 48; ) {
                    char *nl = strchr(line, '\n');
                    if (nl)
                        *nl++ = '\0';
                    if (strstr(line, "mmc") || strstr(line, "major"))
                        n += snprintf(buf + n, bufsz - (size_t)n, "  %s\r\n", line);
                    line = nl;
                }
            }
        }
    }
    return n;
}
