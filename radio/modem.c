#define _GNU_SOURCE
#include "modem.h"
#include "radio.h"
#include "radio_state.h"
#include "radio_utils.h"
#include "cnss.h"
#include "wlan.h"
#include "firmware.h"
#include "minios/log.h"
#include "minios/watchdog.h"
#include "minios/plog.h"
#include "radio_utils.h"
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
#include <sys/prctl.h>
#include <grp.h>
#include <stdint.h>

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#endif
#ifndef PR_CAP_AMBIENT_RAISE
#define PR_CAP_AMBIENT_RAISE 2
#endif

#ifndef SYS_finit_module
#if defined(__aarch64__)
#define SYS_finit_module 379
#elif defined(__arm__)
#define SYS_finit_module 380
#else
#define SYS_finit_module 313
#endif
#endif
int load_ko_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int rc;

    if (fd < 0) {
        LOGI("radio", "%s %s", "modem ko open", path);
        return -1;
    }
    rc = (int)syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    if (rc != 0) {
        char ebuf[96];
        snprintf(ebuf, sizeof(ebuf), "errno=%d %s uid=%d",
                 errno, strerror(errno), getuid());
        LOGI("radio", "%s %s", "modem ko fail", ebuf);
        return -1;
    }
    LOGI("radio", "%s %s", "modem ko ok", path);
    return 0;
}


void try_load_modem_ko(void)
{
    const char *paths[] = {
        "/lib/minios_modem_boot.ko",
        "/lib/modules/minios_modem_boot.ko",
        NULL,
    };

    if (access("/sys/kernel/boot_modem/boot", W_OK) == 0)
        return;

    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], R_OK) != 0)
            continue;
        LOGI("radio", "%s %s", "modem ko try", paths[i]);
        if (load_ko_file(paths[i]) == 0 &&
            access("/sys/kernel/boot_modem/boot", W_OK) == 0) {
            LOGI("radio", "%s", "modem: boot_modem sysfs ready");
            return;
        }
    }
}


void radio_load_modem_ko(void)
{
    try_load_modem_ko();
}


/* Diagnostic: kprobe-based QRTR RX/TX packet snoop (see minios/kmod/qrtr_snoop.c).
 * Load as early as possible so it's active before qrtr-ns/cnss-daemon/modem
 * PIL even start — output goes to dmesg/boot.log/pstore via printk, no new
 * capture plumbing needed. Best-effort: absence of this .ko must never
 * block boot. */
void try_load_qrtr_snoop(void)
{
    const char *paths[] = {
        "/lib/qrtr_snoop.ko",
        "/lib/modules/qrtr_snoop.ko",
        NULL,
    };

    /* Diagnostic (this session): register_kprobe() for this module's two
     * symbols (qrtr_endpoint_post, qcom_smd_qrtr_send) fails with -ENOSYS
     * every time this .ko is the very first kprobe-based module inserted
     * after boot (~1.68s), while identical register_kprobe() calls in
     * qrtr_snoop_local.ko/open_snoop.ko succeed cleanly ~16-20ms later —
     * confirmed via kprobes.c-level tracing that NONE of register_kprobe()'s
     * own exit paths are hit for these two calls, a genuine unexplained
     * paradox. Testing whether this is purely an early-boot timing race by
     * delaying this specific insmod; remove if this doesn't fix it. */
    usleep(150000);

    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], R_OK) != 0)
            continue;
        if (load_ko_file(paths[i]) == 0) {
            LOGI("radio", "%s %s", "qrtr_snoop ko ok", paths[i]);
            break;
        }
    }

    /* Diagnostic: kprobe on qrtr_local_enqueue() (net/qrtr/qrtr.c) — sees
     * QRTR traffic delivered between two endpoints on THIS processor
     * (kernel<->userspace, e.g. icnss's wlan_pd servreg lookup against
     * pd-mapper), which qrtr_snoop.ko's RX/TX hooks above never see (those
     * only capture packets crossing to/from the modem chip). Validated
     * safe: a full real-Lineage cold boot with this hook armed captured
     * the actual wlan/fw GET_DOMAIN_LIST exchange with pd-mapper cleanly,
     * no crash, no reboot. */
    {
        const char *lpaths[] = {
            "/lib/qrtr_snoop_local.ko",
            "/lib/modules/qrtr_snoop_local.ko",
            NULL,
        };

        for (int i = 0; lpaths[i]; i++) {
            if (access(lpaths[i], R_OK) != 0)
                continue;
            if (load_ko_file(lpaths[i]) == 0) {
                LOGI("radio", "%s %s", "qrtr_snoop_local ko ok", lpaths[i]);
                break;
            }
        }
    }

    /* Diagnostic (MEMORY.md §4.5ay): filtered do_filp_open kprobe, checks
     * whether MiniOS's own irsc_util reaches the same persist/rfs/modemst
     * EFS-sync paths real Android's irsc_util opens right before WLAN FW
     * ready. Same best-effort loading pattern as qrtr_snoop above. */
    {
        const char *ospaths[] = {
            "/lib/open_snoop.ko",
            "/lib/modules/open_snoop.ko",
            NULL,
        };
        for (int i = 0; ospaths[i]; i++) {
            if (access(ospaths[i], R_OK) != 0)
                continue;
            if (load_ko_file(ospaths[i]) == 0) {
                LOGI("radio", "%s %s", "open_snoop ko ok", ospaths[i]);
                break;
            }
        }
    }
}


int read_subsys_state(const char *subsys, char *buf, size_t bufsz)
{
    char path[128];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/sys/bus/msm_subsys/devices/%s/state", subsys);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    {
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = '\0';
    }
    return 0;
}


void boot_remote_procs(void)
{
    const char *boots[] = {
        "/sys/kernel/boot_cdsp/boot",
        "/sys/kernel/boot_slpi/boot",
        NULL
    };

    for (int i = 0; boots[i]; i++) {
        if (access(boots[i], W_OK) == 0) {
            wf_boot(boots[i]);
            LOGI("radio", "%s %s", "boot", boots[i]);
        }
    }
}


int wait_modem_leave_offlining(char *st, size_t stsz, int max_sec)
{
    for (int i = 0; i < max_sec * 2; i++) {
        if (read_subsys_state("subsys0", st, stsz) != 0) {
            strncpy(st, "unknown", stsz);
            return -1;
        }
        if (!strcmp(st, "ONLINE"))
            return 2;
        if (strcmp(st, "OFFLINING"))
            return 1;
        if (i == 0 || (i % 20) == 0)
            LOGI("radio", "%s %s", "modem offlining wait", st);
        usleep(500000);
    }
    return 0;
}


/* The modem polls the RMTFS/rmt_storage QMI service for its EFS/NV
 * partitions (modem_fs1/modem_fs2/modem_fsg) for a fixed period after
 * coming out of reset, then self-asserts a fatal error if it never
 * answers (matches the ~40.0s modem-reset-to-panic timer measured this
 * session, MEMORY.md §4.5/§4.5f). The real ROM starts this unconditionally
 * on `on boot` well before modem PIL triggers (vendor.qti.rmt_storage.rc:
 * `service vendor.rmt_storage /vendor/bin/rmt_storage`, class core, user
 * root, no args) — MiniOS never started it at all. */
static void start_rmt_storage(void)
{
    if (proc_running("rmt_storage"))
        return;
    const char *bin = stage_vendor_bin("rmt_storage");
    if (!bin) {
        LOGI("radio", "%s", "rmt_storage: not found on vendor");
        return;
    }
    char *argv[] = { (char *)"rmt_storage", NULL };
    pid_t p = start_vendor_daemon(bin, argv);
    LOGI("radio", "rmt_storage: start pid=%d", (int)p);
    plog_append(p > 0 ? "rmt_storage: started" : "rmt_storage: start failed");
    if (p > 0) {
        char rmsg[128];
        if (reap_child_status_poll(p, "rmt_storage", rmsg, sizeof(rmsg), 2000)) {
            LOGI("radio", "%s", rmsg);
            plog_append(rmsg);
        } else if (!proc_running("rmt_storage")) {
            LOGI("radio", "%s", "rmt_storage: died early (status unknown)");
            plog_append("rmt_storage: died early (status unknown)");
        }
    }
}

/* Real ROM starts this unconditionally too: vendor.qti.tftp.rc, class core,
 * user root, no args, `on post-fs-data: mkdir /data/vendor/pddump 0770
 * oem_2903 oem_2903` (oem_2903 = vendor_rfs, gid 2903, confirmed via the
 * real vendor/etc/group pulled this session). Its only known purpose is a
 * modem-crash panic-dump directory — low-plausibility for the wlfw crash
 * (§4.5g), added anyway per the user's own "add the still-missing
 * known-real-ROM daemons" request rather than left as a permanent gap. */
static void start_tftp_server(void)
{
    if (proc_running("tftp_server"))
        return;
    if (mkdir("/data/vendor/pddump", 0770) != 0 && errno != EEXIST)
        LOGI("radio", "tftp_server: mkdir pddump failed errno=%d", errno);
    else
        chown("/data/vendor/pddump", 2903, 2903);
    const char *bin = stage_vendor_bin("tftp_server");
    if (!bin) {
        LOGI("radio", "%s", "tftp_server: not found on vendor");
        return;
    }
    char *argv[] = { (char *)"tftp_server", NULL };
    pid_t p = start_vendor_daemon(bin, argv);
    LOGI("radio", "tftp_server: start pid=%d", (int)p);
    plog_append(p > 0 ? "tftp_server: started" : "tftp_server: start failed");
    if (p > 0) {
        char rmsg[128];
        if (reap_child_status_poll(p, "tftp_server", rmsg, sizeof(rmsg), 2000)) {
            LOGI("radio", "%s", rmsg);
            plog_append(rmsg);
        } else if (!proc_running("tftp_server")) {
            LOGI("radio", "%s", "tftp_server: died early (status unknown)");
            plog_append("tftp_server: died early (status unknown)");
        }
    }
}

/* Diagnostic only (not part of normal boot): routes modem DIAG F3 debug
 * messages into the AP's own kernel log via klogctl(), so `dmesg` shows
 * modem-firmware-side log lines directly -- no QXDM/database decoding
 * needed since diag_klog does the formatting itself using the on-device
 * libdiag.so. Used to investigate why wlfw never registers (MEMORY.md
 * §4.5b1/b2): every AP-side signal (RFS, PDR/locator, kernel QMI client)
 * is confirmed healthy, so the remaining question is what the modem's own
 * firmware is doing/waiting on, which only a modem-side log can show. */
void start_diag_klog(void)
{
    if (proc_running("diag_klog"))
        return;
    const char *bin = stage_vendor_bin("diag_klog");
    if (!bin) {
        LOGI("radio", "%s", "diag_klog: not found on vendor");
        return;
    }
    char *argv[] = { (char *)"diag_klog", NULL };
    pid_t p = start_vendor_daemon(bin, argv);
    LOGI("radio", "diag_klog: start pid=%d", (int)p);
    plog_append(p > 0 ? "diag_klog: started" : "diag_klog: start failed");
}

/* diag_klog (above) turned out to be a klog->diag *forwarder* (reads the
 * AP's own kernel log via klogctl(SYSLOG_ACTION_READ) and pushes it out
 * over diag) -- confirmed via Ghidra decompile of its main(), not a way to
 * see modem-side messages at all. diag_mdlog is the real modem log
 * capture tool (writes .qmdl files); run with no mask file (-f) for a
 * default capture -- some F3 messages carry literal ASCII text even
 * without a proper QCAT/QXDM message database, so a binary substring
 * search on the .qmdl may already show something readable. Output goes
 * to /tmp (tmpfs, always available, no removable-media mount fragility)
 * -- read back over COM via the qmdl-find command (com.c), no physical
 * SD swap needed. Small -s cap since this is RAM-backed. */
void start_diag_mdlog(void)
{
    if (proc_running("diag_mdlog"))
        return;
    const char *bin = stage_vendor_bin("diag_mdlog");
    if (!bin) {
        LOGI("radio", "%s", "diag_mdlog: not found on vendor");
        return;
    }
    mkdir("/tmp/diagmdlog", 0755);
    char *argv[] = {
        (char *)"diag_mdlog",
        (char *)"-o", (char *)"/tmp/diagmdlog",
        (char *)"-s", (char *)"2",
        NULL
    };
    pid_t p = start_vendor_daemon(bin, argv);
    LOGI("radio", "diag_mdlog: start pid=%d", (int)p);
    plog_append(p > 0 ? "diag_mdlog: started" : "diag_mdlog: start failed");
}

/* diag_mdlog is the real vendor tool but this device's actual /vendor
 * doesn't ship it (§4.5b4 — confirmed via vendor-has, only present in the
 * stock_miui reference dump, an eng-build artifact). diag_capture
 * (scratch/diag_capture.c, MiniOS's own, staged to /sbin regardless of
 * vendor content) opens /dev/diag directly in MEMORY_DEVICE_MODE and dumps
 * every raw DIAG packet to a file — no QXDM/QPST needed, just a binary
 * substring search afterward for embedded F3/ERR/FATAL ASCII text. Static
 * ELF (like logd_stub) -- direct fork+execv, no linker64. */
void start_diag_capture(void)
{
    static const char *path = "/sbin/diag_capture";

    if (proc_running("diag_capture") || !path_exists(path))
        return;
    char *argv[] = { (char *)"diag_capture", (char *)"/tmp/diag_raw.bin", NULL };
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) { dup2(fd, 0); close(fd); }
        fd = open("/tmp/diag_capture.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execv(path, argv);
        _exit(127);
    }
    LOGI("radio", "diag_capture: start pid=%d", (int)p);
    plog_append(p > 0 ? "diag_capture: started" : "diag_capture: start failed");
}

/* Both daemons above used to only start deep inside boot_modem(), which
 * itself (per §4.5v) now deliberately runs LAST — after qrtr-ns/pd-mapper/
 * cnss-daemon are already up, to match real-ROM's observed modem-PIL-
 * trigger timing. But start_rmt_storage()'s own comment already establishes
 * that the real ROM starts *rmt_storage itself* far earlier than that — on
 * `boot`, before modem PIL triggers at all — and the modem's own internal
 * software begins polling RMTFS for its EFS/NV partitions immediately once
 * it comes out of reset. Deferring rmt_storage/tftp_server until after the
 * whole cnss stack (and thus after modem PIL trigger) conflated "when to
 * kick off modem PIL boot" with "when the RMTFS *server* needs to already
 * be listening" — real ROM decouples these; this does too now. Call this
 * early in radio_work(), before the cnss stack, so both daemons are already
 * up well before modem PIL (still triggered later, unchanged) ever starts
 * polling. Both start_*() calls already no-op via proc_running() if called
 * again later from boot_modem() — safe, cheap, not removed there. */
void start_rmtfs_daemons_early(void)
{
    radio_trace("start_rmtfs_daemons_early");
    start_rmt_storage();
    start_tftp_server();
    plog_append(proc_running("rmt_storage") ?
                "rmtfs: rmt_storage alive" : "rmtfs: rmt_storage not running");
}

/* Real identity per init.qcom.rc: `service ssgqmigd ... class late_start
 * user radio group radio gps system`, plus a pre-created seqpacket socket
 * `ssgqmig` this probe does NOT create (Android's init binds+passes control
 * sockets via ANDROID_SOCKET_<name> before exec; MiniOS has no equivalent
 * yet) — same "probe first, real socket plumbing later if this ever shows
 * signs of mattering" approach as §4.5h's first RILD attempt. gid 1021
 * (gps) and 1000 (system) are standard, version-stable AOSP AIDs, not
 * device-specific — unlike radio/log/inet/etc in qcrild_groups above, these
 * were not re-verified live this session (no root session available), flag
 * if this ever needs to be trusted for something more than a probe. */
static const gid_t ssgqmigd_groups[] = { 1001 /* radio */, 1021 /* gps */, 1000 /* system */ };

static void ssgqmigd_drop_to_radio(void)
{
    if (setgroups(sizeof(ssgqmigd_groups) / sizeof(ssgqmigd_groups[0]), ssgqmigd_groups) != 0)
        LOGI("radio", "ssgqmigd: setgroups failed errno=%d", errno);
    if (setgid(1001) != 0)
        LOGI("radio", "ssgqmigd: setgid failed errno=%d", errno);
    if (setuid(1001) != 0)
        LOGI("radio", "ssgqmigd: setuid failed errno=%d", errno);
    LOGI("radio", "ssgqmigd: dropped to uid=%d gid=%d", (int)getuid(), (int)getgid());
}

static void start_ssgqmigd_probe(void)
{
    const char *bin;
    pid_t p;

    if (proc_running("ssgqmigd"))
        return;
    bin = stage_vendor_bin("ssgqmigd");
    if (!bin) {
        LOGI("radio", "%s", "ssgqmigd: not found on vendor");
        return;
    }
    p = fork();
    if (p < 0) {
        LOGI("radio", "%s", "ssgqmigd: fork failed");
        return;
    }
    if (p == 0) {
        char *argv[] = { (char *)"ssgqmigd", NULL };
        daemon_child_setup(bin, "/tmp/ssgqmigd.log");
        ssgqmigd_drop_to_radio();
        execv(bin, argv);
        exec_via_linker64(bin, argv);
        _exit(127);
    }
    LOGI("radio", "ssgqmigd: start pid=%d (probe, no socket, user radio)", (int)p);
}

/* Real identity per imsqmidaemon.rc: `class main user radio group radio
 * vendor_qti_diag log`, plus a pre-created stream socket `ims_qmid` this
 * probe does NOT create — same caveat as ssgqmigd above. §4.5g flagged this
 * as IMS-related and unconfirmed for WLAN specifically; probing anyway per
 * the user's explicit request rather than leaving it untried. */
static const gid_t imsqmidaemon_groups[] = { 1001 /* radio */, 2901 /* vendor_qti_diag */, 1007 /* log */ };

static void imsqmidaemon_drop_to_radio(void)
{
    if (setgroups(sizeof(imsqmidaemon_groups) / sizeof(imsqmidaemon_groups[0]), imsqmidaemon_groups) != 0)
        LOGI("radio", "imsqmidaemon: setgroups failed errno=%d", errno);
    if (setgid(1001) != 0)
        LOGI("radio", "imsqmidaemon: setgid failed errno=%d", errno);
    if (setuid(1001) != 0)
        LOGI("radio", "imsqmidaemon: setuid failed errno=%d", errno);
    LOGI("radio", "imsqmidaemon: dropped to uid=%d gid=%d", (int)getuid(), (int)getgid());
}

static void start_imsqmidaemon_probe(void)
{
    const char *bin;
    pid_t p;

    if (proc_running("imsqmidaemon"))
        return;
    bin = stage_vendor_bin("imsqmidaemon");
    if (!bin) {
        LOGI("radio", "%s", "imsqmidaemon: not found on vendor");
        return;
    }
    p = fork();
    if (p < 0) {
        LOGI("radio", "%s", "imsqmidaemon: fork failed");
        return;
    }
    if (p == 0) {
        char *argv[] = { (char *)"imsqmidaemon", NULL };
        daemon_child_setup(bin, "/tmp/imsqmidaemon.log");
        imsqmidaemon_drop_to_radio();
        execv(bin, argv);
        exec_via_linker64(bin, argv);
        _exit(127);
    }
    LOGI("radio", "imsqmidaemon: start pid=%d (probe, no socket, user radio)", (int)p);
}

/* Drop from root to exactly what the real ROM's qcrild.rc declares:
 *   user radio
 *   group radio cache inet misc audio log readproc wakelock oem_2901
 *   capabilities BLOCK_SUSPEND NET_ADMIN NET_RAW
 * (verified live via `id radio; id cache; id inet; id misc; id audio;
 * id log; id readproc; id wakelock; id oem_2901` on the real ROM, and the
 * exact capability set + names from /vendor/etc/init/qcrild.rc itself —
 * not guessed). The §4.5h probe ran this as plain root, which got RILD
 * running but never moved the WLAN crash timer at all; this is the first
 * attempt at giving it its *real* identity, in case the modem's own
 * internal logic (or SELinux, though we run permissive) gates something
 * on RILD's actual uid/gid/capabilities rather than merely "a RIL-shaped
 * client exists". Capabilities must go through the *ambient* set (not
 * just permitted/effective) to survive the setuid() below — that's the
 * whole point of Android's own `capabilities` service-manifest keyword. */
static const gid_t qcrild_groups[] = {
    1001 /* radio */, 2001 /* cache */, 3003 /* inet */, 9998 /* misc */,
    1005 /* audio */, 1007 /* log */, 3009 /* readproc */,
    3010 /* wakelock */, 2901 /* oem_2901 */,
};
#define QCRILD_UID 1001  /* radio */
#define QCRILD_GID 1001  /* radio (primary group) */

static void qcrild_drop_to_radio(void)
{
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    struct __user_cap_data_struct data[2];
    static const int caps[] = { CAP_BLOCK_SUSPEND, CAP_NET_ADMIN, CAP_NET_RAW };

    memset(data, 0, sizeof(data));
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        int c = caps[i];
        uint32_t bit = 1u << (c & 31);
        int w = (c < 32) ? 0 : 1;

        data[w].effective |= bit;
        data[w].permitted |= bit;
        data[w].inheritable |= bit;
    }
    if (syscall(SYS_capset, &hdr, data) != 0) {
        LOGI("radio", "qcrild: capset failed errno=%d", errno);
        return;
    }
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, caps[i], 0, 0) != 0)
            LOGI("radio", "qcrild: ambient raise cap=%d failed errno=%d", caps[i], errno);
    }
    if (setgroups(sizeof(qcrild_groups) / sizeof(qcrild_groups[0]), qcrild_groups) != 0)
        LOGI("radio", "qcrild: setgroups failed errno=%d", errno);
    if (setgid(QCRILD_GID) != 0)
        LOGI("radio", "qcrild: setgid failed errno=%d", errno);
    if (setuid(QCRILD_UID) != 0)
        LOGI("radio", "qcrild: setuid failed errno=%d", errno);
    LOGI("radio", "qcrild: dropped to uid=%d gid=%d", (int)getuid(), (int)getgid());
}

/* §4.5h ran RILD as plain root (a diagnostic probe, not this). This version
 * gives it its real identity (see qcrild_drop_to_radio() above) — a bigger,
 * still-not-guaranteed-to-help step, per the user's own explicit request to
 * try it after the targeted-MODEM-F3-log attempt (§4.5p) came back negative. */
static void start_qcrild_probe(void)
{
    const char *bin;
    pid_t p;

    if (proc_running("qcrild"))
        return;
    bin = stage_vendor_bin("qcrild");
    if (!bin) {
        LOGI("radio", "%s", "qcrild: not found on vendor");
        return;
    }

    p = fork();
    if (p < 0) {
        LOGI("radio", "%s", "qcrild: fork failed");
        return;
    }
    if (p == 0) {
        char *argv[] = { (char *)"qcrild", NULL };
        char logfile[64];
        snprintf(logfile, sizeof(logfile), "/tmp/qcrild.log");
        daemon_child_setup(bin, logfile);
        qcrild_drop_to_radio();
        execv(bin, argv);
        exec_via_linker64(bin, argv);
        _exit(127);
    }
    LOGI("radio", "qcrild: start pid=%d (user radio)", (int)p);
}


static void modem_state_watch_start(void)
{
    pid_t p = fork();

    if (p != 0)
        return;
    for (int i = 0; i < 25; i++) {
        char st[32], line[160];

        if (read_subsys_state("subsys0", st, sizeof(st)) != 0)
            snprintf(st, sizeof(st), "?");
        snprintf(line, sizeof(line),
                 "modem_watch +%ds state=%s qrtr=%d pdmap=%d rmt=%d cnss=%d wlfw=%d",
                 i * 2, st,
                 proc_running("qrtr-ns"), proc_running("pd-mapper"),
                 proc_running("rmt_storage"), proc_running("cnss-daemon"),
                 qrtr_has_wlfw());
        plog_append(line);
        sleep(2);
    }
    _exit(0);
}


static void modem_try_ssr_reset(void)
{
    const char *ssr = "/sys/kernel/boot_adsp/ssr";

    if (access(ssr, W_OK) != 0)
        return;
    LOGI("radio", "%s", "modem: boot_adsp SSR reset");
    plog_append("modem: boot_adsp SSR reset");
    wf_boot(ssr);
    usleep(2000000);
}


void radio_modem_recover_stuck(void)
{
    char st[32] = "?";

    if (read_subsys_state("subsys0", st, sizeof(st)) != 0)
        return;
    /* SUBSYS_OFFLINING=0 is the kzalloc default in subsystem_restart.c —
     * not proof of a stuck shutdown.  boot_adsp SSR before the first
     * subsystem_get("modem") is a no-op (pil_h NULL → -EINVAL). */
    LOGI("radio", "%s %s", "modem recover boot", st);
    plog_append("modem-recover: boot state (informational only)");
    plog_append(st);
}


void boot_modem(void)
{
    const char *direct = "/sys/kernel/boot_modem/boot";
    char st[32] = "?";

    radio_trace("boot_modem: enter");
    plog_append("boot_modem: begin");
    ensure_rmtfs_firmware_paths();
    try_load_modem_ko();

    if (ensure_modem_pil_firmware() != 0) {
        LOGI("radio", "%s", "modem: PIL firmware incomplete — continuing anyway");
        plog_append("modem: PIL firmware incomplete — continuing");
    }

    read_subsys_state("subsys0", st, sizeof(st));
    LOGI("radio", "%s %s", "modem before", st);
    {
        char line[64];
        snprintf(line, sizeof(line), "modem before state=%s", st);
        plog_append(line);
    }
    if (!strcmp(st, "ONLINE")) {
        LOGI("radio", "%s", "modem: already ONLINE");
        return;
    }
    if (!strcmp(st, "OFFLINING")) {
        /* enum value 0 before first subsystem_get — usually uninitialized,
         * not an active shutdown in subsys_stop().  Do not wait 30s. */
        LOGI("radio", "%s", "modem: OFFLINING (default) — booting");
        plog_append("modem: OFFLINING likely uninitialized — booting");
    }

    link_modem_pil_firmware();
    if (ensure_modem_pil_firmware() != 0) {
        LOGI("radio", "%s", "modem: PIL fw missing after link");
        plog_append("modem: PIL fw missing after link");
    }

    start_rmt_storage();
    start_tftp_server();
    start_qcrild_probe();
    start_ssgqmigd_probe();
    start_imsqmidaemon_probe();

    run_sh("setenforce 0 2>/dev/null");

    if (access(direct, W_OK) == 0) {
        wf(direct, "1");
        LOGI("radio", "%s", "modem: boot_modem sysfs");
        plog_append("modem: boot_modem sysfs=1");
    } else if (access("/sys/kernel/boot_adsp/boot", W_OK) == 0) {
        wf_boot("/sys/kernel/boot_adsp/boot");
        LOGI("radio", "%s %s", "modem: boot via adsp-loader", st);
        plog_append("modem: boot via boot_adsp");
    } else {
        LOGI("radio", "%s", "modem: no boot interface");
        plog_append("modem: no boot interface");
        return;
    }

    modem_state_watch_start();
    radio_dump_qrtr("boot_modem PIL triggered");

    /* Log kernel-side status if available */
    {
        int fd = open("/sys/kernel/boot_modem/status", O_RDONLY);
        if (fd >= 0) {
            char msg[64];
            ssize_t n = read(fd, msg, sizeof(msg) - 1);
            close(fd);
            if (n > 0) {
                msg[n] = '\0';
                char *nl = strchr(msg, '\n');
                if (nl)
                    *nl = '\0';
                LOGI("radio", "%s %s", "modem boot", msg);
            }
        }
    }

    /* Real-ROM reference capture (MEMORY.md, this same session): modem
     * reset -> WLAN FW ready takes ~4s total on real Android, and the
     * modem's own internal bailout panics the whole SoC exactly 40.05s
     * after "Brought out of reset" if AP-side WLAN bring-up hasn't
     * happened yet (§4.5aq). This function used to block its caller here
     * for a flat 18s just to grab a late PIL dmesg snapshot for /tmp/
     * radio.log -- a live pstore capture this session showed that alone
     * pushes RF power-on (in wlan.c, after this function returns) to
     * ~38s post-reset, leaving only ~2s of margin before the 40s bailout
     * every single boot, independent of wlan.c's own settle-wait tuning.
     * Do the same dmesg snapshot in a forked child instead so it doesn't
     * eat any of that budget -- purely diagnostic, nothing downstream
     * depends on it having completed before boot_modem() returns. */
    {
        pid_t p = fork();
        if (p == 0) {
            usleep(18000000);
            run_sh("dmesg 2>/dev/null | grep -iE 'pil|PIL|subsys|modem|mpss|q6|minios_modem' >> /tmp/radio.log");
            _exit(0);
        }
    }
    plog_append("boot_modem: dmesg PIL snapshot scheduled (async, +18s, non-blocking)");
    plog_save_tmp_logs();
}


int wait_modem_online(int max_sec)
{
    char st[32] = "unknown";

    for (int i = 0; i < max_sec * 2; i++) {
        wdt_pet();
        if (read_subsys_state("subsys0", st, sizeof(st)) == 0 &&
            !strcmp(st, "ONLINE")) {
            LOGI("radio", "%s", "modem: ONLINE");
            return 1;
        }
        if (i == 0 || (i % 20) == 0)
            LOGI("radio", "%s %s", "modem wait", st);
        usleep(500000);
    }
    LOGI("radio", "%s %s", "modem wait timeout", st);
    return 0;
}


void radio_early_modem_boot(void)
{
    char st[32] = "?";

    ensure_block_layout();
    sync_mount_flags();
    if (!modem_mounted && try_mount_part("modem", "/vendor/firmware_mnt") == 0)
        modem_mounted = 1;
    link_modem_pil_firmware();
    if (read_subsys_state("subsys0", st, sizeof(st)) == 0)
        LOGI("radio", "%s %s", "early modem", st);
    if (!strcmp(st, "ONLINE")) {
        LOGI("radio", "%s", "early modem: already ONLINE");
        return;
    }
    /* Was a whitelist of "OFFLINE"/"UNKNOWN"/"?" only -- live testing this
     * session showed the real state read here is "OFFLINING" (not
     * "OFFLINE"), which the whitelist silently skipped, meaning this
     * function never actually called boot_modem() early at all; the modem
     * only got its real subsystem_get() kick from the old, much-later call
     * in cnss.c's start_cnss_stack(), at its usual ~40s point, regardless
     * of this function running at t~25s. subsystem_get() is refcounted by
     * the SSR framework, so calling it here for any non-ONLINE state is
     * safe even if some other transition happens to already be pending. */
    LOGI("radio", "%s", "early modem: boot");
    boot_modem();
}


