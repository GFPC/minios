#include <sys/sysmacros.h>
#define _GNU_SOURCE
#include "cnss.h"
#include "firmware.h"
#include "modem.h"
#include "radio.h"
#include "radio_state.h"
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
#include <stdint.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <linux/capability.h>

extern char **environ;

extern int system_mounted, vendor_mounted;
extern pid_t cnss_qrtr_pid, cnss_pdmap_pid, cnss_daemon_pid;
#define CNSS_EXEC_LOG "/tmp/cnss.exec.log"
#define CNSS_BUILD_TAG "cnss-start v43-boot_modem-late"
void ensure_cnss_devnodes(void)
{
    char buf[32];
    unsigned maj, min;
    int fd;

    /* Real ueventd.rc: /dev/diag 0660 system vendor_qti_diag (uid=1000,
     * gid=2901 -- confirmed via stock_miui/vendor_tree/etc/group). This
     * used to return early here without ever chmod/chown-ing whenever
     * devtmpfs had already auto-created the node itself (same devtmpfs-
     * timing race as /dev/zero, /dev/uioN, /dev/kmsg elsewhere in this
     * project) --
     * silently leaving it root:root at whatever default mode the diag
     * driver's own devnode() callback picked, which every non-root diag
     * client (QMID/ims-qmi-daemon/RILD, confirmed via logd_stub finally
     * capturing their liblog output: "Diag_LSM_Init: Failed to open
     * handle to diag driver, error = 13") then failed to open. Now
     * unconditionally fixes perms/ownership regardless of who created
     * the node or whether this is a repeat call. */
    if (!path_exists("/dev/diag")) {
        fd = open("/sys/class/diag/diag/dev", O_RDONLY);
        if (fd < 0)
            fd = open("/sys/devices/virtual/diag/diag/dev", O_RDONLY);
        if (fd < 0)
            return;
        if (read(fd, buf, sizeof(buf) - 1) <= 0) {
            close(fd);
            return;
        }
        close(fd);
        if (sscanf(buf, "%u:%u", &maj, &min) != 2)
            return;
        mknod("/dev/diag", S_IFCHR | 0660, makedev(maj, min));
        LOGI("radio", "%s", "cnss: /dev/diag created");
    }
    chmod("/dev/diag", 0660);
    chown("/dev/diag", 1000, 2901);
}


/* cnss_qrtr_pid is a plain global — fine within one process, but
 * radio_work() (and everything it calls, including this file's own
 * start_cnss_stack()) runs inside a fork()ed child (start_job()). Writes to
 * the global there live in that child's own copy-on-write memory and are
 * invisible to the top-level init process that actually serves COM/qrtr-pid
 * — which keeps reporting whatever cnss_qrtr_pid held in ITS OWN address
 * space (always the earliest, pre-fork spawn, never the later one started
 * during an actual `radio` run). Persist the real PID to a file instead,
 * which any process can read regardless of which fork wrote it. */
void write_qrtr_pid_file(pid_t pid)
{
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    int fd = open("/tmp/qrtr_ns.pid", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, buf, (size_t)n);
        close(fd);
    }
}

void cnss_log_exit(pid_t pid)
{
    int st = 0;
    int fd;

    if (pid <= 0)
        return;
    if (waitpid(pid, &st, WNOHANG) != pid)
        return;
    fd = open("/tmp/cnss.exec.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        if (WIFEXITED(st))
            dprintf(fd, "exit status=%d\n", WEXITSTATUS(st));
        else if (WIFSIGNALED(st))
            dprintf(fd, "signal=%d\n", WTERMSIG(st));
        else
            dprintf(fd, "wait status=0x%x\n", st);
        close(fd);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "cnss: daemon exit %d",
                 WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        LOGI("radio", "%s", msg);
    }
}


void cnss_log_line(const char *msg)
{
    /* Shared across multiple daemons that each drop to a different uid
     * (qrtr-ns=2906, pd-mapper/cnss-daemon=1000) -- O_CREAT's mode is
     * masked by the creating process's umask, so requesting 0666 here
     * isn't enough by itself (a default 022 umask silently turns it back
     * into 0644, and the next daemon's non-root write fails again). Force
     * it with an explicit fchmod so umask can't undo it. */
    int fd = open(CNSS_EXEC_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        fchmod(fd, 0666);
        dprintf(fd, "%s\n", msg);
        close(fd);
    }
}


/* setcap()-ing a capability into effective/permitted only sticks past a
 * following setuid() to a non-root uid if PR_SET_KEEPCAPS was set first,
 * and even then the kernel only *keeps* it in the permitted set across
 * setuid() -- it still has to be re-raised into effective with a second
 * capset() call afterward. The capset()-before-setuid() sequence this
 * replaced looked like it granted CAP_NET_ADMIN across the privilege drop
 * but actually didn't: the capability was silently wiped the moment
 * setuid() ran, with nothing to reveal that either. */
static void cnss_raise_cap(unsigned cap, const char *label)
{
    struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[2];
    char msg[96];

    memset(data, 0, sizeof(data));
    data[0].effective = data[0].permitted = data[0].inheritable = (1U << cap);
    if (syscall(SYS_capset, &hdr, data) != 0) {
        snprintf(msg, sizeof(msg), "%s: capset failed errno=%d (%s)", label, errno, strerror(errno));
        cnss_log_line(msg);
        return;
    }

    /* capset() above only reaches effective/permitted/inheritable of THIS
     * (pre-exec) process image. This function's whole purpose is to survive
     * the execve()/exec_via_linker64() that follows shortly after (into the
     * real, unprivileged, no-file-capabilities cnss-daemon binary) -- and
     * per the kernel's own exec capability rules, inheritable alone does
     * NOT propagate into the new image's effective/permitted set unless the
     * target binary has matching file capabilities (it doesn't) or the
     * AMBIENT set carries it across. Confirmed via a real capset failure
     * this session: cnss-daemon's own "Failed to init genl between daemon
     * and platform" (net/wireless/cnss_genl's GENL_ADMIN_PERM ops need
     * CAP_NET_ADMIN in the *running* daemon's effective set, not just this
     * pre-exec process) -- was silently losing the capability at exec time
     * despite this function successfully setting it beforehand. Without
     * this, keep_cap=CAP_NET_ADMIN was a no-op past the exec boundary. */
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) != 0) {
        snprintf(msg, sizeof(msg), "%s: PR_CAP_AMBIENT_RAISE failed errno=%d (%s)", label, errno, strerror(errno));
        cnss_log_line(msg);
    }
}

/* Drop from root to (uid, gid, supplementary groups), matching this
 * daemon's real identity on a stock ROM (see KERNEL_CHANGES.md / the
 * structure-audit that found these running as root the whole time despite
 * init.qcom.rc/init.target.rc specifying otherwise). Every failure logs its
 * own errno instead of the old bare "stay root" -- if this doesn't work,
 * the log says exactly why. */
void cnss_drop_privileges(uid_t uid, gid_t gid, const gid_t *groups, int ngroups,
                           unsigned keep_cap, const char *label)
{
    char msg[128];

    snprintf(msg, sizeof(msg), "%s: drop_privileges entered, target uid=%u gid=%u ngroups=%d",
             label, uid, gid, ngroups);
    cnss_log_line(msg);

    if (prctl(PR_SET_KEEPCAPS, 1) != 0)
        cnss_log_line("prctl(KEEPCAPS) failed (ok as root)");

    if (setgroups(ngroups, groups) != 0) {
        snprintf(msg, sizeof(msg), "%s: setgroups failed errno=%d (%s)", label, errno, strerror(errno));
        cnss_log_line(msg);
        return;
    }
    if (setgid(gid) != 0) {
        snprintf(msg, sizeof(msg), "%s: setgid(%u) failed errno=%d (%s)", label, gid, errno, strerror(errno));
        cnss_log_line(msg);
        return;
    }
    if (setuid(uid) != 0) {
        snprintf(msg, sizeof(msg), "%s: setuid(%u) failed errno=%d (%s)", label, uid, errno, strerror(errno));
        cnss_log_line(msg);
        return;
    }
    if (keep_cap)
        cnss_raise_cap(keep_cap, label);

    /* setuid()/setgid() away from root clears the process's dumpable flag
     * (kernel: commit_creds() -> set_dumpable(SUID_DUMP_DISABLE)) unless the
     * new creds already had elevated privilege carried over via a kept
     * capability. Non-dumpable restricts /proc/self/* for the process itself
     * -- and bionic's linker64 reads /proc/self/maps during its own
     * self-relocation on some builds. qrtr-ns and pd-mapper (keep_cap=0,
     * plain uid/gid drop) both crash inside linker64 at a fixed small offset
     * (NULL+0xa0, translation fault, confirmed via dmesg with
     * exception-trace enabled) right after this privilege drop, while
     * cnss-daemon (keep_cap=CAP_NET_ADMIN) does not -- restore dumpable
     * explicitly here so linker64 sees the same /proc/self access either
     * way. */
    if (prctl(PR_SET_DUMPABLE, 1) != 0)
        cnss_log_line("prctl(SET_DUMPABLE) failed (non-fatal)");

    snprintf(msg, sizeof(msg), "%s: dropped uid=%d gid=%d", label, getuid(), getgid());
    cnss_log_line(msg);
}

void cnss_drop_to_system(void)
{
    static const gid_t groups[] = { 1000, 3003, 3005, 3009 }; /* system, inet, net_admin, wifi */
    cnss_drop_privileges(1000, 1000, groups, 4, CAP_NET_ADMIN, "cnss-daemon");
}

/* Real ROM identities from init.qcom.rc/init.target.rc (uid confirmed via
 * stock_miui/vendor_tree/etc/passwd: vendor_qrtr=2906).
 *
 * keep_cap=0: binding QRTR_PORT_CTRL doesn't need CAP_NET_BIND_SERVICE at
 * all (kernel/net/qrtr/qrtr.c's qrtr_port_assign() allows it via
 * in_egroup_p(AID_VENDOR_QRTR), gid=2906, independent of any capability).
 * NOTE: an earlier version of this comment blamed the capability for a
 * secureexec/AT_SECURE bionic-linker crash — WRONG, disproven live: with
 * keep_cap=0 here the crash (SIGSEGV inside linker64 itself, NULL+0xa0,
 * translation fault) still happened, identically, on both the kernel's own
 * PT_INTERP-resolved linker64 and our own explicit /lib64/linker64
 * (exec_via_linker64()) — so it's not about which capability or which
 * linker binary. pd-mapper (cnss_drop_to_pd_mapper(), also keep_cap=0)
 * crashes the exact same way. The one remaining difference from
 * cnss_drop_to_system() (which does NOT crash) is groups: cnss-daemon gets
 * a real 4-entry supplementary group list, qrtr-ns/pd-mapper got
 * setgroups(0, NULL) — an empty list, unlike what Android's own init
 * actually does for a `user X group X` service (its initgroups()-equivalent
 * still includes the user's own gid as a supplementary entry, never truly
 * zero groups). Passing the daemon's own gid as its one supplementary group
 * here to match that and test the correlation directly. */
void cnss_drop_to_vendor_qrtr(void)
{
    static const gid_t groups[] = { 2906 };
    cnss_drop_privileges(2906, 2906, groups, 1, 0, "qrtr-ns");
}

void cnss_drop_to_pd_mapper(void)
{
    /* See cnss_drop_to_vendor_qrtr()'s comment — same empty-groups vs
     * linker64-SIGSEGV correlation, same test. */
    static const gid_t groups[] = { 1000 };
    cnss_drop_privileges(1000, 1000, groups, 1, 0, "pd-mapper");
}

/* Real logd is what pd-mapper/cnss-daemon's liblog actually talks to; see
 * minios/firmware/adb/logd_stub.c for why nothing else was ever there.
 * Must be up before those daemons exec (they connect on first log call and
 * never retry), so callers start this first. */
void start_logd_stub(void)
{
    static const char *path = "/sbin/logd_stub";
    char *argv[] = { (char *)"logd_stub", NULL };
    pid_t p;

    if (proc_running("logd_stub") || !path_exists(path))
        return;
    /* Unlike real vendor daemons (dynamically linked against Android's own
     * libc/liblog, needing exec_via_linker64()), logd_stub is one of our
     * own tools, built -static (see minios/Makefile) -- a plain ET_EXEC
     * ELF the kernel loads directly, no interpreter at all. Routing it
     * through start_vendor_daemon()'s linker64-first exec unconditionally
     * fails: the real Android linker64 refuses a non-PIE ELF outright
     * ("has unexpected e_type: 2") and exits immediately, so logd_stub
     * never actually started this whole project until now -- confirmed
     * via catlog logd_stub showing exactly that linker64 error. */
    p = fork();
    if (p == 0) {
        setsid();
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) { dup2(fd, 0); close(fd); }
        fd = open("/tmp/logd_stub.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execv(path, argv);
        _exit(127);
    }
    if (p > 0)
        LOGI("radio", "%s", "logd_stub started");
}


void cnss_log_argv(const char *run, char *const argv[])
{
    char msg[384];
    int n = snprintf(msg, sizeof(msg), "%s exec %s argv:", CNSS_BUILD_TAG, run);
    for (int i = 0; argv[i] && n < (int)sizeof(msg) - 32; i++)
        n += snprintf(msg + n, sizeof(msg) - (size_t)n, " [%d]=%s", i, argv[i]);
    cnss_log_line(msg);
}


int exec_via_linker64(const char *run, char *const argv[])
{
    char *linker_argv[16];
    int i = 0, j;

    if (!path_exists("/lib64/linker64"))
        return -1;
    linker_argv[i++] = (char *)"linker64";
    linker_argv[i++] = (char *)run;
    for (j = 1; argv[j] && i < 14; j++)
        linker_argv[i++] = argv[j];
    linker_argv[i] = NULL;
    execve("/lib64/linker64", linker_argv, environ);
    return -1;
}


int cnss_try_exec(const char *run, char *const argv[])
{
    cnss_log_argv(run, argv);
    execve(run, argv, environ);
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "execve %s errno=%d (%s)", run, errno, strerror(errno));
        cnss_log_line(msg);
    }
    if (path_exists("/lib64/linker64")) {
        char *linker_argv[16];
        int i = 0, j;

        linker_argv[i++] = (char *)"linker64";
        linker_argv[i++] = (char *)run;
        for (j = 1; argv[j] && i < 14; j++)
            linker_argv[i++] = argv[j];
        linker_argv[i] = NULL;
        cnss_log_argv("/lib64/linker64", linker_argv);
        execve("/lib64/linker64", linker_argv, environ);
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "linker execve errno=%d (%s)", errno, strerror(errno));
            cnss_log_line(msg);
        }
    }
    return -1;
}


void set_daemon_preload(void)
{
    char preload[256];
    int n = 0;

    preload[0] = '\0';
    if (access("/lib64/propstub.so", F_OK) == 0)
        n += snprintf(preload + n, sizeof(preload) - (size_t)n, "/lib64/propstub.so");
    if (access("/lib64/libcnss_shim.so", F_OK) == 0) {
        if (n > 0)
            n += snprintf(preload + n, sizeof(preload) - (size_t)n, ":");
        n += snprintf(preload + n, sizeof(preload) - (size_t)n, "/lib64/libcnss_shim.so");
    }
    if (preload[0])
        setenv("LD_PRELOAD", preload, 1);
}


void cnss_child_setup(const char *run)
{
    setsid();
    {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, 0);
            close(fd);
        }
        fd = open(CNSS_EXEC_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            fchmod(fd, 0666);
            dprintf(fd, "--- child pid=%d run=%s ---\n", getpid(), run);
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
    }
    setenv("PATH", "/vendor/bin:/system/bin:/bin:/sbin", 1);
    setenv("LD_LIBRARY_PATH", "/lib64:/vendor/lib64:/vendor/lib:/system/lib64", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_VENDOR", "/vendor", 1);
    setenv("ANDROID_RUNTIME_ROOT", "/system", 1);
    unsetenv("LD_CONFIG_FILE");
    unsetenv("LD_PRELOAD");
    set_daemon_preload();
    ensure_linker64_real();
    ensure_etc_group();
    ensure_etc_passwd();
    cnss_drop_to_system();
}


pid_t cnss_spawn_variant(const char *run, char *const argv[], const char *label)
{
    pid_t p;
    int st = 0;

    cnss_log_line(label);
    p = fork();
    if (p != 0) {
        if (p < 0)
            return 0;
        usleep(500000);
        if (waitpid(p, &st, WNOHANG) == p) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s exit=%d",
                     label, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            cnss_log_line(msg);
        }
        if (proc_running("cnss-daemon") || proc_cmdline_has("cnss-daemon"))
            return p;
        if (pid_alive(p))
            return p;
        return 0;
    }
    cnss_child_setup(run);
    cnss_try_exec(run, argv);
    _exit(127);
}


pid_t start_cnss_daemon(const char *path)
{
    const char *run;
    pid_t last = 0;

    if (!path || !path_exists(path))
        return 0;
    cnss_log_line("--- cnss-daemon start ---");
    run = stage_cnss_daemon(path);

    {
        /* -d twice (max verbosity, see cnss-daemon's own usage string:
         * "-dd for more") -- default wsvc_debug_level only shows priority
         * <=2 messages (confirmed via SD-card boot.log across dozens of
         * boots: "wlfw_start: Starting"/priority 2 always present, but
         * "WLFW service connected"/"FW status: 0x%lx"/priority 3 and
         * "FW memory is ready"/"Wait for FW memory ready indication"/
         * "Received FW memory ready indication"/priority 4 never appear
         * even once -- meaning this project has never actually been able
         * to observe from logs alone whether wlfw_napier_init() reaches,
         * blocks on, or gets signaled past its FW_MEM_READY wait). */
        char *a0[] = { (char *)"cnss-daemon", (char *)"-n", (char *)"-l", (char *)"-d", (char *)"-d", NULL };
        char *a1[] = { (char *)"cnss-daemon", (char *)"-n", (char *)"-d", (char *)"-d", NULL };
        struct { char *label; char **argv; } tries[] = {
            { "try0: -n -l -d -d (init.rc + max verbosity)", a0 },
            { "try1: -n -d -d", a1 },
        };

        for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
            pid_t p = cnss_spawn_variant(run, tries[i].argv, tries[i].label);
            if (p > 0 && (pid_alive(p) || proc_running("cnss-daemon"))) {
                LOGI("radio", "%s", "cnss: cnss-daemon alive");
                return p;
            }
            if (p > 0)
                last = p;
        }
    }
    return last;
}


void daemon_child_setup(const char *path, const char *logfile)
{
    int fd;

    setsid();
    fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) { dup2(fd, 0); close(fd); }

    fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dprintf(fd, "--- start %s ---\n", path);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }

    setenv("PATH", "/vendor/bin:/system/bin:/bin:/sbin", 1);
    setenv("LD_LIBRARY_PATH", "/lib64:/vendor/lib64:/vendor/lib:/system/lib64", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_VENDOR", "/vendor", 1);
    setenv("ANDROID_RUNTIME_ROOT", "/system", 1);
    unsetenv("ANDROID_ART_ROOT");
    unsetenv("LD_CONFIG_FILE");
    unsetenv("LD_PRELOAD");
    set_daemon_preload();
    ensure_linker64_real();
    ensure_etc_group();
    ensure_etc_passwd();
}


static pid_t start_vendor_daemon_impl(const char *path, char *const argv[], void (*drop)(void))
{
    pid_t p;
    char logfile[128];
    const char *name;

    if (!path || !path_exists(path))
        return 0;

    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    snprintf(logfile, sizeof(logfile), "/tmp/%s.log", name);

    p = fork();
    if (p != 0)
        return p > 0 ? p : 0;

    daemon_child_setup(path, logfile);
    if (drop)
        drop();
    /* exec_via_linker64() first, not as a fallback: plain execv() resolves
     * PT_INTERP straight out of the ELF header, which on this device is
     * "/system/bin/linker64" — a path on a read-only partition that's often
     * not a real file at all here (see ensure_linker64_real()'s own comment,
     * firmware.c). The failure mode isn't execv() returning an error (which
     * WOULD fall through to the code below) — the kernel still finds *some*
     * inode there and "successfully" execs it as the interpreter, which
     * then itself crashes immediately (observed: qrtr-ns and pd-mapper both
     * segfaulting inside linker64 at the same offset, right after setuid,
     * before either binary's own code ever runs). Since execv() never
     * returns on that kind of "successful" wrong-interpreter load, the old
     * execv()-then-exec_via_linker64() order could never actually reach the
     * fallback in exactly the case it exists for. Try our own known-good
     * /lib64/linker64 explicitly first; only fall back to plain execv() if
     * that's unavailable. */
    exec_via_linker64(path, argv);
    execv(path, argv);
    {
        char msg[192];
        snprintf(msg, sizeof(msg), "exec fail %s errno=%d (%s)", path, errno, strerror(errno));
        LOGI("radio", "%s", msg);
        int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dprintf(fd, "%s\n", msg); close(fd); }
    }
    _exit(127);
}

pid_t start_vendor_daemon(const char *path, char *const argv[])
{
    return start_vendor_daemon_impl(path, argv, NULL);
}

pid_t start_vendor_daemon_dropped(const char *path, char *const argv[], void (*drop)(void))
{
    return start_vendor_daemon_impl(path, argv, drop);
}

/* pd-mapper-specific: plain execve() first, exec_via_linker64() as fallback
 * -- the opposite order from start_vendor_daemon_impl() above, mirroring
 * cnss_try_exec()'s order (the path cnss-daemon uses, and which works
 * correctly). Root-caused live via Ghidra + a real-vs-MiniOS wire capture
 * of pd-mapper's own wlan/fw servreg query: the QMI request/response
 * protocol is byte-identical to a working real-Lineage capture, proving
 * pd-mapper's QMI server itself runs fine -- but its response always
 * reports zero domains, traced to its static `json_dir_list` global
 * (holding the config search paths) reading back as an all-NULL array at
 * runtime despite a valid R_AARCH64_RELATIVE relocation for it existing in
 * the ELF file itself (confirmed via readelf -r). That points at the
 * manual "linker64 argv[1]=target" invocation path specifically failing to
 * apply this relocation for pd-mapper, not a data or permissions problem --
 * qrtr-ns's earlier, unrelated crash bug (fixed separately, /dev/null
 * permissions) was already confirmed identical regardless of exec
 * mechanism, but this is a different bug class (silent data corruption,
 * not a crash) so that earlier finding doesn't rule this out. Try plain
 * execve() (kernel's own normal PT_INTERP-driven load, same as any regular
 * process start) first for pd-mapper specifically, since that's the
 * mechanism cnss-daemon already uses successfully. */
pid_t start_vendor_daemon_dropped_execfirst(const char *path, char *const argv[], void (*drop)(void))
{
    pid_t p;
    char logfile[128];
    const char *name;

    if (!path || !path_exists(path))
        return 0;

    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    snprintf(logfile, sizeof(logfile), "/tmp/%s.log", name);

    p = fork();
    if (p != 0)
        return p > 0 ? p : 0;

    daemon_child_setup(path, logfile);
    if (drop)
        drop();
    execv(path, argv);
    exec_via_linker64(path, argv);
    {
        char msg[192];
        snprintf(msg, sizeof(msg), "exec fail %s errno=%d (%s)", path, errno, strerror(errno));
        LOGI("radio", "%s", msg);
        int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dprintf(fd, "%s\n", msg); close(fd); }
    }
    _exit(127);
}


void run_vendor_oneshot(const char *path, char *const argv[])
{
    run_vendor_oneshot_timeout(path, argv, 0);
}

int run_vendor_oneshot_timeout(const char *path, char *const argv[], int timeout_sec)
{
    pid_t p = start_vendor_daemon(path, argv);
    char line[160];
    int st;

    if (p <= 0)
        return -1;
    if (timeout_sec <= 0) {
        waitpid(p, NULL, 0);
        return 0;
    }
    for (int i = 0; i < timeout_sec * 10; i++) {
        pid_t w = waitpid(p, &st, WNOHANG);
        if (w == p)
            return 0;
        if (w < 0 && errno == ECHILD)
            return 0;
        wdt_pet();
        usleep(100000);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    snprintf(line, sizeof(line),
             "cnss: oneshot timeout %ds pid=%d cmd=%s — killed",
             timeout_sec, (int)p, path);
    plog_append(line);
    LOGI("radio", "%s", line);
    return 1;
}


void stage_cnss_libs(void)
{
    const char *syslibs[] = {
        "libvndksupport.so", "libutils.so", "libcutils.so",
        "libhidlbase.so", "libhidltransport.so", "libhwbinder.so",
        "libbinder.so", "libbase.so", "libprocessgroup.so",
        "libvintf.so", "libjsoncpp.so", "libnl.so", "libc++.so",
        "liblog.so", "libhidlallocatorutils.so",
        "libbinder_ndk.so",
        NULL
    };
    const char *vendor_force[] = {
        "libqrtr.so", "libpdmapper.so", "libdiag.so", "libdsutils.so",
        "libidl.so", "libmdmdetect.so", "libnetmgr.so", "libcgrouprc.so",
        "libwlfw.so", "libqmi_cci.so", "libqmi_common_so.so", "libqmi_encdec.so",
        "libqmi_client_qmux.so", "libqmi_client_helper.so", "libjson.so",
        /* HIDL tokens needed by hwservicemanager (64-bit from vendor/lib64) */
        "android.hidl.token@1.0.so",
        "android.hidl.base@1.0.so",
        "android.hidl.manager@1.0.so",
        /* Full transitive closure of /vendor/bin/hw/qcrild (RILD)'s NEEDED
         * libs, resolved offline via a recursive readelf -d walk on the real
         * ROM (MEMORY.md §4.5h) rather than discovered one flash-cycle at a
         * time. qcrild itself is a ~12KB stub; the real logic lives in the
         * 33MB libril-qc-hal-qmi.so, which pulls in the rest. */
        "android.hardware.radio.config@1.0.so",
        "android.hardware.radio.config@1.1.so",
        "android.hardware.radio.config@1.2.so",
        "android.hardware.radio.deprecated@1.0.so",
        "android.hardware.radio@1.0.so",
        "android.hardware.radio@1.1.so",
        "android.hardware.radio@1.2.so",
        "android.hardware.radio@1.3.so",
        "android.hardware.radio@1.4.so",
        "android.hardware.radio@1.5.so",
        "android.hardware.secure_element@1.0.so",
        "android.hardware.secure_element@1.1.so",
        "android.hardware.secure_element@1.2.so",
        "android.system.suspend-V1-ndk.so",
        "libaconfig_storage_read_api_cc.so",
        "libconfigdb.so",
        "libdsi_netctrl.so",
        "libhardware_legacy.so",
        "liblqe.so",
        "libnetutils.so",
        "libpdnotifier.so",
        "libperipheral_client.so",
        "libprotobuf-cpp-full-3.9.1.so",
        "libqcrilFramework.so",
        "libqdi.so",
        "libqdp.so",
        "libqmiservices.so",
        "libril-qc-hal-qmi.so",
        "libril-qc-logger.so",
        "librilqmiservices.so",
        "libsqlite.so",
        "libsystem_health_mon.so",
        "libtime_genoff.so",
        "libxml.so",
        "libxml2.so",
        "libz.so",
        "qcrild_librilutils.so",
        "qtibus.so",
        "qtimutex.so",
        "server_configurable_flags.so",
        "vendor.qti.hardware.data.connection@1.0.so",
        "vendor.qti.hardware.data.connection@1.1.so",
        "vendor.qti.hardware.data.iwlan@1.0.so",
        "vendor.qti.hardware.radio.am@1.0.so",
        "vendor.qti.hardware.radio.ims@1.0.so",
        "vendor.qti.hardware.radio.ims@1.1.so",
        "vendor.qti.hardware.radio.ims@1.2.so",
        "vendor.qti.hardware.radio.ims@1.3.so",
        "vendor.qti.hardware.radio.ims@1.4.so",
        "vendor.qti.hardware.radio.ims@1.5.so",
        "vendor.qti.hardware.radio.ims@1.6.so",
        "vendor.qti.hardware.radio.internal.deviceinfo@1.0.so",
        "vendor.qti.hardware.radio.lpa@1.0.so",
        "vendor.qti.hardware.radio.qcriNvOpt@1.0.so",
        "vendor.qti.hardware.radio.qcrilhook@1.0.so",
        "vendor.qti.hardware.radio.qtiradio@1.0.so",
        "vendor.qti.hardware.radio.qtiradio@2.0.so",
        "vendor.qti.hardware.radio.qtiradio@2.1.so",
        "vendor.qti.hardware.radio.qtiradio@2.2.so",
        "vendor.qti.hardware.radio.qtiradio@2.3.so",
        "vendor.qti.hardware.radio.qtiradio@2.4.so",
        "vendor.qti.hardware.radio.uim@1.0.so",
        "vendor.qti.hardware.radio.uim@1.1.so",
        "vendor.qti.hardware.radio.uim@1.2.so",
        "vendor.qti.hardware.radio.uim_remote_client@1.0.so",
        "vendor.qti.hardware.radio.uim_remote_client@1.1.so",
        "vendor.qti.hardware.radio.uim_remote_client@1.2.so",
        "vendor.qti.hardware.radio.uim_remote_server@1.0.so",
        NULL
    };
    static int staged = 0;

    /* BUG FIXED (MEMORY.md §4.5b1): this function is called twice per boot
     * — once very early (main.c's unconditional modem_qmi_services_start(),
     * long before /system is ever mounted) and again later from
     * start_cnss_stack() (after radio_prepare()/mount_radio_partitions()
     * has mounted /system). The old `staged = 1` at the end of this
     * function was set unconditionally, including on the early call where
     * `system_mounted` is always false and the whole force-refresh loop
     * below is a no-op — poisoning this cache so the *real* refresh (once
     * /system actually becomes available) never runs for the rest of that
     * boot. Confirmed live: this exact regression reproduced the original
     * historical bug (§4.3 #5) byte-for-byte — vndservicemanager exiting
     * with the same stale-libc++.so linker error — even though the
     * force-refresh code itself was never removed. Only treat staging as
     * "done" once it actually had a mounted /system to copy from. */
    if (staged && path_exists("/lib64/libvndksupport.so")) {
        LOGI("radio", "%s", "cnss libs: cached");
        return;
    }

    md("/lib64");
    for (int i = 0; syslibs[i]; i++) {
        char src[256], dst[256];
        /* MEMORY.md §4.3 #5 / §7.6: this whole set is baked into the
         * initramfs itself (present at /lib64 before this function ever
         * runs) from an older base, and the old path_exists(dst)-skip
         * logic meant none of them could ever be replaced by the real
         * device's own matching versions. First confirmed live via
         * vndservicemanager.log/servicemanager.log for just libc++.so/
         * libbinder.so (missing std::stringstream / ServiceWithMetadata
         * vtables) — narrowly force-refreshing only those two just moved
         * the failure to the *next* stale lib in line each retest
         * (libutils.so's android::Looper::repoll, then libvintf.so's
         * ManifestInstance::accessor — a real whack-a-mole pattern, not a
         * one-off). Force-refresh the *entire* list unconditionally
         * instead of guessing one name at a time — safe, since it's this
         * exact device's own matching version, not a foreign one. */
        snprintf(dst, sizeof(dst), "/lib64/%s", syslibs[i]);
        if (system_mounted) {
            snprintf(src, sizeof(src), "/system/lib64/%s", syslibs[i]);
            if (path_exists(src))
                copy_file_bin(src, dst);
        }
    }
    for (int i = 0; vendor_force[i]; i++) {
        char src[256], dst[256];

        snprintf(dst, sizeof(dst), "/lib64/%s", vendor_force[i]);
        /* Actually force-refresh, matching syslibs[] above and its own
         * name — previously this skipped copying whenever *anything*
         * already existed at dst, which silently kept a stale bundled
         * version around for any name that happened to already be present
         * in the initramfs. Confirmed live (MEMORY.md §4.5h): a stale
         * bundled libhardware_legacy.so needed the old HIDL
         * android.system.suspend@1.0.so (doesn't exist on this device at
         * all) instead of the real ROM's android.system.suspend-V1-ndk.so
         * — CANNOT LINK EXECUTABLE for qcrild, same whack-a-mole class of
         * bug as the original libc++/libbinder one. */
        snprintf(src, sizeof(src), "/vendor/lib64/%s", vendor_force[i]);
        if (path_exists(src)) {
            copy_file_bin(src, dst);
            continue;
        }
        snprintf(src, sizeof(src), "/mnt/vendor/lib64/%s", vendor_force[i]);
        if (path_exists(src)) {
            copy_file_bin(src, dst);
            continue;
        }
        /* Some ROMs only ship the 64-bit HIDL token libs under
         * /system/lib64, not /vendor/lib64 — confirmed live via catlog:
         * without this, LD_LIBRARY_PATH fell through past the empty
         * /lib64 and /vendor/lib64 entries to /vendor/lib and picked up
         * a 32-bit android.hidl.token@1.0.so, so hwservicemanager's
         * linker refused to load it ("is 32-bit instead of 64-bit") and
         * it exited immediately every time despite the binary and the
         * /dev/hwbinder node both being fine. */
        if (system_mounted) {
            snprintf(src, sizeof(src), "/system/lib64/%s", vendor_force[i]);
            if (path_exists(src))
                copy_file_bin(src, dst);
        }
    }
    /* MiniOS pm-service stub: override the vendor libperipheral_client.so with our
     * minimal stub immediately after the main staging loop so that cnss-daemon
     * does not spin forever waiting for a pm-service QMI server that does not
     * exist on MiniOS.  The real vendor .so busy-loops inside pm_client_connect()
     * trying to connect to the Peripheral Manager QMI service; our stub grants
     * ACCESS_ALLOWED immediately via a direct callback, allowing cnss-daemon to
     * proceed to the actual WLFW bring-up sequence.
     * The stub .so lives at /lib64/pm_client_stub.so (baked into initramfs by
     * build-minios-hybrid.sh) and is installed here, after the vendor_force[]
     * loop would have just overwritten /lib64/libperipheral_client.so with the
     * broken vendor version. */
    {
        const char *pm_stub = "/lib64/pm_client_stub.so";
        const char *pm_dst  = "/lib64/libperipheral_client.so";
        if (path_exists(pm_stub)) {
            copy_file_bin(pm_stub, pm_dst);
            LOGI("radio", "%s", "pm_client: stub installed over vendor version");
        } else {
            LOGI("radio", "%s", "pm_client: stub not found at /lib64/pm_client_stub.so -- vendor version active");
        }
    }

    unlink("/lib64/libqrtr.so");
    if (path_exists("/vendor/lib64/libqrtr.so"))
        symlink_force("/vendor/lib64/libqrtr.so", "/lib64/libqrtr.so");
    else if (path_exists("/mnt/vendor/lib64/libqrtr.so"))
        symlink_force("/mnt/vendor/lib64/libqrtr.so", "/lib64/libqrtr.so");
    if (system_mounted) {
        staged = 1;
        plog_append("cnss libs: staged from mounted /system");
    } else {
        plog_append("cnss libs: /system not mounted yet, not caching (will retry)");
    }
    LOGI("radio", "%s", path_exists("/lib64/libvndksupport.so") ?
         "cnss libs: vndksupport ok" : "cnss libs: vndksupport missing");
}


/* binderfs auto-creates only the default "binder" node on mount; extra
 * named instances ("hwbinder", "vndbinder") must be explicitly requested
 * via BINDER_CTL_ADD on the control device. Without this, hwservicemanager
 * and vndservicemanager open() a /dev/hwbinder / /dev/vndbinder symlink
 * that points at nothing, exit immediately, and cnss-daemon's HIDL wlfw
 * transaction fails with EINVAL forever (confirmed live: servicemanager,
 * which only needs plain /dev/binder, started fine; hwservicemanager and
 * vndservicemanager did not, despite the binaries being present). */
#define BINDERFS_MAX_NAME 255
struct minios_binderfs_device {
    char name[BINDERFS_MAX_NAME + 1];
    uint32_t major;
    uint32_t minor;
};
#define MINIOS_BINDER_CTL_ADD _IOWR('b', 1, struct minios_binderfs_device)

static int add_binderfs_node(const char *ctl_path, const char *name)
{
    struct minios_binderfs_device dev;
    int fd, rc;

    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "%s", name);
    fd = open(ctl_path, O_RDONLY);
    if (fd < 0)
        return -errno;
    rc = ioctl(fd, MINIOS_BINDER_CTL_ADD, &dev);
    close(fd);
    return rc == 0 ? 0 : -errno;
}

void ensure_binder_nodes(void)
{
    if (path_exists("/dev/binder") && path_exists("/dev/hwbinder"))
        return;

    md("/dev/binderfs");
    if (!path_exists("/dev/binderfs/binder")) {
        if (mount("binder", "/dev/binderfs", "binder", 0, "max=1024") != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "binder mount errno=%d", errno);
            LOGI("radio", "%s", msg);
            return;
        }
        LOGI("radio", "%s", "binderfs mounted");
    }

    if (!path_exists("/dev/binderfs/hwbinder")) {
        int rc = add_binderfs_node("/dev/binderfs/binder-control", "hwbinder");
        LOGI("radio", "binderfs: add hwbinder rc=%d", rc);
    }
    if (!path_exists("/dev/binderfs/vndbinder")) {
        int rc = add_binderfs_node("/dev/binderfs/binder-control", "vndbinder");
        LOGI("radio", "binderfs: add vndbinder rc=%d", rc);
    }

    symlink_force("/dev/binderfs/binder", "/dev/binder");
    symlink_force("/dev/binderfs/hwbinder", "/dev/hwbinder");
    symlink_force("/dev/binderfs/vndbinder", "/dev/vndbinder");
    chmod("/dev/binderfs/binder", 0666);
    chmod("/dev/binderfs/hwbinder", 0666);
    chmod("/dev/binderfs/vndbinder", 0666);
}


static char binder_exit_report[512] = "binder-exit: (not run yet)\r\n";
static pid_t hwservicemanager_pid;

const char *get_binder_exit_report(void)
{
    return binder_exit_report;
}

/* MEMORY.md §4.3.5: hwservicemanager survives the initial 400ms check in
 * start_binder_services() below ("still alive after 400ms") but is later
 * found dead (binder-state's running=0) with no exit status ever captured
 * — nothing reaps it in between, so its real exit reason (signal vs. clean
 * exit vs. code) has never actually been observed. MiniOS has no global
 * SIGCHLD/zombie reaper (checked core/process.c), so an unreaped child
 * just sits as a zombie indefinitely — meaning a WNOHANG waitpid() on its
 * specific PID, called from anywhere later in boot, will still return the
 * real exit status if it already died by then. Call this right before
 * wlfw_start to finally get a real answer instead of "running=0" with no
 * further information. */
void check_hwservicemanager_late_exit(void)
{
    int st = 0;

    if (hwservicemanager_pid <= 0)
        return;
    if (waitpid(hwservicemanager_pid, &st, WNOHANG) != hwservicemanager_pid) {
        plog_append(proc_running("hwservicemanager") ?
                    "hwservicemanager: still alive at wlfw_start time" :
                    "hwservicemanager: not running, not yet reaped (still zombie or already reaped elsewhere)");
        return;
    }
    char line[96];
    if (WIFEXITED(st))
        snprintf(line, sizeof(line), "hwservicemanager: late exit status=%d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        snprintf(line, sizeof(line), "hwservicemanager: late kill signal=%d (%s)",
                 WTERMSIG(st), strsignal(WTERMSIG(st)));
    else
        snprintf(line, sizeof(line), "hwservicemanager: late wait status=0x%x", (unsigned)st);
    plog_append(line);
    hwservicemanager_pid = 0; /* reaped — don't re-check or misreport next call */
}

void start_binder_services(void)
{
    const char *svcs[] = {
        "hwservicemanager",
        "servicemanager",
        "vndservicemanager",
        NULL
    };
    int n = snprintf(binder_exit_report, sizeof(binder_exit_report), "binder-exit:\r\n");

    for (int i = 0; svcs[i]; i++) {
        const char *bin;
        char *argv[3];
        char bin_copy[128];

        if (proc_running(svcs[i])) {
            LOGI("radio", "binder svc %s already running", svcs[i]);
            n += snprintf(binder_exit_report + n, sizeof(binder_exit_report) - (size_t)n,
                          "%s: already running\r\n", svcs[i]);
            continue;
        }
        bin = vendor_bin(svcs[i]);
        if (!bin) {
            LOGI("radio", "binder svc %s: binary not found (no /system, /vendor)", svcs[i]);
            n += snprintf(binder_exit_report + n, sizeof(binder_exit_report) - (size_t)n,
                          "%s: binary not found\r\n", svcs[i]);
            continue;
        }
        /* vendor_bin() returns a shared static buffer — snapshot it before
         * the next lookup (e.g. inside start_vendor_daemon()) overwrites it. */
        snprintf(bin_copy, sizeof(bin_copy), "%s", bin);
        argv[0] = (char *)svcs[i];
        /* MEMORY.md §4.3 #5 / §7.6: the real ROM's own vndservicemanager.rc
         * invokes it as "vndservicemanager /dev/vndbinder" — the binder
         * device path is a required argv[1], not implied. servicemanager
         * and hwservicemanager both run with zero arguments on the real
         * ROM (confirmed live via /proc/<pid>/cmdline) and default to
         * /dev/binder / /dev/hwbinder internally — only vndservicemanager
         * needs this explicit argument. Without it, vndservicemanager was
         * very plausibly either failing to init or fighting servicemanager
         * over /dev/binder, matching the observed running=0. */
        if (!strcmp(svcs[i], "vndservicemanager")) {
            argv[1] = (char *)"/dev/vndbinder";
            argv[2] = NULL;
        } else {
            argv[1] = NULL;
        }
        pid_t p = start_vendor_daemon(bin_copy, argv);
        if (p > 0 && !strcmp(svcs[i], "hwservicemanager"))
            hwservicemanager_pid = p;
        if (p > 0) {
            LOGI("radio", "binder svc %s started from %s", svcs[i], bin_copy);
            /* Give it a moment, then check whether it's actually still
             * alive — a linker/runtime failure exits within milliseconds,
             * long before proc_running() would be checked elsewhere, and
             * start_vendor_daemon() itself never waits or reports this. */
            usleep(400000);
            int st = 0;
            pid_t w = waitpid(p, &st, WNOHANG);
            if (w == p) {
                /* This is the very first point where we can actually see
                 * why a binder service died — the only prior visibility
                 * into this was the live-only binder-state COM command's
                 * binder_exit_report buffer, which doesn't survive a
                 * reboot. plog_append() it too so it's visible on SD after
                 * the fact (see check_hwservicemanager_late_exit() above:
                 * before this fix, its later waitpid() call on an
                 * already-reaped PID just returned ECHILD and printed a
                 * misleading "not yet reaped" — the real reason was always
                 * captured right here, just never persisted). */
                char sdline[96];
                if (WIFEXITED(st)) {
                    n += snprintf(binder_exit_report + n, sizeof(binder_exit_report) - (size_t)n,
                                  "%s: exited status=%d\r\n", svcs[i], WEXITSTATUS(st));
                    snprintf(sdline, sizeof(sdline), "%s: exited status=%d", svcs[i], WEXITSTATUS(st));
                } else if (WIFSIGNALED(st)) {
                    n += snprintf(binder_exit_report + n, sizeof(binder_exit_report) - (size_t)n,
                                  "%s: killed by signal=%d\r\n", svcs[i], WTERMSIG(st));
                    snprintf(sdline, sizeof(sdline), "%s: killed by signal=%d (%s)",
                             svcs[i], WTERMSIG(st), strsignal(WTERMSIG(st)));
                } else {
                    n += snprintf(binder_exit_report + n, sizeof(binder_exit_report) - (size_t)n,
                                  "%s: wait status=0x%x\r\n", svcs[i], (unsigned)st);
                    snprintf(sdline, sizeof(sdline), "%s: wait status=0x%x", svcs[i], (unsigned)st);
                }
                plog_append(sdline);
                if (!strcmp(svcs[i], "hwservicemanager"))
                    hwservicemanager_pid = 0; /* already reaped here */
            } else {
                n += snprintf(binder_exit_report + n, sizeof(binder_exit_report) - (size_t)n,
                              "%s: still alive after 400ms\r\n", svcs[i]);
            }
        } else {
            LOGI("radio", "binder svc %s: start_vendor_daemon failed (%s)", svcs[i], bin_copy);
            n += snprintf(binder_exit_report + n, sizeof(binder_exit_report) - (size_t)n,
                          "%s: fork failed\r\n", svcs[i]);
        }
        usleep(300000);
    }
    plog_save_tmp_logs();
}


void ensure_cnss_sockets(void)
{
    md("/dev/socket");
    chmod("/dev/socket", 0777);
    md("/dev/socket/netmgr");

    if (access("/dev/socket/wpa_wlan0", F_OK) == 0)
        return;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/dev/socket/wpa_wlan0");
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        chmod("/dev/socket/wpa_wlan0", 0660);
        LOGI("radio", "%s", "cnss: wpa_wlan0 socket ok");
    }
    close(fd);
}


static void start_netmgrd(void)
{
    const char *bin;

    if (proc_running("netmgrd")) {
        plog_append("cnss: netmgrd already running");
        return;
    }
    md("/data/vendor/netmgr/recovery");
    bin = stage_vendor_bin("netmgrd");
    if (!bin) {
        plog_append("cnss: netmgrd not found, skipping");
        return;
    }
    {
        char *argv[] = { (char *)"netmgrd", NULL };
        pid_t p = start_vendor_daemon(bin, argv);
        char line[80];
        snprintf(line, sizeof(line), "cnss: netmgrd start pid=%d", (int)p);
        plog_append(line);
        usleep(400000);
        snprintf(line, sizeof(line), "cnss: netmgrd alive=%d",
                 proc_running("netmgrd"));
        plog_append(line);
    }
}


/* Passive readiness check for pd-mapper's config source, used instead of
 * calling ensure_modem_firmware_mounted() directly at each pd-mapper spawn
 * site: that function can itself (re)mount /vendor/firmware_mnt
 * (mount_modem_image() does umount2()+mount()), and start_cnss_stack()/
 * start_modem_qmi_services() each run in their own forked child (see the
 * cnss_qrtr_pid comment above) with independent, non-synchronized copies of
 * the modem_mounted global -- calling it from both concurrently raced two
 * mount()s against each other and changed pd-mapper's opendir() failure
 * from ENOENT to EINVAL (confirmed live via the opendir LD_PRELOAD trace),
 * i.e. made it worse, not better. Just wait for the directory to already be
 * populated (whoever's mounting it gets there on its own -- confirmed
 * elsewhere in boot, well before either of these call sites normally run)
 * without touching mount state ourselves. Bounded at 10s so a genuinely
 * stuck/missing modem partition doesn't hang pd-mapper's spawn forever. */
static void wait_modem_fw_dir_ready(void)
{
    for (int i = 0; i < 20; i++) {
        if (path_exists("/vendor/firmware_mnt/image/modem.mdt") ||
            path_exists("/vendor/firmware_mnt/image/wlanmdsp.mbn"))
            return;
        usleep(500000);
    }
}

/* Workaround, not a fix: pd-mapper's config-directory scan of
 * /vendor/firmware_mnt/image (its first, normal candidate) gets a real,
 * reproducible ENOENT from its own forked+exec_via_linker64 process for
 * this one specific vfat-mounted subdirectory -- confirmed via an
 * LD_PRELOAD opendir()/stat() trace inside pd-mapper itself, and confirmed
 * NOT a narrow timing race: this init process (never itself exec'd, just
 * running continuously since boot) polled the exact same path every 5s
 * from t=5s to t=100s on a fresh boot and saw it fine every single time,
 * while every exec_via_linker64-spawned daemon (qrtr-ns, cnss-daemon,
 * pd-mapper alike) failed on it across a wide t=9-35s+ window. Root cause
 * not yet found (MEMORY.md §4.5b8) -- this sidesteps it entirely by
 * pre-staging the same .jsn files pd-mapper needs into
 * /vendor/firmware, its own THIRD candidate directory, which its
 * opendir() has been confirmed to open successfully every time. */
static void stage_pdmapper_jsn_files(void)
{
    static const char *names[] = {
        "modemr.jsn", "modemuw.jsn", "adspr.jsn", "adsps.jsn",
        "adspua.jsn", "cdspr.jsn", NULL
    };
    char src[192], dst[192];
    int copied = 0;

    if (path_exists("/vendor/firmware/modemuw.jsn"))
        return;

    if (mount(NULL, "/vendor", NULL, MS_REMOUNT, NULL) != 0) {
        LOGI("radio", "%s", "pdmapper-jsn: remount /vendor rw failed");
        return;
    }
    for (int i = 0; names[i]; i++) {
        snprintf(src, sizeof(src), "/vendor/firmware_mnt/image/%s", names[i]);
        snprintf(dst, sizeof(dst), "/vendor/firmware/%s", names[i]);
        if (path_exists(src) && copy_file_bin(src, dst) == 0)
            copied++;
    }
    mount(NULL, "/vendor", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "pdmapper-jsn: staged %d files", copied);
        LOGI("radio", "%s", msg);
    }
}


void start_modem_qmi_services(void)
{
    const char *qrtr, *pdmap, *irsc;

    if (proc_running("qrtr-ns") && proc_running("pd-mapper")) {
        LOGI("radio", "%s", "modem qmi: already up");
        return;
    }

    if (!vendor_mounted)
        mount_vendor_partition();
    if (!vendor_mounted) {
        LOGI("radio", "%s", "modem qmi: no vendor");
        return;
    }

    ensure_android_roots();
    /* stage_cnss_libs() before start_binder_services() — see comment in
     * start_cnss_stack(); hwservicemanager/vndservicemanager need their
     * HIDL shared libs staged before they can start. */
    stage_cnss_libs();
    ensure_binder_nodes();
    start_binder_services();
    usleep(300000);
    ensure_cnss_sockets();
    start_logd_stub();

    qrtr = stage_vendor_bin("qrtr-ns");
    if (qrtr && !proc_running("qrtr-ns")) {
        char *argv[] = { (char *)"qrtr-ns", (char *)"-f", NULL };
        cnss_qrtr_pid = start_vendor_daemon_dropped(qrtr, argv, cnss_drop_to_vendor_qrtr);
        if (cnss_qrtr_pid > 0) {
            write_qrtr_pid_file(cnss_qrtr_pid);
            {
                char smsg[64];
                snprintf(smsg, sizeof(smsg), "modem qmi: qrtr-ns started pid=%d", (int)cnss_qrtr_pid);
                LOGI("radio", "%s", smsg);
                plog_append(smsg);
            }
            {
                char rmsg[128];
                if (reap_child_status_poll(cnss_qrtr_pid, "modem qmi: qrtr-ns", rmsg,
                                           sizeof(rmsg), 5000)) {
                    LOGI("radio", "%s", rmsg);
                    plog_append(rmsg);
                    cnss_log_line(rmsg);
                } else if (!proc_running("qrtr-ns")) {
                    LOGI("radio", "%s", "modem qmi: qrtr-ns died early (status unknown)");
                    plog_append("modem qmi: qrtr-ns died early (status unknown)");
                    cnss_log_line("modem qmi: qrtr-ns died early (status unknown)");
                }
            }
        }
    }

    irsc = stage_vendor_bin("irsc_util");
    if (irsc && path_exists("/vendor/etc/sec_config")) {
        char *argv[] = { (char *)"irsc_util", (char *)"/vendor/etc/sec_config", NULL };
        plog_append("modem qmi: irsc_util start (5s cap)");
        run_vendor_oneshot_timeout(irsc, argv, 5);
        plog_append("modem qmi: irsc_util done");
    }
    usleep(200000);

    /* See the matching comment in start_cnss_stack() -- defensive, cheap,
     * idempotent guard against spawning pd-mapper before its config
     * directory is populated. */
    wait_modem_fw_dir_ready();
    stage_pdmapper_jsn_files();

    pdmap = stage_vendor_bin("pd-mapper");
    if (pdmap && !proc_running("pd-mapper")) {
        char *argv[] = { (char *)"pd-mapper", NULL };
        cnss_pdmap_pid = start_vendor_daemon_dropped(pdmap, argv, cnss_drop_to_pd_mapper);
        if (cnss_pdmap_pid > 0) {
            LOGI("radio", "%s", "modem qmi: pd-mapper started");
            usleep(400000);
        }
    }

    LOGI("radio", "modem qmi: qrtr=%d pdmap=%d",
         proc_running("qrtr-ns"), proc_running("pd-mapper"));
    klogf2("modem qmi",
           proc_running("qrtr-ns") && proc_running("pd-mapper") ? "OK" : "partial");
}


void start_cnss_stack(void)
{
    const char *qrtr, *pdmap, *cnss, *irsc;

    if (!vendor_mounted)
        mount_vendor_partition();
    if (!vendor_mounted)
        LOGI("radio", "%s", "cnss: vendor not mounted — continuing with /sbin fallbacks");

    ensure_android_roots();
    ensure_wifi_config();
    ensure_cnss_devnodes();
    /* /dev/diag only reliably exists from this point on (ensure_cnss_
     * devnodes() above just mknod'd it) -- starting a diag reader any
     * earlier than this (tried: right after try_load_qrtr_snoop() in
     * main.c) meant it gave up after a 60s retry budget with /dev/diag
     * still not present. This call site is right before cnss-daemon/wlfw
     * negotiation even begins, so it still covers the whole MSA_READY ->
     * FW_MEM_READY window.
     *
     * Auto-starting the real vendor diag_mdlog here instead of our own
     * diag_capture (scratch/diag_capture.c) -- diag_mdlog is the mature,
     * correct tool (mask setup, HDLC framing, per-peripheral config all
     * handled properly instead of hand-rolled and getting subtly wrong);
     * it was previously confirmed absent from this device's actual live
     * /vendor (a stripped retail image, §4.5b4) but is now bundled
     * straight from the stock_miui eng-build reference dump into
     * minios/assets/vendor_radio/bin/ (its one dependency, libdiag.so,
     * was already bundled there from earlier work) -- build-initramfs.sh
     * already copies this whole assets tree into /vendor/bin as a
     * fallback whenever the live vendor mount doesn't have a given name,
     * so stage_vendor_bin("diag_mdlog") now finds it with no further
     * code changes needed. Don't run both diag_capture and diag_mdlog at
     * once -- they'd fight over the same /dev/diag MEMORY_DEVICE_MODE
     * switch; diag_capture remains available standalone via its own
     * "diag-capture" COM command if ever needed again. */
    start_diag_mdlog();
    /* stage_cnss_libs() must run BEFORE start_binder_services(): it copies
     * libhwbinder.so/libhidltransport.so/libhidlbase.so/libvintf.so etc.
     * into /lib64, which hwservicemanager/vndservicemanager need to even
     * start. Launching them first meant the dynamic linker couldn't find
     * those libs yet, so they exited immediately — the binder device nodes
     * were fine (confirmed live via binder-state) but the daemons never
     * stayed up; only servicemanager (fewer HIDL deps) survived. */
    stage_cnss_libs();
    ensure_binder_nodes();
    start_binder_services();
    usleep(500000);
    ensure_cnss_sockets();
    ensure_debugfs();
    start_logd_stub();

    /* adsprpcd and perfd skipped — booting ADSP/compute DSPs without full
     * vendor services causes subsystem SSR with SYSRESET level on SM6125. */

    qrtr = stage_vendor_bin("qrtr-ns");
    if (!qrtr)
        LOGI("radio", "%s", "cnss: qrtr-ns missing");
    else if (!proc_running("qrtr-ns")) {
        char *argv[] = { (char *)"qrtr-ns", (char *)"-f", NULL };
        cnss_qrtr_pid = start_vendor_daemon_dropped(qrtr, argv, cnss_drop_to_vendor_qrtr);
        if (cnss_qrtr_pid > 0) {
            write_qrtr_pid_file(cnss_qrtr_pid);
            {
                char smsg[64];
                snprintf(smsg, sizeof(smsg), "cnss: qrtr-ns started pid=%d", (int)cnss_qrtr_pid);
                LOGI("radio", "%s", smsg);
                plog_append(smsg);
            }
            {
                char rmsg[128];
                if (reap_child_status_poll(cnss_qrtr_pid, "cnss: qrtr-ns", rmsg,
                                           sizeof(rmsg), 5000)) {
                    LOGI("radio", "%s", rmsg);
                    plog_append(rmsg);
                    cnss_log_line(rmsg);
                } else if (!proc_running("qrtr-ns")) {
                    LOGI("radio", "%s", "cnss: qrtr-ns died early (status unknown)");
                    plog_append("cnss: qrtr-ns died early (status unknown)");
                    cnss_log_line("cnss: qrtr-ns died early (status unknown)");
                }
            }
        }
    }
    usleep(400000);

    /* irsc_util runs once at early boot (start_modem_qmi_services) with a 5s
     * cap — it can block forever on QMI when modem is OFFLINING. Do not repeat
     * here before boot_modem(); post-PIL retry is below after boot_modem(). */
    plog_append("cnss: irsc_util skipped pre-boot_modem (early boot)");
    usleep(200000);

    /* pd-mapper's config-directory scan (opendir() on
     * /vendor/firmware_mnt/image and its 3 other candidates) fails with
     * ENOENT for every one of them if spawned before that partition is
     * mounted/populated -- confirmed live via an LD_PRELOAD shim logging
     * pd-mapper's own opendir() calls directly (see minios/firmware/adb/
     * cnss_shim.c). Once that happens pd-mapper still binds its QMI server
     * and answers every servreg query with an empty domain list forever
     * (proven via a wire-level capture matching real Lineage's own
     * GET_DOMAIN_LIST protocol byte-for-byte, differing only in this
     * result) -- there is no retry inside pd-mapper itself. Unlike
     * start_modem_qmi_services() (which runs later in the overall boot
     * sequence, after firmware_mnt is guaranteed populated),
     * start_cnss_stack() has no such guarantee and was observed spawning
     * pd-mapper as early as t=9.7s, well before the modem partition is
     * ready. wait_modem_fw_dir_ready() (above) waits for it passively,
     * without touching mount state itself -- see its own comment for why
     * calling ensure_modem_firmware_mounted() directly here instead made
     * things worse (EINVAL from a real concurrent-mount race). */
    wait_modem_fw_dir_ready();
    stage_pdmapper_jsn_files();

    pdmap = stage_vendor_bin("pd-mapper");
    if (!pdmap)
        LOGI("radio", "%s", "cnss: pd-mapper missing");
    else if (!proc_running("pd-mapper")) {
        char *argv[] = { (char *)"pd-mapper", NULL };
        cnss_pdmap_pid = start_vendor_daemon_dropped(pdmap, argv, cnss_drop_to_pd_mapper);
        if (cnss_pdmap_pid > 0) {
            LOGI("radio", "%s", "cnss: pd-mapper started");
            plog_append("cnss: pd-mapper started");
            usleep(500000);
            if (!proc_running("pd-mapper"))
                LOGI("radio", "%s", "cnss: pd-mapper died early");
        }
    }
    usleep(400000);

    start_netmgrd();
    usleep(200000);

    /* EXPERIMENTAL (see MEMORY.md §4.5): on a real working LineageOS boot on
     * this exact hardware, `pm-service` (Qualcomm Peripheral Manager,
     * vendor.per_mgr) starts immediately after pd-mapper and before
     * cnss-daemon (confirmed via timestamped ps output: pd-mapper PID 1170,
     * pm-service PID 1171, cnss-daemon PID 1391, all within the same
     * second). cnss-daemon links libperipheral_client.so, which is presumed
     * to vote via pm-service to keep the modem subsystem powered during the
     * wlfw QMI handshake; without it, cnss-daemon spams a failing binder
     * transaction for ~35s and the modem then faults with an unreadable SFR.
     * A first attempt at this same experiment used LOGI() (kmsg-only) and
     * left literally zero trace anywhere — not even the "not found"
     * fallback line — so that test proved nothing either way. This retry
     * uses plog_append() (direct SD write + fsync, the same reliable path
     * used for the wlan: breadcrumbs below) specifically so this attempt
     * gives an unambiguous answer. Matches the adsprpcd/perfd risk noted
     * above: pm-service doesn't boot a DSP subsystem itself (just a
     * power/clock vote broker), so that specific SSR precedent may not
     * apply, but this is still an unconfirmed live experiment. */
    {
        const char *pmsvc = stage_vendor_bin("pm-service");
        if (!pmsvc) {
            plog_append("cnss: pm-service not found, skipping");
        } else if (!proc_running("pm-service")) {
            char *argv[] = { (char *)"pm-service", NULL };
            pid_t pm_pid = start_vendor_daemon(pmsvc, argv);
            if (pm_pid > 0) {
                char line[64];
                snprintf(line, sizeof(line), "cnss: pm-service started pid=%d", (int)pm_pid);
                plog_append(line);
                usleep(500000);
                plog_append(proc_running("pm-service") ?
                            "cnss: pm-service alive after 500ms" :
                            "cnss: pm-service died early");
            } else {
                plog_append("cnss: pm-service start failed");
            }
        } else {
            plog_append("cnss: pm-service already running");
        }
        usleep(200000);
    }

    /* REMOVED (see MEMORY.md §4.5, "BREAKTHROUGH" live-ROM comparison):
     * this used to block here on wait_modem_online(120) before ever
     * starting cnss-daemon. A live dmesg capture from a cold boot of the
     * real, working LineageOS on this exact hardware shows cnss-daemon's
     * own init.rc entry has NO modem-readiness dependency at all (plain
     * `class late_start`, starts alongside dozens of unrelated services)
     * and its wlfw_start fires ~1s after qrtr-ns starts — BEFORE modem is
     * even "Brought out of reset", not after modem reaches ONLINE. The
     * real system trusts cnss-daemon's own wlfw_start/QMI logic to
     * synchronize with modem readiness internally; nothing external gates
     * it. MiniOS's wait_modem_online(120) had no counterpart on real
     * hardware and forced a serialization (full modem ONLINE, then only
     * afterward start cnss-daemon) the real firmware may not tolerate well
     * — every previous test consistently panicked (`Fatal error on
     * modem!`) ~35s after wlfw_start regardless of what else changed.
     * boot_remote_procs() stays removed — booting CDSP/SLPI triggers SSR
     * sysreset on SM6125 when firmware auth fails without full Android
     * vendor services. WiFi (WCN3998 / CNSS) only needs modem (MPSS) +
     * qrtr, not CDSP/SLPI. */

    check_hwservicemanager_late_exit();

    cnss = vendor_bin("cnss-daemon");
    if (!cnss)
        LOGI("radio", "%s", "cnss: cnss-daemon missing");
    else if (!pid_alive(cnss_daemon_pid)) {
        cnss_daemon_pid = start_cnss_daemon(cnss);
        if (cnss_daemon_pid > 0) {
            LOGI("radio", "%s", "cnss: cnss-daemon started");
            usleep(800000);
            cnss_log_exit(cnss_daemon_pid);
            if (!proc_running("cnss-daemon") && !proc_cmdline_has("cnss-daemon")) {
                int st = 0;
                if (waitpid(cnss_daemon_pid, &st, WNOHANG) == cnss_daemon_pid)
                    cnss_log_exit(cnss_daemon_pid);
                else
                    LOGI("radio", "%s", "cnss: daemon died early");
                cnss_daemon_pid = 0;
            }
        }
    }

    /* boot_modem() runs HERE — after qrtr-ns/pd-mapper/pm-service/cnss-daemon
     * are all confirmed up — matching the one ordering this project has ever
     * captured a real "Brought out of reset" + full PIL trace with (see
     * project MEMORY.md §4.5v/§4.5aq: reordering to call boot_modem() last
     * produced a confirmed subsystem_get(modem)->Brought out of reset->QRTR
     * handshake sequence in a captured kernel panic trace; calling it earlier,
     * before cnss-daemon, is the one thing this project has direct evidence
     * against — do not move this earlier without re-reading that history). */
    ensure_rmtfs_firmware_paths();
    plog_append("cnss: boot_modem (post-cnss-daemon, matches MEMORY.md #4.5v/aq)");
    radio_trace("cnss: boot_modem now");
    boot_modem();
    radio_dump_qrtr("post boot_modem");
    usleep(500000);

    irsc = stage_vendor_bin("irsc_util");
    if (irsc && path_exists("/vendor/etc/sec_config")) {
        char *argv[] = { (char *)"irsc_util", (char *)"/vendor/etc/sec_config", NULL };
        plog_append("cnss: irsc_util post-boot_modem (5s cap)");
        run_vendor_oneshot_timeout(irsc, argv, 5);
        plog_append("cnss: irsc_util post-boot_modem done");
    }

    plog_append("cnss: stack up (boot_modem triggered late, post-cnss-daemon)");
}


int cnss_stack_running(void)
{
    return proc_running("qrtr-ns") && proc_running("cnss-daemon");
}


