#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/boot_display.h"
#include "minios/com.h"
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/radio.h"
#include "minios/usb.h"
#include "minios/watchdog.h"
#include "minios/touch.h"

void trim_crlf(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
}

void com_dmesg_tail(int out, int max_lines)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        write(out, "no kmsg\r\n", 9);
        return;
    }
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        write(out, "kmsg empty\r\n", 12);
        return;
    }
    buf[n] = '\0';
    int lines = 0;
    for (char *p = buf + n - 1; p >= buf && lines < max_lines; p--) {
        if (*p == '\n') {
            lines++;
            if (lines == max_lines) {
                write(out, p + 1, (size_t)(buf + n - (p + 1)));
                break;
            }
        }
    }
    if (lines < max_lines)
        write(out, buf, (size_t)n);
    write(out, "\r\n", 2);
}

void com_read_file_out(int out, const char *path, const char *missing)
{
    int fd = open(path, O_RDONLY);
    char buf[512];
    ssize_t n;

    if (fd < 0) {
        if (missing)
            write(out, missing, strlen(missing));
        return;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(out, buf, (size_t)n);
    close(fd);
    write(out, "\r\n", 2);
}

void com_run_cmd_out(int out, const char *cmd)
{
    int pfd[2];

    if (pipe(pfd) != 0)
        return;
    pid_t p = fork();
    if (p == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        setenv("PATH", "/bin:/sbin:/system/bin", 1);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    close(pfd[1]);
    char buf[256];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0)
        write(out, buf, (size_t)n);
    close(pfd[0]);
    if (p > 0)
        waitpid(p, NULL, 0);
}

int com_handle(int out, const char *line)
{
    if (!strcmp(line, "ping"))
        return write(out, "pong\r\n", 6), 1;
    if (!strcmp(line, "help")) {
        const char *h = "commands: ping help status usb net drm dmesg kms touch touchmon adb adb-tcp adb-on usb-adb com-on ffslog fb radio wifi bt wifi-scan scan-result metrics poweroff reboot recovery\r\n";
        write(out, h, strlen(h));
        return 1;
    }
    if (!strcmp(line, "status") || !strcmp(line, "drm")) {
        devnodes_ensure_drm();
        char b[512];
        int adb_run = adb_pid_alive() > 0;
        int n = snprintf(b, sizeof(b),
            "minios\r\nusb=18d1:d001\r\n"
            "ncm=%s if=%s\r\n"
            "adb=%s tcp=5555\r\n"
            "dri=%s sys=%s fb=%s bl=%s\r\n",
            usb_ncm_active ? "on" : "off", usb_net_if,
            adb_run ? "run" : "off",
            access("/dev/dri/card0", F_OK) == 0 ? "yes" : "no",
            access("/sys/class/drm/card0", F_OK) == 0 ? "yes" : "no",
            access("/dev/fb0", F_OK) == 0 ? "yes" : "no",
            access("/sys/class/backlight", F_OK) == 0 ? "yes" : "no");
        if (n > 0) write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "dmesg") || !strncmp(line, "dmesg ", 6)) {
        com_dmesg_tail(out, 40);
        return 1;
    }
    if (!strcmp(line, "kms")) {
        devnodes_ensure_drm();
        if (boot_display_run_kms() == 0)
            write(out, "kms ok\r\n", 8);
        else
            write(out, "kms fail\r\n", 10);
        return 1;
    }
    if (!strcmp(line, "touch")) {
        touch_ensure_nodes();
        int tfd = touch_open();
        char b[256];
        int n;
        if (tfd >= 0) {
            TouchCal cal;
            touch_get_cal(tfd, &cal);
            n = snprintf(b, sizeof(b),
                "touch ok: %s raw %d..%d x %d..%d\r\n",
                touch_name(), cal.min_x, cal.max_x, cal.min_y, cal.max_y);
            touch_close(tfd);
        } else {
            n = snprintf(b, sizeof(b), "touch fail\r\n");
        }
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "touchmon")) {
        touch_ensure_nodes();
        int tfd = touch_open();
        if (tfd < 0) {
            write(out, "touch fail\r\n", 12);
            return 1;
        }
        TouchCal cal = {0};
        touch_get_cal(tfd, &cal);
        cal.screen_w = 1080;
        cal.screen_h = 2340;
        write(out, "touchmon 5s...\r\n", 16);
        for (int i = 0; i < 50; i++) {
            int x, y, down;
            if (touch_read_timeout(tfd, &cal, &x, &y, &down, 100)) {
                char b[96];
                int n = snprintf(b, sizeof(b), "x=%d y=%d %s\r\n",
                                 x, y, down ? "down" : "up");
                if (n > 0)
                    write(out, b, (size_t)n);
            }
            wdt_pet();
        }
        touch_close(tfd);
        write(out, "done\r\n", 6);
        return 1;
    }
    if (!strcmp(line, "adb") || !strcmp(line, "adb status")) {
        char b[384];
        pid_t apid = adb_pid_alive();
        int adb_run = apid > 0;
        int n = snprintf(b, sizeof(b),
            "adb=%s pid=%d tcp=5555 if=%s\r\n"
            "ffslog=%s\r\n",
            adb_run ? "run" : "off",
            adb_run ? (int)apid : -1,
            usb_net_if,
            access("/tmp/ffs_adb.log", F_OK) == 0 ? "yes" : "no");
        if (n > 0) write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "ffslog") || !strcmp(line, "adb log")) {
        const char *paths[] = { "/tmp/ffs_adb.log", "/tmp/adbd.log", NULL };
        int got = 0;
        for (int i = 0; paths[i]; i++) {
            int fd = open(paths[i], O_RDONLY);
            if (fd < 0)
                continue;
            got = 1;
            char hdr[64];
            int hn = snprintf(hdr, sizeof(hdr), "== %s ==\r\n", paths[i]);
            if (hn > 0) write(out, hdr, (size_t)hn);
            char buf[512];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                write(out, buf, (size_t)n);
            close(fd);
        }
        if (!got)
            write(out, "no adb/ffs log\r\n", 17);
        write(out, "\r\n", 2);
        return 1;
    }
    if (!strcmp(line, "adb-on") || !strcmp(line, "adb on") ||
        !strcmp(line, "adb-tcp") || !strcmp(line, "adb tcp")) {
        write(out, "TCP adb :5555 (host: adb connect 192.168.42.129:5555)\r\n", 56);
        pid_t job = fork();
        if (job == 0) {
            usb_net_setup();
            adb_start_tcp();
            _exit(0);
        }
        return 1;
    }
    if (!strcmp(line, "usb-adb") || !strcmp(line, "usb adb")) {
        write(out, "USB ADB-only (replug USB, then: adb devices)\r\n", 47);
        int fd = open("/tmp/adb.on", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            close(fd);
        return 1;
    }
    if (!strcmp(line, "com-on") || !strcmp(line, "com on")) {
        usb_restore_com_only();
        write(out, "COM-only restored (re-attach usbipd)\r\n", 41);
        return 1;
    }
    if (!strcmp(line, "adb restart")) {
        pid_t apid = adb_pid_alive();
        if (apid > 0)
            kill(apid, SIGTERM);
        adbd_pid = -1;
        unlink("/tmp/adbd.pid");
        usleep(200000);
        write(out, "adb restart pending (reboot)\r\n", 30);
        return 1;
    }
    if (!strcmp(line, "keys")) {
        char b[512];
        int fd = open("/data/misc/adb/adb_keys", O_RDONLY);
        int n = snprintf(b, sizeof(b), "keys ");
        if (fd >= 0) {
            ssize_t r = read(fd, b + n, sizeof(b) - (size_t)n - 4);
            close(fd);
            if (r > 0)
                n += (int)r;
        } else {
            n += snprintf(b + n, sizeof(b) - (size_t)n, "missing");
        }
        b[n++] = '\r';
        b[n++] = '\n';
        write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "listen")) {
        char b[768];
        int n = 0;
        int fd = open("/proc/net/tcp", O_RDONLY);
        char buf[4096];

        if (fd < 0) {
            write(out, "no /proc/net/tcp\r\n", 19);
            return 1;
        }
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r <= 0) {
            write(out, "tcp empty\r\n", 11);
            return 1;
        }
        buf[r] = '\0';
        n += snprintf(b + n, sizeof(b) - (size_t)n, "tcp listeners :5555 (15B3):\r\n");
        for (char *p = buf; *p && n < (int)sizeof(b) - 80; ) {
            char *e = strchr(p, '\n');
            if (e)
                *e = '\0';
            if (strstr(p, ":15B3") || strstr(p, ":15b3"))
                n += snprintf(b + n, sizeof(b) - (size_t)n, "  %s\r\n", p);
            if (!e)
                break;
            p = e + 1;
        }
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "net")) {
        char b[512];
        int n = snprintf(b, sizeof(b),
            "ncm=%s if=%s ip=192.168.42.129\r\n",
            usb_ncm_active ? "on" : "off", usb_net_if);
        if (n > 0)
            write(out, b, (size_t)n);
        com_run_cmd_out(out, "ip -4 addr show 2>/dev/null");
        return 1;
    }
    if (!strcmp(line, "usb")) {
        char b[512];
        int n = 0;
        DIR *d = opendir(USB_G "/configs/c.1");

        n += snprintf(b + n, sizeof(b) - (size_t)n, "gadget %s UDC=", USB_G);
        {
            char udc[64];
            int fd = open(USB_G "/UDC", O_RDONLY | O_CLOEXEC);
            udc[0] = '\0';
            if (fd >= 0) {
                read(fd, udc, sizeof(udc) - 1);
                close(fd);
            }
            n += snprintf(b + n, sizeof(b) - (size_t)n, "%s\r\n", udc);
        }
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < (int)sizeof(b) - 64) {
                if (e->d_name[0] == '.')
                    continue;
                n += snprintf(b + n, sizeof(b) - (size_t)n, "  %s\r\n", e->d_name);
            }
            closedir(d);
        }
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "fb") || !strcmp(line, "fastboot")) {
        write(out, "reboot bootloader\r\n", 20);
        sync();
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "bootloader");
        reboot(RB_AUTOBOOT);
        return 1;
    }
    if (!strcmp(line, "recovery")) {
        write(out, "reboot recovery\r\n", 18);
        sync();
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "recovery");
        reboot(RB_AUTOBOOT);
        return 1;
    }
    if (!strcmp(line, "reboot")) {
        write(out, "reboot\r\n", 8);
        sync();
        reboot(RB_AUTOBOOT);
        return 1;
    }
    if (!strcmp(line, "poweroff") || !strcmp(line, "halt")) {
        write(out, "power off\r\n", 11);
        sync();
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_POWER_OFF, NULL);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (!strcmp(line, "radio") || !strcmp(line, "wifi") || !strcmp(line, "bt")) {
        radio_request_async();
        char b[512];
        int n = radio_format_status(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        if (radio_job_running())
            write(out, "bringup started (COM stays up)\r\n", 32);
        return 1;
    }
    if (!strcmp(line, "wifi-scan") || !strcmp(line, "scan")) {
        radio_scan_request_async();
        write(out, "scan started — check: scan-result\r\n", 37);
        return 1;
    }
    if (!strcmp(line, "scan-result") || !strcmp(line, "wifi-scan-result")) {
        if (radio_scan_running())
            write(out, "scan still running...\r\n", 23);
        com_read_file_out(out, "/tmp/wifi-scan.txt",
                          "no scan yet — run: wifi-scan\r\n");
        return 1;
    }
    if (!strcmp(line, "metrics") || !strcmp(line, "top")) {
        char b[512];
        int n = 0;
        int fd = open("/proc/meminfo", O_RDONLY);
        char mbuf[256];
        if (fd >= 0) {
            ssize_t r = read(fd, mbuf, sizeof(mbuf) - 1);
            close(fd);
            if (r > 0) {
                mbuf[r] = '\0';
                char *p;
                if ((p = strstr(mbuf, "MemTotal:")))
                    n += snprintf(b + n, sizeof(b) - (size_t)n, "%s\r\n", p);
                if ((p = strstr(mbuf, "MemAvailable:")))
                    n += snprintf(b + n, sizeof(b) - (size_t)n, "%s\r\n", p);
            }
        }
        fd = open("/proc/loadavg", O_RDONLY);
        if (fd >= 0) {
            char lb[64];
            ssize_t r = read(fd, lb, sizeof(lb) - 1);
            close(fd);
            if (r > 0) {
                lb[r] = '\0';
                n += snprintf(b + n, sizeof(b) - (size_t)n, "load %s", lb);
            }
        }
        if (n > 0)
            write(out, b, (size_t)n);
        else
            write(out, "no metrics\r\n", 12);
        return 1;
    }
    return 0;
}

void com_shell(int fd)
{
    char line[256];
    int pos = 0;

    write(fd, "\r\n=== MiniOS COM ===\r\nphone> ", 30);

    for (;;) {
        wdt_pet();
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            usleep(50000);
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (pos == 0)
                continue;
            line[pos] = '\0';
            trim_crlf(line);
            klogf("com: %s", line);
            if (!com_handle(fd, line)) {
                char r[280];
                snprintf(r, sizeof(r), "unknown: %s\r\n", line);
                write(fd, r, strlen(r));
            }
            write(fd, "phone> ", 7);
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
