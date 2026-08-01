#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/boot_display.h"
#include "minios/com.h"
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/plog.h"
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
    /* A fresh /dev/kmsg fd's read cursor starts at the OLDEST record still
     * in the kernel ring buffer, not the newest — reading sequentially
     * forward from there. The old 64KB cap here was smaller than this
     * kernel's actual printk buffer (CONFIG_LOG_BUF_SHIFT=17 -> 128KB), so
     * once boot had produced more than 64KB of log lines (trivially true
     * within seconds once the known USB-PHY-drop-during-wlan-power-on
     * reconnect spam starts), this loop always exhausted its buffer on old
     * messages and could NEVER reach anything logged after that point —
     * every "dmesg" this session silently showed only early boot lines
     * regardless of how much later, more relevant content existed.  Size
     * this comfortably larger than the kernel's own buffer so a full read
     * always reaches the true current end. */
    /* Must be >= kernel log_buf_len (8M, see scripts/build-minios-hybrid.sh
     * cmdline) or a full read can again fall short of the true current end,
     * same class of bug this function's own comment already describes once
     * (256KB was already too small the first time this was "fixed"). */
    static char buf[8 * 1024 * 1024 + 4096];
    size_t total = 0;
    while (total < sizeof(buf) - 1000) {
        ssize_t n = read(fd, buf + total, 1000);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    if (total == 0) {
        write(out, "kmsg empty\r\n", 12);
        return;
    }
    buf[total] = '\0';
    int lines = 0;
    for (char *p = buf + total - 1; p >= buf && lines < max_lines; p--) {
        if (*p == '\n') {
            lines++;
            if (lines == max_lines) {
                write(out, p + 1, total - (size_t)(p + 1 - buf));
                break;
            }
        }
    }
    if (lines < max_lines)
        write(out, buf, total);
    write(out, "\r\n", 2);
}

void com_dmesg_find(int out, const char *needle, int max_matches)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        write(out, "no kmsg\r\n", 9);
        return;
    }
    /* Must be >= kernel log_buf_len (8M, see scripts/build-minios-hybrid.sh
     * cmdline) or a full read can again fall short of the true current end,
     * same class of bug this function's own comment already describes once
     * (256KB was already too small the first time this was "fixed"). */
    static char buf[8 * 1024 * 1024 + 4096];
    size_t total = 0;
    while (total < sizeof(buf) - 1000) {
        ssize_t n = read(fd, buf + total, 1000);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    if (total == 0 || !needle || !needle[0]) {
        write(out, "kmsg empty or no pattern\r\n", 26);
        return;
    }
    buf[total] = '\0';
    int matches = 0;
    char *line = buf;
    while (line < buf + total && matches < max_matches) {
        char *nl = strchr(line, '\n');
        size_t linelen = nl ? (size_t)(nl - line) : strlen(line);
        if (linelen > 0) {
            char saved = line[linelen];
            line[linelen] = '\0';
            if (strstr(line, needle)) {
                write(out, line, linelen);
                write(out, "\r\n", 2);
                matches++;
            }
            line[linelen] = saved;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    if (matches == 0)
        write(out, "no match\r\n", 10);
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
        dup2(pfd[1], 2);
        close(pfd[1]);
        setenv("PATH", "/bin:/sbin:/system/bin", 1);
        execl("/bin/busybox", "busybox", "sh", "-c", cmd, NULL);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        perror("execl failed");
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
    if (!strncmp(line, "run ", 4)) {
        com_run_cmd_out(out, line + 4);
        return 1;
    }
    if (!strcmp(line, "ping"))
        return write(out, "pong\r\n", 6), 1;
    if (!strcmp(line, "help")) {
        const char *h = "commands: ping help status usb net drm dmesg dmesg-find kms touch touchmon adb adb-tcp adb-on usb-adb com-on ffslog fb radio wifi bt wifi-scan scan-result radio-log cnss-log crash-log qrtr-log pd-log icnss-state binder-state catlog qrtr-lookup radio-diag radio-pid radio-ls wifi-fw metrics save-log sync bt-attach modem-state boot-count pstore poweroff reboot recovery mount-debugfs\r\n";
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
    if (!strncmp(line, "dmesg-find ", 11)) {
        com_dmesg_find(out, line + 11, 200);
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
            if (usb_enable_ncm() != 0)
                klog("adb-tcp: NCM enable failed");
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
        usleep(500000);
        adb_start_tcp();
        write(out, "adb restarted tcp :5555\r\n", 26);
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
    if (!strcmp(line, "usb-rebind") || !strcmp(line, "usb rebind")) {
        usb_rebind_request();
        write(out, "usb rebind requested\r\n", 24);
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
    if (!strcmp(line, "wifi") || !strcmp(line, "radio")) {
        radio_request_wifi_async();
        char b[512];
        int n = radio_format_status(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        if (radio_job_running())
            write(out, "wifi bringup started (COM stays up)\r\n", 37);
        return 1;
    }
    if (!strcmp(line, "bt")) {
        radio_request_async();
        char b[512];
        int n = radio_format_status(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        if (radio_job_running())
            write(out, "wifi+bt bringup started\r\n", 25);
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
    if (!strcmp(line, "radio-log")) {
        com_read_file_out(out, "/tmp/radio.log", "no radio log yet\r\n");
        return 1;
    }
    if (!strcmp(line, "cnss-log")) {
        com_read_file_out(out, "/tmp/cnss.exec.log", "no cnss exec log yet\r\n");
        return 1;
    }
    if (!strcmp(line, "crash-log") || !strcmp(line, "plog")) {
        /* Widened from 16KB/12000 — the known USB-PHY-drop reconnect spam
         * (~86ms/line) can fill that whole window in about a second, which
         * silently hid every earlier, more relevant line (modem PIL
         * timing, wlfw QMI activity) behind the spam for the rest of the
         * boot. 64KB buys a much bigger look-back at the cost of a few
         * more seconds to transmit over the 115200-baud COM link. */
        static char b[65536];
        int n = plog_format_tail(b, sizeof(b), sizeof(b) - 64);
        if (n > 0)
            write(out, b, (size_t)n);
        else
            write(out, "no crash log\r\n", 14);
        return 1;
    }
    if (!strcmp(line, "panic-dmesg")) {
        com_read_file_out(out, "/sys/fs/pstore/dmesg-ramoops-0", "no pstore dmesg\r\n");
        return 1;
    }
    if (!strcmp(line, "panic-tail")) {
        /* Read last 8KB of dmesg-ramoops-0 — this is where the panic backtrace is */
        const char *path = "/sys/fs/pstore/dmesg-ramoops-0";
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            write(out, "no pstore dmesg\r\n", 17);
        } else {
            off_t sz = lseek(fd, 0, SEEK_END);
            off_t tail = (sz > 8192) ? sz - 8192 : 0;
            lseek(fd, tail, SEEK_SET);
            char buf[8192];
            ssize_t n = read(fd, buf, sizeof(buf));
            close(fd);
            if (n > 0)
                write(out, buf, (size_t)n);
            else
                write(out, "empty pstore\r\n", 14);
        }
        return 1;
    }
    if (!strcmp(line, "console-ramoops")) {
        com_read_file_out(out, "/sys/fs/pstore/console-ramoops-0", "no pstore console\r\n");
        return 1;
    }
    if (!strcmp(line, "qrtr-log")) {
        com_read_file_out(out, "/tmp/qrtr-ns.log", "no qrtr log yet\r\n");
        return 1;
    }
    if (!strcmp(line, "pd-log")) {
        com_read_file_out(out, "/tmp/pd-mapper.log", "no pd-mapper log yet\r\n");
        return 1;
    }
    if (!strcmp(line, "icnss-state")) {
        char b[4096];
        int n = radio_format_icnss(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    /* mount-debugfs: diagnose why /sys/kernel/debug/icnss/state is
     * unreachable — is "debugfs" even a registered VFS filesystem type on
     * this kernel (CONFIG_DEBUG_FS), or is the early mount() in main.c
     * failing for some other, live-fixable reason (timing, existing
     * mountpoint, permission)? Reports /proc/filesystems' debugfs line
     * plus a fresh mount() attempt with errno. */
    if (!strcmp(line, "mount-debugfs")) {
        char b[2048];
        int n = 0;
        int fd = open("/proc/filesystems", O_RDONLY);
        if (fd >= 0) {
            char chunk[1024];
            ssize_t r = read(fd, chunk, sizeof(chunk) - 1);
            close(fd);
            if (r > 0) {
                chunk[r] = '\0';
                int has = strstr(chunk, "debugfs") != NULL;
                n += snprintf(b + n, sizeof(b) - (size_t)n,
                              "/proc/filesystems has debugfs: %s\r\n",
                              has ? "YES" : "NO");
            }
        } else {
            n += snprintf(b + n, sizeof(b) - (size_t)n,
                          "/proc/filesystems open failed errno=%d\r\n", errno);
        }
        mkdir("/sys/kernel/debug", 0755);
        errno = 0;
        int rc = mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
        n += snprintf(b + n, sizeof(b) - (size_t)n,
                      "fresh mount() rc=%d errno=%d (%s)\r\n",
                      rc, errno, strerror(errno));
        n += snprintf(b + n, sizeof(b) - (size_t)n,
                      "/sys/kernel/debug exists: %s\r\n",
                      access("/sys/kernel/debug", F_OK) == 0 ? "yes" : "no");
        n += snprintf(b + n, sizeof(b) - (size_t)n,
                      "/sys/kernel/debug/icnss exists: %s\r\n",
                      access("/sys/kernel/debug/icnss", F_OK) == 0 ? "yes" : "no");
        write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "binder-state")) {
        char b[2048];
        int n = radio_format_binder(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    /* catlog <name>: dump the tail of /tmp/<name>.log — start_vendor_daemon()
     * redirects each daemon's stdout/stderr there, so this shows exactly why
     * e.g. hwservicemanager exited instead of just that it isn't running. */
    if (!strncmp(line, "catlog ", 7)) {
        char path[192];
        char b[2048];
        int n;
        snprintf(path, sizeof(path), "/tmp/%s.log", line + 7);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            n = snprintf(b, sizeof(b), "no %s (errno=%d)\r\n", path, errno);
        } else {
            off_t sz = lseek(fd, 0, SEEK_END);
            off_t start = sz > (off_t)sizeof(b) - 64 ? sz - ((off_t)sizeof(b) - 64) : 0;
            lseek(fd, start, SEEK_SET);
            ssize_t r = read(fd, b, sizeof(b) - 64);
            close(fd);
            n = r > 0 ? (int)r : snprintf(b, sizeof(b), "%s empty\r\n", path);
        }
        write(out, b, (size_t)n);
        return 1;
    }
    /* qrtr-lookup [service_hex]: runs /sbin/qrtr_lookup (raw AF_QIPCRTR
     * client — see tools/qrtr_lookup.c) synchronously and returns its
     * output. Defaults to 0x45 (wlfw) if no service given. This exists
     * because /proc/net/qrtr does NOT exist on this kernel (confirmed by
     * reading kernel/net/qrtr/qrtr.c directly — no proc_create/debugfs
     * anywhere in it), so wait_for_wlfw()/qrtr_has_wlfw() checking that
     * path in wlan.c have never been able to actually detect anything —
     * this is the real replacement check. */
    if (!strcmp(line, "qrtr-lookup") || !strncmp(line, "qrtr-lookup ", 12)) {
        const char *svc = line[11] == ' ' ? line + 12 : "45";
        char path[64];
        snprintf(path, sizeof(path), "/tmp/qrtr-lookup.txt");
        pid_t p = fork();
        if (p == 0) {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, 1);
                dup2(fd, 2);
                close(fd);
            }
            execl("/sbin/qrtr_lookup", "qrtr_lookup", svc, "8", NULL);
            dprintf(2, "execl qrtr_lookup failed errno=%d\n", errno);
            _exit(127);
        }
        if (p > 0) {
            int st = 0;
            waitpid(p, &st, 0);
        }
        char b[2048];
        int fd = open(path, O_RDONLY);
        int n;
        if (fd < 0) {
            n = snprintf(b, sizeof(b), "qrtr-lookup: no output file\r\n");
        } else {
            ssize_t r = read(fd, b, sizeof(b) - 1);
            close(fd);
            n = r > 0 ? (int)r : snprintf(b, sizeof(b), "qrtr-lookup: empty output\r\n");
        }
        write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "radio-diag")) {
        char b[4096];
        int n = radio_format_diag(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "radio-pid")) {
        char b[2048];
        int n = radio_format_pidinfo(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "wifi-fw")) {
        char b[2048];
        int n = radio_format_fw_list(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strcmp(line, "radio-ls")) {
        char b[4096];
        int n = radio_format_src_list(b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
        return 1;
    }
    if (!strncmp(line, "radio-find ", 11)) {
        char b[4096];
        int n = radio_format_find(line + 11, b, sizeof(b));
        if (n > 0)
            write(out, b, (size_t)n);
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
    /* save-log: copy /tmp radio/cnss/qrtr logs to SD persistent storage */
    if (!strcmp(line, "save-log") || !strcmp(line, "sd-log")) {
        plog_save_tmp_logs();
        plog_save_pstore();
        const char *path = plog_path();
        char b[256];
        int n;
        if (path && path[0])
            n = snprintf(b, sizeof(b), "saved logs to %s\r\n", path);
        else
            n = snprintf(b, sizeof(b), "no SD storage — logs only in /tmp\r\n");
        if (n > 0) write(out, b, (size_t)n);
        return 1;
    }
    /* sync: flush all filesystem write caches — run this before physically
     * pulling the SD card. Without it, the FAT32 metadata can be mid-write
     * on the card and every hard removal risks corruption (needed chkdsk
     * /f repeatedly in practice before this command existed). */
    if (!strcmp(line, "sync")) {
        plog_save_tmp_logs();
        sync();
        write(out, "synced — safe to remove SD now\r\n", 33);
        return 1;
    }
    if (!strcmp(line, "boot-count")) {
        write(out, "boot-count:\r\n", 13);
        com_run_cmd_out(out, "grep 'BOOT #' /mnt/sdcard/minios/boot.log 2>/dev/null "
                              "|| grep 'BOOT #' /mnt/persist/minios/boot.log 2>/dev/null "
                              "|| echo no_boot_log");
        return 1;
    }
    if (!strcmp(line, "pstore")) {
        plog_save_pstore();
        /* List pstore files */
        com_run_cmd_out(out, "ls -la /sys/fs/pstore/ 2>/dev/null");
        /* Read files directly in C — busybox 'test' not available */
        {
            const char *ps_files[] = {
                "/sys/fs/pstore/dmesg-ramoops-0",
                "/sys/fs/pstore/dmesg-ramoops-1",
                "/sys/fs/pstore/console-ramoops-0",
                NULL
            };
            for (int pi = 0; ps_files[pi]; pi++) {
                int pfd = open(ps_files[pi], O_RDONLY);
                if (pfd < 0)
                    continue;
                {
                    char hdr[128];
                    int hn = snprintf(hdr, sizeof(hdr), "\r\n=== %s (last 40KB) ===\r\n", ps_files[pi]);
                    if (hn > 0) write(out, hdr, (size_t)hn);
                }
                /* Seek to last 40KB — panic backtrace is at end */
                off_t sz = lseek(pfd, 0, SEEK_END);
                off_t tail_off = (sz > 40960) ? sz - 40960 : 0;
                lseek(pfd, tail_off, SEEK_SET);
                
                char buf[4096];
                ssize_t n;
                while ((n = read(pfd, buf, sizeof(buf))) > 0) {
                    write(out, buf, (size_t)n);
                    wdt_pet();
                }
                close(pfd);
            }
        }
        write(out, "\r\n", 2);
        return 1;
    }
    /* bt-attach: manually trigger hciattach on UART BT */
    if (!strcmp(line, "bt-attach")) {
        write(out, "hciattach /dev/ttyHS0 qca 3000000...\r\n", 38);
        pid_t p = fork();
        if (p == 0) {
            int null = open("/dev/null", O_RDONLY);
            if (null >= 0) { dup2(null, 0); close(null); }
            int lfd = open("/tmp/hciattach.log",
                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (lfd >= 0) { dup2(lfd, 1); dup2(lfd, 2); close(lfd); }
            execl("/sbin/hciattach", "hciattach",
                  "-n", "/dev/ttyHS0", "qca", "3000000", "flow", NULL);
            execl("/sbin/hciattach", "hciattach",
                  "-n", "/dev/ttyHS0", "any", "115200", NULL);
            _exit(127);
        }
        if (p > 0) {
            write(out, "hciattach started — check: crash-log\r\n", 39);
        } else {
            write(out, "fork failed\r\n", 14);
        }
        return 1;
    }
    /* modem-state: quick subsys0 + wlfw + ICNSS FW_READY status */
    if (!strcmp(line, "modem-state")) {
        char b[768];
        int n = 0;
        /* subsys state */
        char st[32] = "?";
        {
            int fd = open("/sys/bus/msm_subsys/devices/subsys0/state", O_RDONLY);
            if (fd >= 0) {
                ssize_t r = read(fd, st, sizeof(st) - 1);
                close(fd);
                if (r > 0) {
                    st[r] = '\0';
                    char *nl = strchr(st, '\n');
                    if (nl) *nl = '\0';
                }
            }
        }
        n += snprintf(b + n, sizeof(b) - (size_t)n, "subsys0=%s\r\n", st);
        /* QRTR wlfw */
        {
            int fd = open("/proc/net/qrtr", O_RDONLY);
            if (fd >= 0) {
                char qb[2048]; ssize_t r = read(fd, qb, sizeof(qb) - 1);
                close(fd);
                if (r > 0) {
                    qb[r] = '\0';
                    int has_wlfw = (strstr(qb, " 69 ") || strstr(qb, "0x45")) ? 1 : 0;
                    n += snprintf(b + n, sizeof(b) - (size_t)n,
                                  "qrtr_wlfw=%d\r\n", has_wlfw);
                } else {
                    n += snprintf(b + n, sizeof(b) - (size_t)n, "qrtr_wlfw=? (empty)\r\n");
                }
            } else {
                n += snprintf(b + n, sizeof(b) - (size_t)n, "qrtr_wlfw=? (no /proc/net/qrtr)\r\n");
            }
        }
        /* ICNSS FW_READY */
        {
            char icst[32] = "n/a";
            int fd = open("/sys/kernel/debug/icnss/state", O_RDONLY);
            if (fd >= 0) {
                ssize_t r = read(fd, icst, sizeof(icst) - 1);
                close(fd);
                if (r > 0) {
                    icst[r] = '\0';
                    char *nl = strchr(icst, '\n');
                    if (nl) *nl = '\0';
                }
            }
            unsigned long icval = strtoul(icst, NULL, 16);
            n += snprintf(b + n, sizeof(b) - (size_t)n,
                          "icnss_state=0x%s fw_ready=%d\r\n",
                          icst, (icval & 4) ? 1 : 0);
        }
        /* wlan0 / hci0 */
        n += snprintf(b + n, sizeof(b) - (size_t)n,
                      "wlan0=%s hci0=%s\r\n",
                      access("/sys/class/net/wlan0", F_OK) == 0 ? "yes" : "no",
                      access("/sys/class/bluetooth/hci0", F_OK) == 0 ? "yes" : "no");
        if (n > 0) write(out, b, (size_t)n);
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
            /* f_cdev_read() (kernel/drivers/usb/gadget/function/f_cdev.c)
             * unconditionally calls usb_cser_start_rx() at the end of
             * every single read(), even ones that return no data — on
             * this board that re-arm attempt fails every time
             * ("usb ep(ep1out) queue failed", real kernel-level cause not
             * yet fixed). Polling read() every 50ms turned that into a
             * continuous, unbounded retry (confirmed via a direct SD-card
             * boot.log read: tens of thousands of repeats in a single
             * boot, every ~50ms, never stopping) — real CPU/log overhead
             * on top of the cosmetic spam, and a plausible contributor to
             * this session's own COM-channel flakiness. Back off the poll
             * interval substantially; still fine for an interactive debug
             * console, and cuts the retry rate by ~5x. */
            usleep(250000);
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
