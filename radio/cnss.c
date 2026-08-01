#include <sys/sysmacros.h>
#define _GNU_SOURCE
#include "cnss.h"
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
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <linux/capability.h>

extern char **environ;

extern int system_mounted, vendor_mounted;
extern pid_t cnss_qrtr_pid, cnss_pdmap_pid, cnss_daemon_pid;
#define CNSS_EXEC_LOG "/tmp/cnss.exec.log"
#define CNSS_BUILD_TAG "cnss-start v36 no-rproc"
void ensure_cnss_devnodes(void)
{
    char buf[32];
    unsigned maj, min;
    int fd;

    if (path_exists("/dev/diag"))
        return;
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
    mknod("/dev/diag", S_IFCHR | 0666, makedev(maj, min));
    LOGI("radio", "%s", "cnss: /dev/diag created");
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
    int fd = open(CNSS_EXEC_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dprintf(fd, "%s\n", msg);
        close(fd);
    }
}


void cnss_drop_to_system(void)
{
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    struct __user_cap_data_struct data[2];

    memset(data, 0, sizeof(data));
    data[0].effective = data[0].permitted = data[0].inheritable = (1U << CAP_NET_ADMIN);
    if (syscall(SYS_capset, &hdr, data) != 0)
        cnss_log_line("capset NET_ADMIN failed (ok as root)");
    /* No /etc/group in initramfs — setuid(1000) usually fails; stay root. */
    if (setgroups(0, NULL) == 0 && setgid(1000) == 0 && setuid(1000) == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "dropped uid=%d gid=%d", getuid(), getgid());
        cnss_log_line(msg);
    } else
        cnss_log_line("stay root (no /etc/group)");
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
        fd = open(CNSS_EXEC_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
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
        char *a0[] = { (char *)"cnss-daemon", (char *)"-n", (char *)"-l", NULL };
        char *a1[] = { (char *)"cnss-daemon", (char *)"-n", NULL };
        struct { char *label; char **argv; } tries[] = {
            { "try0: -n -l (init.rc)", a0 },
            { "try1: -n", a1 },
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
}


pid_t start_vendor_daemon(const char *path, char *const argv[])
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
    pid_t p = start_vendor_daemon(path, argv);
    if (p > 0)
        waitpid(p, NULL, 0);
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
    unlink("/lib64/libqrtr.so");
    if (path_exists("/vendor/lib64/libqrtr.so"))
        symlink_force("/vendor/lib64/libqrtr.so", "/lib64/libqrtr.so");
    else if (path_exists("/mnt/vendor/lib64/libqrtr.so"))
        symlink_force("/mnt/vendor/lib64/libqrtr.so", "/lib64/libqrtr.so");
    staged = 1;
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

    qrtr = stage_vendor_bin("qrtr-ns");
    if (qrtr && !proc_running("qrtr-ns")) {
        char *argv[] = { (char *)"qrtr-ns", (char *)"-f", NULL };
        cnss_qrtr_pid = start_vendor_daemon(qrtr, argv);
        if (cnss_qrtr_pid > 0) {
            LOGI("radio", "%s", "modem qmi: qrtr-ns started");
            usleep(400000);
        }
    }

    irsc = stage_vendor_bin("irsc_util");
    if (irsc && path_exists("/vendor/etc/sec_config")) {
        char *argv[] = { (char *)"irsc_util", (char *)"/vendor/etc/sec_config", NULL };
        run_vendor_oneshot(irsc, argv);
    }
    usleep(200000);

    pdmap = stage_vendor_bin("pd-mapper");
    if (pdmap && !proc_running("pd-mapper")) {
        char *argv[] = { (char *)"pd-mapper", NULL };
        cnss_pdmap_pid = start_vendor_daemon(pdmap, argv);
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

    /* adsprpcd and perfd skipped — booting ADSP/compute DSPs without full
     * vendor services causes subsystem SSR with SYSRESET level on SM6125. */

    qrtr = stage_vendor_bin("qrtr-ns");
    if (!qrtr)
        LOGI("radio", "%s", "cnss: qrtr-ns missing");
    else if (!proc_running("qrtr-ns")) {
        char *argv[] = { (char *)"qrtr-ns", (char *)"-f", NULL };
        cnss_qrtr_pid = start_vendor_daemon(qrtr, argv);
        if (cnss_qrtr_pid > 0) {
            LOGI("radio", "%s", "cnss: qrtr-ns started");
            usleep(500000);
            if (!proc_running("qrtr-ns"))
                LOGI("radio", "%s", "cnss: qrtr-ns died early");
        }
    }
    usleep(400000);

    irsc = stage_vendor_bin("irsc_util");
    if (irsc && path_exists("/vendor/etc/sec_config")) {
        char *argv[] = { (char *)"irsc_util", (char *)"/vendor/etc/sec_config", NULL };
        run_vendor_oneshot(irsc, argv);
        LOGI("radio", "%s", "cnss: irsc_util done");
    }
    usleep(200000);

    pdmap = stage_vendor_bin("pd-mapper");
    if (!pdmap)
        LOGI("radio", "%s", "cnss: pd-mapper missing");
    else if (!proc_running("pd-mapper")) {
        char *argv[] = { (char *)"pd-mapper", NULL };
        cnss_pdmap_pid = start_vendor_daemon(pdmap, argv);
        if (cnss_pdmap_pid > 0) {
            LOGI("radio", "%s", "cnss: pd-mapper started");
            usleep(500000);
            if (!proc_running("pd-mapper"))
                LOGI("radio", "%s", "cnss: pd-mapper died early");
        }
    }
    usleep(400000);

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

    /* Reordered per MEMORY.md live-ROM trace: the real ROM has cnss-daemon
     * (and its wlfw QMI listener) already up ~1s after qrtr-ns starts,
     * BEFORE the modem is even brought out of reset — nothing on the real
     * system waits for modem readiness before starting cnss-daemon. This
     * used to call boot_modem() before qrtr-ns/pd-mapper/cnss-daemon even
     * started, burning a chunk of the WLAN firmware's internal ~40s
     * handshake window before anything was listening. Trigger the modem
     * PIL boot last instead, once the whole QMI listener chain is already
     * up and ready to react the instant the WLAN co-processor announces
     * itself. */
    boot_modem();
}


int cnss_stack_running(void)
{
    return proc_running("qrtr-ns") && proc_running("cnss-daemon");
}


