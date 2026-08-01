#define _GNU_SOURCE
#include "radio_utils.h"
#include "radio.h"
#include "radio_utils.h"
#include "minios/log.h"
#include "minios/watchdog.h"
#include "minios/plog.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <sys/xattr.h>

void wf(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    }
}


int wf_checked(const char *path, const char *val)
{
    int fd;
    ssize_t n;

    if (!path || !val)
        return -1;
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    n = write(fd, val, strlen(val));
    close(fd);
    return (n >= 0 && (size_t)n == strlen(val)) ? 0 : -1;
}


void wf_boot(const char *path)
{
    if (access(path, W_OK) != 0)
        return;
    if (wf_checked(path, "1") == 0)
        return;
    (void)wf_checked(path, "1u");
}


void md(const char *p)
{
    mkdir(p, 0755);
}


int path_exists(const char *p)
{
    return access(p, F_OK) == 0;
}


int pid_alive(pid_t p)
{
    return p > 0 && kill(p, 0) == 0;
}


void write_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (data && data[0])
        (void)write(fd, data, strlen(data));
    close(fd);
}


void ensure_etc_group(void)
{
    if (path_exists("/etc/group"))
        return;
    write_file("/etc/group",
               "root:x:0:\n"
               "system:x:1000:inet,net_admin,wifi\n"
               "inet:x:3003:\n"
               "net_admin:x:3005:\n"
               "wifi:x:3009:\n");
}


void run_sh(const char *cmd)
{
    pid_t p = fork();
    if (p == 0) {
        int n = open("/dev/null", O_WRONLY);
        if (n >= 0) {
            dup2(n, 1);
            dup2(n, 2);
            close(n);
        }
        setenv("PATH", "/bin:/sbin", 1);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}


void radio_child_setup(void)
{
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
        dup2(fd, 0);
        close(fd);
    }
    fd = open("/tmp/radio.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
}


void wf_path_join(const char *dir, const char *leaf)
{
    char path[384];
    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    if (access(path, W_OK) == 0)
        wf(path, "1");
}


int copy_file_bin(const char *src, const char *dst)
{
    int fds, fdd;
    char buf[8192];
    ssize_t n;

    fds = open(src, O_RDONLY);
    if (fds < 0)
        return -errno;
    fdd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fdd < 0) {
        close(fds);
        return -errno;
    }
    while ((n = read(fds, buf, sizeof(buf))) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = write(fdd, buf + w, (size_t)(n - w));
            if (r <= 0) {
                close(fds);
                close(fdd);
                return -EIO;
            }
            w += r;
        }
    }
    /* Preserve file capabilities (security.capability xattr) — a plain
     * read()/write() copy silently drops them, since they're stored as an
     * extended attribute, not file content. Confirmed via a real-ROM strace
     * ground-truth capture that pd-mapper (and pm-service) rely on this:
     * `getcap /vendor/bin/pd-mapper` -> cap_net_bind_service=ep, and
     * pd-mapper's own startup does setgid/setuid(1000)+capset() requesting
     * CAP_NET_BIND_SERVICE — a non-root process can never gain a capability
     * back once it's missing from its permitted set, so if this xattr gets
     * stripped during staging, that capset() is doomed to fail regardless
     * of how/whether cnss_drop_to_system() manages the parent's own privilege
     * drop. Copy it byte-for-byte rather than trying to reconstruct it. */
    {
        char capbuf[256];
        ssize_t caplen = fgetxattr(fds, "security.capability", capbuf, sizeof(capbuf));
        if (caplen > 0)
            fsetxattr(fdd, "security.capability", capbuf, (size_t)caplen, 0);
    }

    close(fds);
    close(fdd);
    chmod(dst, 0755);
    return 0;
}


int proc_cmdline_has(const char *needle)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    char path[256], cmd[256];
    int fd;
    ssize_t n;

    if (!d || !needle)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0)
            continue;
        cmd[n] = '\0';
        if (strstr(cmd, needle)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}


int proc_running(const char *comm)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    char path[256], cmd[64];
    int fd;
    ssize_t n;

    if (!d || !comm)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0)
            continue;
        cmd[n] = '\0';
        char *nl = strchr(cmd, '\n');
        if (nl)
            *nl = '\0';
        if (!strcmp(cmd, comm)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}


