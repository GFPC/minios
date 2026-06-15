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
        "QAAAAG0WHAibnCTan58gT/CZVzsdkb3RuJNrgjorNlD2OVfRDnh6v24lBgp9ZHIfTawwrnYFXNLzHDS2rlvPw7Zl/mvFbti80+vnMH1aYji3yzQ8WsvqpvIQHlxi4RsODdS2nmnWr1IUBcGktUuxuHajmU2geaeKctU3ZLM/Vn4r5qMJKjjFKrVamU4w8XBjKexpxMnTA7R52J8Ey1mEEKwFXyMktgwwtwMWXOQOB/eYwHOsOzPJmDzosYuZ+atry32EoXlwrmGW4sAmrW3t7rwcy7tDYz6sJdBjrNI3jKHVjcTLriHgki7Rdz80POe1m9Zf4T0/fNRoAHnSmYdZN6StiY9p1ImDc40LnuDfvH2/jrFNOWlMFbQixJQoh5IMGD2q2FYt6wZngEcP7Of8qvc0al6FdK8unXDBvqyUNik0YuFV15XV2ZnP5LE6b8vi7cF2RGC5XaR5Ixza7Ax3rXC+ITeZ40WP5iiQui/D++r0xfR9p3vo/BQlrdUo8Rq5quJ6GfjxsQDsQUHAwXVe89F1wOaIkxIWYeTzwlny3w5l0706DqOzlLorQ+wJchQOWfNmu743DmEJpdX9gOqxc0oobzcA7I6mBmo+JbwaLD4MTwCcU5Xfci1sxKHVBZyDJWjEomBn0YBNFV/9QhTiB40Nq71Kl6iuMcGSCk+0aX68a1Cx7nO1WQEAAQA= greg@DESKTOP-TNSQ33O\n";
    const char *paths[] = { "/data/misc/adb/adb_keys", "/adb_keys", NULL };
    size_t len = strlen(key);

    sysfs_mkdir("/data/misc/adb");
    chmod("/data/misc/adb", 0700);
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            write(fd, key, len);
            close(fd);
        }
    }
    klog("adb: keys written");
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
    if (access("/system/bin/linker64", F_OK) != 0 &&
        access("/lib64/linker64", F_OK) == 0)
        symlink("/lib64/linker64", "/system/bin/linker64");
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
    if (access("/system/bin/linker64", F_OK) != 0 &&
        access("/lib64/linker64", F_OK) == 0)
        symlink("/lib64/linker64", "/system/bin/linker64");

    adb_env_prepare();

    pid_t p = fork();
    if (p == 0) {
        int lfd = open("/tmp/adbd.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (lfd >= 0) {
            dup2(lfd, 1);
            dup2(lfd, 2);
            close(lfd);
        }
        setenv("ANDROID_ROOT", "/system", 1);
        setenv("ANDROID_DATA", "/data", 1);
        setenv("ANDROID_RUNTIME_ROOT", "/system", 1);
        setenv("TMPDIR", "/tmp", 1);
        setenv("LD_LIBRARY_PATH", "/system/lib64:/lib64", 1);
        if (access("/lib64/propstub.so", F_OK) == 0)
            setenv("LD_PRELOAD", "/lib64/propstub.so", 1);
        execl("/system/bin/adbd", "adbd", "--root_seclabel=u:r:su:s0",
              "--device_banner=recovery", NULL);
        execl("/sbin/adbd", "adbd", "--root_seclabel=u:r:su:s0",
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
