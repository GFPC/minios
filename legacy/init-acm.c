/*
 * init-acm — Lineage repack: USB ACM shell only (no adbd). COM: ping, dmesg, help.
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
#include <unistd.h>
#include <stdarg.h>

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

static void kmsgf(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    kmsg(b);
}

static void trim_crlf(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
}

static void write_fd(int fd, const char *s)
{
    if (fd >= 0)
        (void)write(fd, s, strlen(s));
}

static int handle_cmd(int out, const char *line)
{
    if (!strcmp(line, "ping"))
        return write(out, "pong\r\n", 6), 1;
    if (!strcmp(line, "help")) {
        write_fd(out,
            "commands: ping pong help status dmesg sh kms fb\r\n");
        return 1;
    }
    if (!strcmp(line, "pong"))
        return write(out, "ping\r\n", 6), 1;
    if (!strcmp(line, "status")) {
        char b[320];
        int n = snprintf(b, sizeof(b),
            "minios acm ok\r\n"
            "usb=18d1:d001\r\n"
            "dri=%s\r\n"
            "fb=%s\r\n",
            access("/dev/dri/card0", F_OK) == 0 ? "yes" : "no",
            access("/dev/fb0", F_OK) == 0 ? "yes" : "no");
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "dmesg") || !strncmp(line, "dmesg ", 6)) {
        int dfd = open("/dev/kmsg", O_RDONLY);
        if (dfd < 0)
            return write(out, "no kmsg\r\n", 9), 1;
        char buf[512];
        ssize_t r;
        while ((r = read(dfd, buf, sizeof(buf))) > 0)
            write(out, buf, (size_t)r);
        close(dfd);
        write(out, "\r\n", 2);
        return 1;
    }
    if (!strcmp(line, "kms")) {
        if (access("/sbin/kms_paint", X_OK) == 0)
            return system("/sbin/kms_paint --quick"), 1;
        return write(out, "no kms_paint\r\n", 14), 1;
    }
    if (!strcmp(line, "fb")) {
        if (access("/dev/fb0", F_OK) != 0)
            return write(out, "no fb0\r\n", 8), 1;
        return system("dmesg | tail -5"), 1;
    }
    if (!strcmp(line, "sh")) {
        pid_t p = fork();
        if (p == 0) {
            execl("/bin/sh", "sh", NULL);
            _exit(127);
        }
        if (p > 0)
            waitpid(p, NULL, 0);
        return 1;
    }
    return 0;
}

static void com_shell(int fd)
{
    char line[256];
    int pos = 0;

    write_fd(fd,
        "\r\n=== MiniOS ACM (Lineage kernel) ===\r\n"
        "try: ping | help | dmesg | status | kms\r\n");

    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            usleep(100000);
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (pos == 0)
                continue;
            line[pos] = '\0';
            trim_crlf(line);
            kmsgf("cmd: %s", line);
            if (!handle_cmd(fd, line)) {
                char reply[320];
                snprintf(reply, sizeof(reply), "unknown: %s\r\n", line);
                write_fd(fd, reply);
            }
            write_fd(fd, "phone> ");
            pos = 0;
            continue;
        }
        if (c == 127 || c == 8) {
            if (pos > 0)
                pos--;
            continue;
        }
        if (pos < (int)sizeof(line) - 1)
            line[pos++] = c;
    }
}

static void start_kms_async(void)
{
    if (access("/dev/dri/card0", F_OK) != 0)
        return;
    if (access("/sbin/kms_paint", X_OK) != 0)
        return;
    if (fork() == 0) {
        execl("/sbin/kms_paint", "kms_paint", "--quick", NULL);
        _exit(1);
    }
}

int main(void)
{
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("configfs", "/config", "configfs", 0, NULL);
    kmsg("acm init start");

    if (system("/setup-usb.sh") != 0)
        kmsg("setup-usb failed");

    for (int i = 0; i < 60; i++) {
        if (access("/dev/ttyGS0", F_OK) == 0)
            break;
        usleep(200000);
    }

    start_kms_async();

    int fd = open("/dev/ttyGS0", O_RDWR);
    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        setsid();
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
        com_shell(0);
    }

    kmsg("no tty — sleeping");
    for (;;)
        sleep(3600);
    return 1;
}
