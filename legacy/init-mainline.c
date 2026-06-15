/*
 * init-mainline — USB ACM shell + logs on /cache/minios (readable from TWRP).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>

#define LOG_DIR  "/cache/minios"
#define LOG_MAIN LOG_DIR "/mainline.log"
#define LOG_DMES LOG_DIR "/dmesg.log"

static FILE *logf;

static void kmsg(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[256];
        int n = snprintf(b, sizeof(b), "<6>init: %s\n", s);
        if (n > 0)
            (void)write(fd, b, (size_t)n);
        close(fd);
    }
}

static void log_line(const char *s)
{
    kmsg(s);
    if (logf) {
        fprintf(logf, "%s\n", s);
        fflush(logf);
    }
}

static int wait_path(const char *path, int ms_total, int step_ms)
{
    for (int t = 0; t < ms_total; t += step_ms) {
        if (access(path, F_OK) == 0)
            return 0;
        usleep((useconds_t)step_ms * 1000);
    }
    return -1;
}

static int try_mount(const char *dev, const char *fstype)
{
    char msg[128];
    if (mount(dev, "/cache", fstype, MS_NOATIME, "errors=continue") == 0) {
        snprintf(msg, sizeof(msg), "mounted %s (%s)", dev, fstype);
        log_line(msg);
        return 0;
    }
    return -1;
}

static void try_mount_cache(void)
{
    const char *devs[] = {
        "/dev/block/by-name/cache",
        "/dev/disk/by-partlabel/cache",
        NULL
    };

    mkdir("/cache", 0755);
    for (int i = 0; devs[i]; i++) {
        if (wait_path(devs[i], 45000, 500) != 0)
            continue;
        if (try_mount(devs[i], "ext4") == 0)
            goto ready;
        if (try_mount(devs[i], "f2fs") == 0)
            goto ready;
    }
    log_line("cache mount failed (mmc driver or partition missing?)");
    return;

ready:
    mkdir(LOG_DIR, 0755);
    logf = fopen(LOG_MAIN, "a");
    if (logf) {
        time_t now = time(NULL);
        fprintf(logf, "\n=== mainline boot %s", ctime(&now));
        fflush(logf);
    }
}

static void snapshot_dmesg(void)
{
    if (access(LOG_DIR, W_OK) != 0)
        return;
    int rc = system("dmesg >> " LOG_DMES " 2>/dev/null");
    (void)rc;
}

static void start_dmesg_watcher(void)
{
    if (access(LOG_DIR, W_OK) != 0)
        return;
    pid_t p = fork();
    if (p != 0)
        return;
    execl("/bin/sh", "sh", "-c",
          "while true; do dmesg >> " LOG_DMES " 2>/dev/null; sleep 4; done",
          NULL);
    _exit(1);
}

static void try_fb_blue(void)
{
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        log_line("no /dev/fb0 yet (try: modprobe msm)");
        return;
    }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(fb, FBIOGET_FSCREENINFO, &f) < 0) {
        close(fb);
        return;
    }

    size_t size = f.smem_len ? f.smem_len :
        (size_t)v.yres_virtual * f.line_length;
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (buf == MAP_FAILED) {
        close(fb);
        return;
    }

    memset(buf, 0, size);
    int bpp = v.bits_per_pixel / 8;
    if (bpp < 3)
        bpp = 4;
    int bar = v.yres > 200 ? 200 : v.yres;
    for (int y = 0; y < bar; y++) {
        uint8_t *row = buf + (size_t)y * f.line_length;
        for (unsigned x = 0; x < v.xres; x++) {
            uint8_t *p = row + (size_t)x * bpp;
            p[0] = 0xff;
            p[1] = 0;
            p[2] = 0;
        }
    }
    munmap(buf, size);
    close(fb);
    log_line("fb0: blue bar painted");
}

int main(void)
{
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    kmsg("mainline init start");

    try_mount_cache();
    log_line("mainline init start");
    if (access("/proc/version", R_OK) == 0) {
        char ver[256];
        int fd = open("/proc/version", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, ver, sizeof(ver) - 1);
            close(fd);
            if (n > 0) {
                ver[n] = '\0';
                char *nl = strchr(ver, '\n');
                if (nl)
                    *nl = '\0';
                log_line(ver);
            }
        }
    }

    start_dmesg_watcher();

    for (int i = 0; i < 50 && access("/dev/fb0", F_OK) != 0; i++)
        usleep(100000);
    if (access("/dev/fb0", F_OK) == 0)
        try_fb_blue();

    snapshot_dmesg();

    if (system("/setup-usb.sh") != 0)
        log_line("setup-usb failed");

    snapshot_dmesg();

    for (int i = 0; i < 50; i++) {
        if (access("/dev/ttyGS0", F_OK) == 0)
            break;
        usleep(200000);
    }

    const char *banner =
        "\r\n=== Mainline Phone Demo ===\r\n"
        "USB ACM shell ready\r\n"
        "Logs: /cache/minios/ (pull from TWRP)\r\n"
        "Display: modprobe msm  (then fbtest)\r\n"
        "===========================\r\n";
    write(1, banner, strlen(banner));
    log_line("ACM banner sent");

    setsid();
    int fd = open("/dev/ttyGS0", O_RDWR);
    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }

    execl("/bin/sh", "sh", NULL);
    log_line("no /bin/sh");
    for (;;)
        sleep(3600);
    return 1;
}
