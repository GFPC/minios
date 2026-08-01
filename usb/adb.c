#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

pid_t adb_pid_alive(void)
{
    char buf[32];
    int fd = open("/tmp/adbd.pid", O_RDONLY | O_CLOEXEC);
    pid_t pid = -1;

    if (fd < 0)
        return adbd_pid > 0 && kill(adbd_pid, 0) == 0 ? adbd_pid : -1;
    if (read(fd, buf, sizeof(buf) - 1) > 0) {
        pid = (pid_t)atoi(buf);
        if (pid > 0 && kill(pid, 0) == 0) {
            close(fd);
            return pid;
        }
    }
    close(fd);
    return -1;
}

void adb_pid_save(pid_t pid)
{
    char buf[32];
    int fd = open("/tmp/adbd.pid", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    adbd_pid = pid;
    if (fd < 0)
        return;
    snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    write(fd, buf, strlen(buf));
    close(fd);
}
void adb_start_tcp(void)
{
    if (access("/sbin/adbd", X_OK) != 0)
        return;
    if (adb_pid_alive() > 0) {
        kill(adb_pid_alive(), SIGTERM);
        usleep(500000);
        adbd_pid = -1;
        unlink("/tmp/adbd.pid");
    }
    adb_env_prepare();
    if (adb_start_daemon() > 0)
        klog("adb: TCP adbd on :5555");
}
int usb_mount_ffs_adb(void)
{
    sysfs_mkdir("/dev/usb-ffs");
    sysfs_mkdir("/dev/usb-ffs/adb");
    umount2("/dev/usb-ffs/adb", MNT_DETACH);
    usleep(200000);
    if (mount("adb", "/dev/usb-ffs/adb", "functionfs", 0,
              "uid=2000,gid=1000,rmode=0770,fmode=0660") != 0) {
        klogf("USB: functionfs mount errno=%d", errno);
        return -1;
    }
    klog("USB: functionfs mounted");
    return 0;
}

int usb_link_ffs_adb(void)
{
    char ln[128];
    snprintf(ln, sizeof(ln), USB_G "/configs/c.1/f1");
    if (access(ln, F_OK) == 0)
        return 0;
    if (symlink(USB_G "/functions/ffs.adb", ln) != 0) {
        klogf("USB: ffs link errno=%d", errno);
        return -1;
    }
    klog("USB: ffs.adb linked as f1");
    return 0;
}

pid_t start_ffs_adb(void)
{
    if (access("/sbin/ffs_adb", X_OK) != 0)
        return -1;

    pid_t p = fork();
    if (p == 0) {
        execl("/sbin/ffs_adb", "ffs_adb", NULL);
        _exit(127);
    }
    if (p > 0)
        klogf("adb: ffs_adb pid=%d", p);
    return p;
}

void write_adb_keys(void)
{
    const char key[] =
        "QAAAAKfEjNPpXMO1XPxILUnUZnb0eE1oypAXchhM6bHFKwGrDuI6h5pB89H6sBb5ZM1wL6TRuAD0DDTjNy6bmfQMw3okiyeNdpOdic33SiAJflxR8dUmztmgQQFUBXd5RuQ3/pHLNXdZ6k9bT1KevMcwTNXH+SffdAeAKe9EgkZN5EL8ZO72mSZnfdgYroudNwZwEYmpSO41juwpKxgUwdDVjSrYNrwc95tZrK+7G1vn+aq9gY7npdGfMzbz53+TkFN9j2aW+eCdD+K9Q0F7Ph6Aj32V1vzScDy9F8GTC+NPhmRYETDI9j5dsEzEOXg0OGr2E1eg+VqvCYJN+BtMviNZVdwq6wCsPvmZO53KJp6OxhSbi4KmXzpGR9f45MwLOq7dEV9h1vnTkLYODiyNu9LSmKsn/KvfQ1unsCe4uyOHLdB+jsYaHy6Lv885R1UKczv2VIB8hc5lx7QJTUzgnaPo6LzhRvgQPZ6AoqK3nU1jUS1TpjqjUPEvEirTROghojFFru6q4GHoz9LZgNQR2+sszDbnQ0Ue4mVI4VH+C5tM+ysQaPAwyXVDv2BCdv6vgudT6Pmnw7LQG28WFAMNK34RLgAwOj8OW4dcup3pqoz3Ivm2Ln9bFRvp4dnHTKTQQNRk+7deCFybIrvLyHjhs8MrsdsuHoMq0yR7bx4Uda/jX1TNxgClDQEAAQA= greg@DESKTOP-TNSQ33O\n";
    const char *paths[] = { "/data/misc/adb/adb_keys", "/adb_keys", NULL };
    size_t len = strlen(key);

    sysfs_mkdir("/data/misc/adb");
    chmod("/data/misc/adb", 0755);
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, key, len);
            close(fd);
        }
    }
    klog("adb: keys written");
}

static int adb_setup_control_socket(void)
{
    const char *path = "/dev/socket/adbd";
    struct sockaddr_un addr;
    int fd;

    mkdir("/dev/socket", 0777);
    unlink(path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0666);
    listen(fd, 4);
    return fd;
}

void adb_tcp_selftest(void)
{
    struct sockaddr_in addr;
    char pkt[24 + 32];
    int s, n;
    uint32_t cmd = 0x434e584e;
    uint32_t magic = cmd ^ 0xffffffff;
    const char *payload = "device:recovery;";

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5555);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        klogf("adb: selftest connect errno=%d", errno);
        close(s);
        return;
    }
    memcpy(pkt, &cmd, 4);
    cmd = 0x01000000;
    memcpy(pkt + 4, &cmd, 4);
    cmd = 256 * 1024;
    memcpy(pkt + 8, &cmd, 4);
    cmd = (uint32_t)strlen(payload);
    memcpy(pkt + 12, &cmd, 4);
    cmd = 0;
    memcpy(pkt + 16, &cmd, 4);
    memcpy(pkt + 20, &magic, 4);
    memcpy(pkt + 24, payload, strlen(payload));
    write(s, pkt, 24 + (int)strlen(payload));
    n = read(s, pkt, sizeof(pkt));
    if (n > 0)
        klogf("adb: selftest ok n=%d", n);
    else
        klogf("adb: selftest no reply n=%d errno=%d", n, errno);
    close(s);
}

static void copy_linker64(void)
{
    struct stat st;
    const char *src = NULL;
    const char *candidates[] = {
        "/lib64/linker64",
        "/vendor/bin/linker64",
        "/system/bin/linker64",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        if (lstat(candidates[i], &st) == 0 && S_ISREG(st.st_mode)) {
            src = candidates[i];
            break;
        }
    }
    if (!src)
        return;

    if (lstat("/lib64/linker64", &st) != 0 || !S_ISREG(st.st_mode)) {
        if (lstat("/lib64/linker64", &st) == 0)
            unlink("/lib64/linker64");
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "cp -f '%s' /lib64/linker64 && chmod 755 /lib64/linker64", src);
        system(cmd);
    }

    // Ensure /apex/com.android.runtime/bin/linker64 exists
    sysfs_mkdir("/apex");
    sysfs_mkdir("/apex/com.android.runtime");
    sysfs_mkdir("/apex/com.android.runtime/bin");
    system("cp -f /lib64/linker64 /apex/com.android.runtime/bin/linker64 && chmod 755 /apex/com.android.runtime/bin/linker64");

    if (lstat("/system/bin/linker64", &st) != 0 || !S_ISREG(st.st_mode)) {
        if (lstat("/system/bin/linker64", &st) == 0)
            unlink("/system/bin/linker64");
        system("cp -f /lib64/linker64 /system/bin/linker64 && chmod 755 /system/bin/linker64");
    }
}


void adb_env_prepare(void)
{
    const char *ldtxt =
        "dir.recovery = /system/bin\n"
        "[recovery]\n"
        "namespace.default.isolated = false\n"
        "namespace.default.search.paths = /system/${LIB}\n"
        "namespace.default.asan.search.paths = /data/asan/system/${LIB}\n"
        "namespace.default.asan.search.paths += /system/${LIB}\n";

    sysfs_mkdir("/data");
    sysfs_mkdir("/data/misc");
    sysfs_mkdir("/data/misc/adb");
    sysfs_mkdir("/data/adbd");
    write_adb_keys();
    sysfs_mkdir("/system");
    sysfs_mkdir("/system/bin");
    sysfs_mkdir("/system/lib64");
    sysfs_mkdir("/system/etc");
    sysfs_mkdir("/linkerconfig");

    if (access("/system/lib64", F_OK) != 0)
        symlink("/lib64", "/system/lib64");
    copy_linker64();
    if (access("/system/bin/adbd", F_OK) != 0 &&
        access("/sbin/adbd", X_OK) == 0)
        symlink("/sbin/adbd", "/system/bin/adbd");
    if (access("/system/bin/sh", F_OK) != 0) {
        if (access("/bin/sh", X_OK) == 0)
            symlink("/bin/sh", "/system/bin/sh");
        else if (access("/bin/busybox", X_OK) == 0)
            symlink("/bin/busybox", "/system/bin/sh");
    }
    {
        const char *tools[] = {
            "busybox", "echo", "ls", "cat", "id", "pwd", "getprop", NULL
        };
        for (int i = 0; tools[i]; i++) {
            char dst[128], src[128];
            snprintf(dst, sizeof(dst), "/system/bin/%s", tools[i]);
            if (access(dst, F_OK) == 0)
                continue;
            snprintf(src, sizeof(src), "/bin/%s", tools[i]);
            if (access(src, X_OK) == 0)
                symlink(src, dst);
            else if (access("/bin/busybox", X_OK) == 0)
                symlink("/bin/busybox", dst);
        }
    }

    if (access("/linkerconfig/ld.config.txt", F_OK) != 0) {
        int fd = open("/linkerconfig/ld.config.txt",
                      O_WRONLY | O_CREAT | O_TRUNC, 0444);
        if (fd >= 0) {
            write(fd, ldtxt, strlen(ldtxt));
            close(fd);
        }
    }
    if (access("/system/etc/ld.config.txt", F_OK) != 0) {
        sysfs_mkdir("/system/etc");
        symlink("/linkerconfig/ld.config.txt", "/system/etc/ld.config.txt");
    }
}

pid_t adb_start_daemon(void)
{
    if (access("/sbin/adbd", X_OK) != 0) {
        klog("adb: adbd missing");
        return -1;
    }

    sysfs_mkdir("/system/bin");
    copy_linker64();

    adb_env_prepare();

    pid_t p = fork();
    if (p == 0) {
        int sock = adb_setup_control_socket();
        int lfd = open("/tmp/adbd.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (sock >= 0) {
            if (sock != 3) {
                dup2(sock, 3);
                close(sock);
            }
            setenv("ANDROID_SOCKET_adbd", "3", 1);
        }
        if (lfd >= 0) {
            dup2(lfd, 1);
            dup2(lfd, 2);
            close(lfd);
        }
        setenv("ANDROID_ROOT", "/system", 1);
        setenv("ANDROID_DATA", "/data", 1);
        setenv("ANDROID_RUNTIME_ROOT", "/system", 1);
        setenv("TMPDIR", "/tmp", 1);
        setenv("ANDROID_ADB_SERVER_PORT", "5555", 1);
        setenv("ADB_TRACE", "all", 1);
        setenv("LD_LIBRARY_PATH", "/system/lib64:/lib64", 1);
        if (access("/lib64/propstub.so", F_OK) == 0)
            setenv("LD_PRELOAD", "/lib64/propstub.so", 1);
        execl("/sbin/adbd", "adbd", "--root_seclabel=u:r:su:s0",
              "--device_banner=recovery", NULL);
        execl("/system/bin/adbd", "adbd", "--root_seclabel=u:r:su:s0",
              "--device_banner=recovery", NULL);
        klogf("adb: execl errno=%d", errno);
        _exit(127);
    }
    if (p < 0) {
        klogf("adb: fork errno=%d", errno);
        return -1;
    }
    adb_pid_save(p);
    klogf("adb: adbd pid=%d", p);
    usleep(800000);
    adb_tcp_selftest();
    return p;
}

void wait_ffs_ep1(int sec)
{
    for (int i = 0; i < sec * 10; i++) {
        wdt_pet();
        if (access("/dev/usb-ffs/adb/ep1", F_OK) == 0) {
            klog("adb: ffs ep1 ready");
            return;
        }
        usleep(100000);
    }
    klog("adb: ffs ep1 not seen (non-fatal)");
}

int wait_adbd_handoff(int sec)
{
    for (int i = 0; i < sec * 5; i++) {
        char buf[512];
        int fd;

        wdt_pet();
        fd = open("/tmp/adbd.log", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "adbd started") ||
                    strstr(buf, "UsbFfsConnection constructed") ||
                    strstr(buf, "opening control endpoint")) {
                    klog("adb: adbd handoff ok");
                    return 0;
                }
            }
        }
        fd = open("/tmp/ffs_adb.log", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "FAIL exec adbd") || strstr(buf, "FAIL open ep0"))
                    return -1;
            }
        }
        usleep(200000);
    }
    klog("adb: adbd handoff timeout");
    return -1;
}
