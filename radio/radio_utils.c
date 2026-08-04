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
#include <time.h>

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


/* Look up a character device's major number by name from /proc/devices —
 * needed because CLD3's wlan_hdd_state device is registered via
 * alloc_chrdev_region(&device, 0, ...) (dynamic major), NOT a fixed one.
 * Any hardcoded major (e.g. the legacy "500" from older prima/wcnss_wlan
 * drivers) can silently mismatch the real one (observed live: kernel logs
 * "wlan_hdd_state wlan major(501) initialized" while our mknod used 500),
 * which makes every open()/write() to that hand-made /dev/wlan node target
 * nothing real, and the whole ON/OFF handshake with the driver never
 * happens — no error, just silence, since mknod() itself still succeeds.
 * Returns the major, or -1 if not found. */
int get_chrdev_major(const char *name)
{
    FILE *f = fopen("/proc/devices", "r");
    char line[128];
    int major = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        int m;
        char nm[64];
        if (sscanf(line, "%d %63s", &m, nm) == 2 && !strcmp(nm, name)) {
            major = m;
            break;
        }
    }
    fclose(f);
    return major;
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
               "wifi:x:3009:\n"
               "vendor_qrtr:x:2906:\n"
               "vendor_rfs:x:2903:\n"
               "vendor_rfs_shared:x:2904:\n");
}

/* tftp_server (real vendor binary, staged as-is) drops from root to a
 * "vendor_rfs" identity (uid=gid=2903, confirmed real: stock_miui/
 * vendor_tree/etc/passwd) before it will touch /mnt/vendor/persist/rfs/* —
 * its own strings include "Failed to set UID to RFS (ret %d) errno=%d".
 * MiniOS never had an /etc/passwd file at all (only ensure_etc_group()
 * existed) — live capture this session (MEMORY.md §4.5ay/az, open_snoop.ko)
 * showed tftp_server starts and stays alive but never opens a single rfs/
 * path, unlike real Android's tftp_server which does so within ~150ms of
 * starting; on real Android the same binary drops to this exact identity
 * first. Whatever internal lookup it uses (getpwnam-by-name or a
 * setuid()-then-verify path) needs a real /etc/passwd entry to succeed. */
void ensure_etc_passwd(void)
{
    if (path_exists("/etc/passwd"))
        return;
    write_file("/etc/passwd",
               "root::0:0::/:/bin/sh\n"
               "system::1000:1000::/:/bin/sh\n"
               "vendor_qrtr::2906:2906::/:/bin/sh\n"
               "vendor_rfs::2903:2903::/:/bin/sh\n"
               "vendor_rfs_shared::2904:2904::/:/bin/sh\n");
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


/* A zombie (State: Z) still has a readable /proc/<pid>/comm — a dead child
 * nobody has wait()ed on looks identical to a live one by name alone. Real
 * case that motivated this: qrtr-ns forked very early (start_modem_qmi_
 * services(), main.c, ~t=10s, direct child of our PID-1 init) died and sat
 * as a zombie; every later proc_running("qrtr-ns") check (including
 * start_cnss_stack()'s own respawn-if-missing logic) kept reporting it as
 * running, so a fresh qrtr-ns was never started and QRTR_PORT_CTRL was
 * never bound for the rest of boot. */
static int proc_is_zombie(const char *pid_str)
{
    char path[256], buf[512];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/%s/status", pid_str);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    {
        char *state = strstr(buf, "State:");
        return state && strstr(state, "Z (zombie)") != NULL;
    }
}

/* Daemons started via exec_via_linker64() (qrtr-ns, pd-mapper, ...) are
 * exec'd as `execve("/lib64/linker64", {"linker64", "<real-path>", args...})`
 * — the kernel sets /proc/<pid>/comm from the *file actually executed*
 * (/lib64/linker64), not from argv[0]/argv[1], so comm is "linker64"
 * forever, never the daemon's own name. Every proc_running("qrtr-ns") /
 * proc_running("pd-mapper") check using comm alone therefore ALWAYS
 * returned false for these processes even while they were alive and
 * healthy — which meant start_cnss_stack() duplicate-spawned a brand new
 * qrtr-ns/pd-mapper on every single `radio` trigger (the old one still
 * holding QRTR_PORT_CTRL), and the duplicate's own bind()/registration
 * would fail/exit quickly, producing misleading "died early" log lines
 * while the original process kept running fine the whole time. Confirmed
 * live: a healthy, poll()-blocked qrtr-ns showed comm=linker64 with an
 * open control socket, yet proc_running("qrtr-ns") still said not-found.
 * Fix: fall back to /proc/<pid>/cmdline, whose argv[1] is the real staged
 * path (e.g. "/tmp/qrtr-ns") passed to exec_via_linker64() — match its
 * basename against comm too. */
static int proc_cmdline_matches(const char *pid_str, const char *comm)
{
    char path[256], buf[512];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid_str);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    for (ssize_t off = 0; off < n; ) {
        const char *tok = buf + off;
        size_t toklen = strlen(tok);
        const char *base = strrchr(tok, '/');
        base = base ? base + 1 : tok;
        if (!strcmp(base, comm))
            return 1;
        off += (ssize_t)toklen + 1;
    }
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
        if (proc_is_zombie(e->d_name))
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
        if (!strcmp(cmd, "linker64") && proc_cmdline_matches(e->d_name, comm)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}


/* Reap a spawned child non-blockingly and describe how it died. Returns 1
 * and fills msg if the child had already exited (WNOHANG caught it); 0 if
 * it's still alive or pid is invalid. Nothing else in this codebase ever
 * calls waitpid() on daemon children (they're fire-and-forget), so without
 * this every crashed daemon just sits as an unreaped zombie forever with no
 * record of why — this is the one place that can still answer "exit code or
 * which signal" after the fact. */
static int reap_child_status_fmt(int status, const char *label, char *msg, size_t msgsz)
{
    if (WIFEXITED(status))
        return snprintf(msg, msgsz, "%s: died early, exit code=%d", label, WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        return snprintf(msg, msgsz, "%s: died early, killed by signal=%d (%s)", label,
                        WTERMSIG(status), strsignal(WTERMSIG(status)));
    return snprintf(msg, msgsz, "%s: died early, wait status=0x%x", label, (unsigned)status);
}


int reap_child_status(pid_t pid, const char *label, char *msg, size_t msgsz)
{
    int status;
    pid_t r;

    if (pid <= 0 || !msg || msgsz == 0)
        return 0;
    r = waitpid(pid, &status, WNOHANG);
    if (r != pid)
        return 0;
    reap_child_status_fmt(status, label, msg, msgsz);
    return 1;
}


/* qrtr-ns routinely outlives a single 400-500ms usleep — it drops
 * privileges, runs a bit, then dies seconds later. Poll waitpid so we
 * reap before PID 1's default SIGCHLD handler discards the status. */
int reap_child_status_poll(pid_t pid, const char *label, char *msg, size_t msgsz,
                             int timeout_ms)
{
    int status;
    pid_t r;
    int waited = 0;

    if (pid <= 0 || !msg || msgsz == 0 || timeout_ms <= 0)
        return 0;
    for (waited = 0; waited < timeout_ms; waited += 100) {
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reap_child_status_fmt(status, label, msg, msgsz);
            return 1;
        }
        if (r < 0 && errno == ECHILD)
            return 0;
        wdt_pet();
        usleep(100000);
    }
    return 0;
}


void radio_trace(const char *msg)
{
    struct timespec ts;
    char line[512];

    if (!msg)
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(line, sizeof(line), "[radio +%lld.%03lds] %s",
             (long long)ts.tv_sec, (long long)(ts.tv_nsec / 1000000L), msg);
    plog_append(line);
    LOGI("radio", "%s", msg);
    {
        int fd = open("/tmp/radio_trace.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dprintf(fd, "%s\n", line);
            fsync(fd);
            close(fd);
        }
    }
}


void radio_dump_qrtr(const char *tag)
{
    char line[128];
    int fd;

    snprintf(line, sizeof(line), "qrtr-dump: %s", tag ? tag : "?");
    radio_trace(line);
    fd = open("/proc/net/qrtr", O_RDONLY);
    if (fd < 0) {
        radio_trace("qrtr-dump: /proc/net/qrtr missing");
        return;
    }
    {
        char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            radio_trace("qrtr-dump: empty");
            return;
        }
        buf[n] = '\0';
        for (char *p = buf; p && *p; ) {
            char *nl = strchr(p, '\n');
            if (nl)
                *nl++ = '\0';
            if (p[0])
                plog_append(p);
            p = nl;
        }
    }
}


