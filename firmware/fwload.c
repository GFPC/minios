#define _GNU_SOURCE
#include "minios/fwload.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *fw_search[] = {
    "/lib/firmware/",
    "/firmware/image/",
    "/vendor/firmware/",
    "/vendor/firmware_mnt/image/",
    "/lib/firmware/vendor_mnt/image/",
    NULL,
};

static void kmsg(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[160];
        int n = snprintf(b, sizeof(b), "<6>fwload: %s\n", s);
        if (n > 0)
            (void)write(fd, b, n);
        close(fd);
    }
}

void fwload_prepare_paths(void)
{
    mkdir("/lib", 0755);
    mkdir("/lib/firmware", 0755);
    mkdir("/vendor", 0755);
    mkdir("/vendor/firmware", 0755);
}

static int serve_one(const char *load_path)
{
    char dir[512];
    char name[128];
    char data_path[576];
    const char *slash;

    strncpy(dir, load_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '/');
    if (!slash || strcmp(slash + 1, "loading") != 0)
        return 0;
    *strrchr(dir, '/') = '\0';
    slash = strrchr(dir, '/');
    if (!slash)
        return 0;
    snprintf(name, sizeof(name), "%s", slash + 1);
    snprintf(data_path, sizeof(data_path), "%s/data", dir);

    int lfd = open(load_path, O_RDONLY);
    if (lfd < 0)
        return 0;
    char st[16] = {0};
    if (read(lfd, st, sizeof(st) - 1) <= 0) {
        close(lfd);
        return 0;
    }
    close(lfd);
    if (st[0] != '0' || (st[1] != '\0' && st[1] != '\n'))
        return 0;

    const char *src = NULL;
    char src_path[320];
    for (int i = 0; fw_search[i]; i++) {
        snprintf(src_path, sizeof(src_path), "%s%s", fw_search[i], name);
        if (access(src_path, R_OK) == 0) {
            src = src_path;
            break;
        }
    }
    if (!src) {
        char msg[160];
        snprintf(msg, sizeof(msg), "missing %s", name);
        kmsg(msg);
        return 0;
    }

    int ffd = open(src, O_RDONLY);
    if (ffd < 0)
        return 0;
    struct stat stb;
    if (fstat(ffd, &stb) < 0 || stb.st_size <= 0) {
        close(ffd);
        return 0;
    }

    lfd = open(load_path, O_WRONLY);
    if (lfd < 0) {
        close(ffd);
        return 0;
    }
    if (write(lfd, "1", 1) != 1) {
        close(lfd);
        close(ffd);
        return 0;
    }
    close(lfd);

    int dfd = open(data_path, O_WRONLY);
    if (dfd < 0) {
        lfd = open(load_path, O_WRONLY);
        if (lfd >= 0) {
            write(lfd, "-1", 2);
            close(lfd);
        }
        close(ffd);
        return 0;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(ffd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, (size_t)(n - off));
            if (w <= 0)
                break;
            off += w;
        }
        if (off < n)
            break;
    }
    close(ffd);
    close(dfd);

    lfd = open(load_path, O_WRONLY);
    if (lfd >= 0) {
        write(lfd, "0", 1);
        close(lfd);
    }

    kmsg(name);
    return 1;
}

static void scan_dir(const char *path, int depth)
{
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (!strcmp(e->d_name, "loading")) {
            serve_one(child);
            continue;
        }
        if (depth < 8) {
            struct stat sb;
            if (stat(child, &sb) == 0 && S_ISDIR(sb.st_mode))
                scan_dir(child, depth + 1);
        }
    }
    closedir(d);
}

static void helper_loop(void)
{
    for (;;) {
        scan_dir("/sys/devices", 0);
        usleep(100000);
    }
}

void fwload_helper_start(void)
{
    pid_t p = fork();
    if (p == 0) {
        helper_loop();
        _exit(0);
    }
    if (p > 0)
        kmsg("helper start");
}
