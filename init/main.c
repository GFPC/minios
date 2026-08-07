#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include "minios/devnodes.h"
#include "minios/fwload.h"
#include "minios/led.h"
#include "minios/log.h"
#include "blockdev.h"
#include "minios/radio.h"
#include "modem.h"
#include "firmware.h"
#include "minios/selinux.h"
#include "minios/sysfs.h"
#include "minios/touch.h"
#include "minios/triggers.h"
#include "minios/usb.h"
#include "minios/watchdog.h"
#include "minios/plog.h"
#include "radio_state.h"

static int cmdline_has(const char *token)
{
    char buf[2048];
    ssize_t n;
    int fd;

    if (!token || !token[0])
        return 0;
    fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return strstr(buf, token) != NULL;
}

static void mount_pstore(void)
{
    mkdir("/sys/fs/pstore", 0755);
    if (mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL) == 0)
        klog("pstore mounted");
}

/* PMI632 (qpnp-smb5) APSD (charger-type autodetect) can settle on
 * POWER_SUPPLY_TYPE_UNKNOWN and then hold USB input current at 2mA for
 * ~223s before its own internal retry finally re-detects and raises it --
 * real, hardware-verified via SD-card boot.log timestamps (dwc3 "Avail
 * curr from USB = 2" at t=17s, still 2 at t=240s, only reaching 100 at
 * t=471s). This is almost certainly a real, large chunk of this project's
 * chronic ~150s+ USB enumeration slowness. qpnp-smb5.c's "usb" power_supply
 * exposes POWER_SUPPLY_PROP_APSD_RERUN as writable (smb5_usb_prop_is_
 * writeable()), wired directly to smblib_rerun_apsd() -- a single PMIC
 * register write (CMD_APSD_REG/APSD_RERUN_BIT) that forces the *hardware*
 * to redo its own real charger-type detection immediately, instead of
 * faking a type/current value in software (this driver's "type" and plain
 * "current_max" nodes aren't even writable -- only apsd_rerun and
 * sdp_current_max are, confirmed via smb5_usb_prop_is_writeable()). Doing
 * this early, before usb_setup(), gives the real detection a chance to
 * land well before any of the ~150s-scale timeouts this project has
 * fought all along. */
static void usb_kick_apsd_rerun(void)
{
    const char *path = "/sys/class/power_supply/usb/apsd_rerun";
    int i;

    for (i = 0; i < 20; i++) {
        if (access(path, F_OK) == 0)
            break;
        usleep(50000);
    }
    if (access(path, F_OK) != 0) {
        klog("usb: apsd_rerun node not found, skipping");
        return;
    }
    sysfs_write(path, "1");
    klog("usb: kicked APSD rerun (charger-type redetect)");
}

static void disable_cpu_idle(void)
{
    sysfs_write("/sys/module/cpuidle/parameters/off", "1");
    sysfs_write("/sys/module/lpm_levels/parameters/sleep_disabled", "1");
}

static int cpu_idle_disabled;

static void disable_cpu_idle_retry(void)
{
    if (cpu_idle_disabled)
        return;
    if (access("/sys/module/lpm_levels/parameters/sleep_disabled", W_OK) != 0)
        return;
    disable_cpu_idle();
    cpu_idle_disabled = 1;
    klog("cpuidle/LPM disabled");
}

/* arch/arm64/kernel/traps.c: show_unhandled_signals defaults to 0 — no kmsg
 * line for userspace SIGSEGV unless enabled. Live path on this kernel:
 * /proc/sys/debug/exception-trace (not userprocess_debug). Turn on before
 * qrtr-ns so early boot crashes print fault addr + esr in dmesg. */
static void enable_kernel_exception_trace(void)
{
    if (access("/proc/sys/debug/exception-trace", W_OK) == 0) {
        sysfs_write("/proc/sys/debug/exception-trace", "1");
        klog("exception-trace enabled");
    }
}

static void trim_nl(char *s)
{
    char *nl;

    if (!s)
        return;
    nl = strchr(s, '\n');
    if (nl)
        *nl = '\0';
}

static void link_modem_fw_symlinks(void)
{
    DIR *d;
    struct dirent *e;
    char src[384], dst[384];

    d = opendir("/vendor/firmware_mnt/image");
    if (!d)
        return;
    mkdir("/lib/firmware", 0755);
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "modem.", 6) != 0)
            continue;
        snprintf(src, sizeof(src), "/vendor/firmware_mnt/image/%s", e->d_name);
        snprintf(dst, sizeof(dst), "/lib/firmware/%s", e->d_name);
        unlink(dst);
        symlink(src, dst);
    }
    closedir(d);
}

static int early_mount_modem_partition(void)
{
    const char *dev;

    mkdir("/vendor", 0755);
    mkdir("/vendor/firmware_mnt", 0755);
    blockdev_wait_mmc(10);
    blockdev_ensure_by_name();
    dev = blockdev_by_name("modem");
    if (!dev) {
        klog("early modem: no modem blockdev");
        return -1;
    }
    if (mount(dev, "/vendor/firmware_mnt", "vfat", MS_RDONLY, NULL) != 0) {
        klog("early modem: vfat mount fail");
        if (mount(dev, "/vendor/firmware_mnt", "ext4", MS_RDONLY, NULL) != 0) {
            klog("early modem: mount fail");
            return -1;
        }
    }
    klog("early modem: firmware_mnt mounted");
    modem_mounted = 1;
    link_modem_fw_symlinks();
    set_firmware_class_path();
    return (access("/lib/firmware/modem.mdt", R_OK) == 0 ||
            access("/vendor/firmware_mnt/image/modem.mdt", R_OK) == 0) ? 0 : -1;
}

static int early_vendor_mount(void)
{
    const char *dev;
    const char *types[] = { "ext4", "erofs", "f2fs", NULL };

    mkdir("/mnt/vendor", 0755);
    mkdir("/vendor", 0755);
    blockdev_ensure_by_name();
    dev = blockdev_by_name("vendor");
    if (!dev) {
        klog("early vendor: no blockdev");
        return -1;
    }
    for (int i = 0; types[i]; i++) {
        if (mount(dev, "/mnt/vendor", types[i], MS_RDONLY, NULL) == 0) {
            klogf("early vendor: mounted", types[i]);
            if (mount("/mnt/vendor", "/vendor", NULL, MS_BIND, NULL) != 0)
                klogf("early vendor: bind fail", strerror(errno));
            else
                klog("early vendor: OK");
            return 0;
        }
    }
    klogf("early vendor: mount fail", strerror(errno));
    return -1;
}

/* PIL modem from /lib/firmware/modem.* — before USB, while subsys still OFFLINE. */
static void early_modem_boot(void)
{
    char state[32] = {0};
    int fd;
    int waited = 0;

    fd = open("/sys/bus/msm_subsys/devices/subsys0/state", O_RDONLY);
    if (fd < 0)
        return;
    if (read(fd, state, sizeof(state) - 1) <= 0) {
        close(fd);
        return;
    }
    close(fd);
    trim_nl(state);
    klogf("early modem: state=%s", state);

    if (!strcmp(state, "ONLINE")) {
        klog("early modem: already ONLINE");
        return;
    }

    while (!strcmp(state, "OFFLINING") && waited < 8) {
        wdt_pet();
        usleep(500000);
        waited++;
        fd = open("/sys/bus/msm_subsys/devices/subsys0/state", O_RDONLY);
        if (fd < 0)
            break;
        if (read(fd, state, sizeof(state) - 1) <= 0) {
            close(fd);
            break;
        }
        close(fd);
        trim_nl(state);
        if (waited == 1 || (waited % 4) == 0)
            klogf("early modem: offlining wait %s", state);
        if (!strcmp(state, "ONLINE"))
            return;
        if (strcmp(state, "OFFLINING"))
            break;
    }

    if (!strcmp(state, "OFFLINING"))
        klogf("early modem: boot during %s", state);

    if (access("/lib/firmware/modem.mdt", R_OK) != 0) {
        if (early_mount_modem_partition() != 0)
            klog("early modem: no firmware source");
    }
    if (access("/lib/firmware/modem.mdt", R_OK) != 0) {
        klog("early modem: no /lib/firmware/modem.mdt");
        return;
    }

    fd = open("/sys/kernel/boot_adsp/boot", O_WRONLY);
    if (fd < 0) {
        klog("early modem: no boot_adsp");
        return;
    }
    if (write(fd, "1", 1) == 1)
        klog("early modem: boot_adsp=1");
    else if (write(fd, "1u", 2) == 2)
        klog("early modem: boot_adsp=1u");
    else
        klog("early modem: boot_adsp write fail");
    close(fd);

    for (int i = 0; i < 24; i++) {
        wdt_pet();
        usleep(500000);
        fd = open("/sys/bus/msm_subsys/devices/subsys0/state", O_RDONLY);
        if (fd < 0)
            break;
        if (read(fd, state, sizeof(state) - 1) <= 0) {
            close(fd);
            break;
        }
        close(fd);
        trim_nl(state);
        if (!strcmp(state, "ONLINE")) {
            klog("early modem: ONLINE");
            return;
        }
        if (i == 0 || (i % 10) == 0)
            klogf("early modem: PIL wait %s", state);
    }
    klogf("early modem: done %s", state);
}

static void save_kmsg_snapshot(void)
{
    int out = open("/tmp/km.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char buf[131072];
    ssize_t n;

    if (out < 0)
        return;
    n = klogctl(3, buf, sizeof(buf) - 1);
    if (n > 0) {
        write(out, buf, (size_t)n);
        klogf("km.txt saved %zd", n);
    } else {
        int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        size_t total = 0;
        if (kfd >= 0) {
            while (total < sizeof(buf) - 1000) {
                ssize_t r = read(kfd, buf + total, 1000);
                if (r <= 0)
                    break;
                total += (size_t)r;
            }
            close(kfd);
            if (total > 0)
                write(out, buf, total);
            klogf("km.txt saved %zu", total);
        }
    }
    close(out);
}

/* devtmpfs only auto-populates /dev for devices registered *after* our own
 * mount("devtmpfs", "/dev", ...) call — anything the kernel's built-in
 * drivers already registered before MiniOS's /init even started running
 * (e.g. UIO devices, which probe very early: msm_sharedmem/qcom,rmtfs_sharedmem
 * at ~t=1s) never gets a node created. Confirmed via direct comparison this
 * session (MEMORY.md §4.5g): /sys/class/uio/uio0/dev existed with the driver
 * fully probed, but /dev/uio0 was simply never created. rmt_storage (see
 * boot_modem()'s start_rmt_storage()) needs /dev/uio0 to mmap its shared
 * memory region with the modem and fails with "Mmap Failed" without it.
 * Manually backfill any /sys/class/uio/uioN nodes missing from /dev. */
static void populate_uio_devnodes(void)
{
    DIR *d = opendir("/sys/class/uio");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "uio", 3) != 0)
            continue;
        char devpath[160], nodepath[32];
        snprintf(devpath, sizeof(devpath), "/sys/class/uio/%s/dev", e->d_name);
        int fd = open(devpath, O_RDONLY);
        if (fd < 0)
            continue;
        char buf[32];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        unsigned major, minor;
        if (sscanf(buf, "%u:%u", &major, &minor) != 2)
            continue;
        snprintf(nodepath, sizeof(nodepath), "/dev/%s", e->d_name);
        if (access(nodepath, F_OK) == 0)
            continue;
        if (mknod(nodepath, S_IFCHR | 0660, makedev(major, minor)) == 0)
            klogf2("uio devnode", nodepath);
    }
    closedir(d);
}

int main(void)
{
    sysfs_mkdir("/dev"); sysfs_mkdir("/proc"); sysfs_mkdir("/sys"); sysfs_mkdir("/config"); sysfs_mkdir("/tmp");

    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mknod("/dev/console", S_IFCHR|0600, makedev(5,1));
    mknod("/dev/null",    S_IFCHR|0666, makedev(1,3));
    mknod("/dev/zero",    S_IFCHR|0666, makedev(1,5));
    mknod("/dev/kmsg",    S_IFCHR|0620, makedev(1,11));
    /* mknod()'s mode is a no-op if devtmpfs already auto-created the node
     * (same class of race as /dev/zero/uio* elsewhere in this function) —
     * chmod/chown explicitly so this applies regardless. root:system 0620
     * matches real ueventd.rc; without it, any daemon that drops to
     * uid=1000/gid=1000 ("system", e.g. cnss-daemon) gets EACCES trying to
     * write /dev/kmsg (confirmed via cnss.exec.log: "sh: can't create
     * /dev/kmsg: Permission denied"). */
    chmod("/dev/kmsg", 0620);
    chown("/dev/kmsg", 0, 1000);
    /* Same devtmpfs-race bug, this time on /dev/null: confirmed live via
     * `stat /dev/null` showing mode=644 despite the 0666 requested above —
     * devtmpfs auto-created it first with a restrictive default, so the
     * mknod() mode above was a no-op. 644's "other" bits are read-only, so
     * any daemon dropped to a non-root/non-group-0 uid (qrtr-ns=2906,
     * pd-mapper=1000) gets EACCES calling open("/dev/null", O_RDWR) —
     * bionic's own __libc_init_AT_SECURE() does exactly that early in libc
     * init (before any of the daemon's own code runs), and when the
     * /sys/fs/selinux/null fallback also fails (no real SELinux here), it
     * calls __early_abort() -- a deliberate SIGSEGV at address == source
     * line number, which is the "translation fault at 0x000000a0" crash
     * confirmed via dmesg with exception-trace enabled. cnss-daemon starts
     * much later in boot and never hit this, which is why only the two
     * early daemons (qrtr-ns, pd-mapper) ever crashed this way. */
    chmod("/dev/null", 0666);
    chmod("/dev/zero", 0666);
    mknod("/dev/urandom", S_IFCHR|0666, makedev(1,9));

    klog("=== MINIOS START ===");
    if (cmdline_has("minios.skip_modem=1"))
        klog("boot: ISOLATE skip_modem=1");
    if (cmdline_has("minios.skip_ko=1"))
        klog("boot: ISOLATE skip_ko=1");
    if (cmdline_has("minios.skip_usb=1"))
        klog("boot: ISOLATE skip_usb=1");
    setenv("PATH", "/bin:/sbin", 1);

    mount("proc",     "/proc",   "proc",    0, NULL);
    mount("sysfs",    "/sys",    "sysfs",   0, NULL);
    for (int i = 0; i < 10; i++) {
        if (access("/sys/kernel", F_OK) == 0)
            break;
        usleep(100000);
    }
    if (access("/sys/kernel", F_OK) == 0) {
        if (access("/sys/kernel/debug", F_OK) != 0)
            mkdir("/sys/kernel/debug", 0755);
        if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) == 0)
            klog("debugfs OK");
        else
            klog("debugfs mount fail");
    }
    klog("proc/sysfs OK");

    /* Diagnostic: kprobe QRTR RX/TX packet snoop (MEMORY.md §4.5ac), loaded as
     * early as physically possible — before any vendor mount, qrtr-ns, or
     * modem PIL — so it's active for the earliest QRTR control-plane traffic
     * too. Best-effort only, must never block boot if the .ko is missing or
     * fails to load (e.g. running on a kernel with mismatched vermagic). */
    try_load_qrtr_snoop();

    populate_uio_devnodes();

    /* Diagnostic-only: ftrace boot-time capture (regulator/gpio/msm_pil_event
     * via cmdline trace_event=), mirroring the real-ROM reference timeline
     * captured this session (MEMORY.md §4.5e). tracing_on gets reset to 0 by
     * something ~3-4s into boot on the real ROM; keep re-asserting it here
     * so the ring buffer keeps recording through the wlfw window. Opt-in via
     * cmdline so normal builds are unaffected. */
    if (cmdline_has("minios.ftrace_keepalive=1")) {
        if (access("/sys/kernel/tracing", F_OK) != 0)
            mkdir("/sys/kernel/tracing", 0755);
        mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, NULL);
        pid_t tfd_pid = fork();
        if (tfd_pid == 0) {
            for (int i = 0; i < 2400; i++) {
                int fd = open("/sys/kernel/tracing/tracing_on", O_WRONLY);
                if (fd >= 0) {
                    write(fd, "1", 1);
                    close(fd);
                }
                usleep(50000);
            }
            _exit(0);
        }
    }

    /* Diagnostic (MEMORY.md §4.5b8): kprobe on fat_search_long() (the vfat
     * driver's directory-entry search, called from vfat_lookup() on every
     * fresh -- not dentry-cache-hit -- lookup of a name inside a vfat dir)
     * via ftrace's kprobe_events interface (CONFIG_FUNCTION_TRACER is off
     * on this kernel, but CONFIG_KPROBE_EVENTS=y is on). Must be armed
     * here, before early_mount_modem_partition() (right below) does the
     * very first lookup of /vendor/firmware_mnt/image this boot -- once
     * any process resolves it successfully the dentry gets cached and
     * later lookups (including this project's own repeated COM-driven
     * `stat`/`readfile` checks all session) never call back into the fat
     * driver at all, confirmed live: reading a known-good file through COM
     * with this same kprobe armed produced zero trace entries. This is
     * the earliest point in main() where debugfs (mounted just above) is
     * available to write kprobe_events through. Best-effort: writes are
     * simply ignored if the kernel rejects the syntax (kept -- ARM64
     * needs %xN raw register refs and +0(%xN):string for pointer derefs
     * needs explicit dereference syntax, not %xN:string directly, on
     * this kernel version -- confirmed live before adding this). */
    {
        int kfd;
        static const char *kcmds[] = {
            "-:fatsl",
            "p:fatsl fat_search_long name=+0(%x1):string len=%x2",
            "-:fatsl_ret",
            "r:fatsl_ret fat_search_long ret=%x0",
            NULL
        };
        for (int i = 0; kcmds[i]; i++) {
            kfd = open("/sys/kernel/debug/tracing/kprobe_events", O_WRONLY);
            if (kfd >= 0) {
                write(kfd, kcmds[i], strlen(kcmds[i]));
                close(kfd);
            }
        }
        kfd = open("/sys/kernel/debug/tracing/events/kprobes/fatsl/enable", O_WRONLY);
        if (kfd >= 0) { write(kfd, "1", 1); close(kfd); }
        kfd = open("/sys/kernel/debug/tracing/events/kprobes/fatsl_ret/enable", O_WRONLY);
        if (kfd >= 0) { write(kfd, "1", 1); close(kfd); }
        kfd = open("/sys/kernel/debug/tracing/tracing_on", O_WRONLY);
        if (kfd >= 0) { write(kfd, "1", 1); close(kfd); }
        pid_t fatsl_keepalive_pid = fork();
        if (fatsl_keepalive_pid == 0) {
            for (int i = 0; i < 2400; i++) {
                int tfd = open("/sys/kernel/debug/tracing/tracing_on", O_WRONLY);
                if (tfd >= 0) { write(tfd, "1", 1); close(tfd); }
                usleep(50000);
            }
            _exit(0);
        }
    }

    disable_cpu_idle_retry();

    wdt_open();
    wdt_pet();
    klog("watchdog open");

    plog_init();
    mount_pstore();
    plog_save_pstore();
    /* Mount modem firmware partition early (paths only — no PIL trigger here).
     * boot_modem() runs later from the radio job once qrtr-ns/pd-mapper/rmt_storage
     * are already listening; early_modem_boot() burned the 40s modem bailout
     * timer before QMI infrastructure was up. */
    if (!cmdline_has("minios.skip_modem=1"))
        fwload_helper_start();
    early_mount_modem_partition();
    early_vendor_mount();
    if (access("/vendor/bin", F_OK) == 0)
        vendor_mounted = 1;
    radio_stage_early_bins();

    /* QMI + RMTFS must be up before modem PIL — real ROM starts these on boot,
     * well before modem reset. Do this even when skip_modem=1 (USB-safe mode)
     * so a manual `radio` command doesn't race pd-mapper vs modem tms/pdr. */
    if (vendor_mounted) {
        enable_kernel_exception_trace();
        plog_append("boot: early modem_qmi_services_start");
        modem_qmi_services_start();
        start_rmtfs_daemons_early();
        plog_append("boot: early qmi+rmtfs done");
        /* A thorough real-Lineage capture (cold boot, continuous dmesg +
         * logcat) this session shows modem PIL trigger at kernel t=11.56s
         * -- essentially right after icnss's own platform driver probes at
         * t=0.6s, NOT deferred until a full userspace stack is up -- and
         * wlfw connects only ~1.15s after "modem: Brought out of reset"
         * (t=12.246 -> t=13.397), with zero dependency on wlan_pd/PDR/
         * servreg visible anywhere in cnss-daemon's own narrated startup
         * log. radio_early_modem_boot() (modem.c) was already written to
         * do exactly this early trigger but was never actually called from
         * anywhere -- dead code. The comment that used to justify deferring
         * modem PIL this late ("early_modem_boot() burned the 40s modem
         * bailout timer before QMI infrastructure was up") is satisfied
         * right here: modem_qmi_services_start()/start_rmtfs_daemons_early()
         * just ran above, so qrtr-ns/pd-mapper/rmt_storage are already
         * listening before this trigger fires. */
        /* EXPERIMENTAL: the same real-Lineage capture shows cdsp: Brought
         * out of reset at t=11.79s, BEFORE modem's own reset at t=12.25s --
         * CDSP boots first, every time, on real hardware. boot_remote_procs()
         * (CDSP+SLPI trigger) exists in this codebase but has never been
         * called -- an old comment in cnss.c says it "stays removed —
         * booting CDSP/SLPI triggers SSR" without full vendor services, but
         * that finding predates this session's fixes (pm-service stub,
         * rmt_storage keepalive, early modem PIL trigger). Given the modem's
         * own WLAN co-processor firmware has been confirmed (via QMI wire
         * trace, this session) to complete MSA_READY but never transition
         * to FW_MEM_READY -- and CDSP/modem/WLAN share silicon and power
         * rails on this SoC -- try matching real hardware's boot order
         * directly. boot_remote_procs() already no-ops safely on any sysfs
         * node it can't write to. */
        /* link_modem_pil_firmware() now stages cdsp./adsp./slpi.-prefixed
         * firmware too (see firmware.c) -- must run BEFORE
         * boot_remote_procs() triggers the CDSP/SLPI PIL load, or that
         * first attempt fails with "Failed to locate cdsp.mdt" (confirmed
         * live) and only succeeds on a later retry, well after modem's
         * own reset instead of before it. radio_early_modem_boot() below
         * would otherwise be the first thing to call this, too late for
         * CDSP's own trigger just above it. */
        {
            /* Diagnostic build confirmed live: this reliably reads 0 files
             * here on the first try -- early_mount_modem_partition() (much
             * earlier in this same function) does its own first mount
             * attempt, but the modem blockdev is not always ready by then
             * (an established, historical pattern in this codebase --
             * radio_early_modem_boot() below retries its own mount for
             * exactly this reason). Without a real mount yet,
             * /vendor/firmware_mnt/image isn't there for this function to
             * scan at all. Retry mounting (matching radio_early_modem_boot
             * ()'s own try_mount_part() call) for up to ~2s before staging,
             * so boot_remote_procs() right after has an actual chance of
             * finding cdsp.mdt on its first attempt. */
            int pilfw_n, tries, scan_tries;

            for (tries = 0; tries < 20 && !modem_mounted; tries++) {
                if (try_mount_part("modem", "/vendor/firmware_mnt") == 0)
                    modem_mounted = 1;
                else
                    usleep(100000);
            }
            /* Confirmed live: mounted=1 on the very first check (tries=0)
             * but link_modem_pil_firmware_count() still read 0 files --
             * the mount() itself succeeding doesn't mean an opendir() right
             * after it will actually see the vfat directory's real content.
             * This exact class of bug (stale/negative dcache entry from a
             * lookup that raced the mount, resolving fine on a plain
             * retry) was already root-caused earlier this session for
             * pd-mapper's own opendir() on this same firmware_mnt path --
             * apply the same proven fix here: just retry the scan itself a
             * few times. */
            pilfw_n = link_modem_pil_firmware_count();
            for (scan_tries = 0; pilfw_n == 0 && scan_tries < 10; scan_tries++) {
                usleep(200000);
                pilfw_n = link_modem_pil_firmware_count();
            }
            {
                char pilfw_msg[96];
                snprintf(pilfw_msg, sizeof(pilfw_msg),
                         "pil fw staged: %d files (mounted=%d tries=%d scan_tries=%d)",
                         pilfw_n, modem_mounted, tries, scan_tries);
                LOGI("radio", "%s", pilfw_msg);
            }
        }
        boot_remote_procs();
        radio_early_modem_boot();
    }

    radio_modem_recover_stuck();

    klog("early modem: settle 5s");
    for (int i = 0; i < 5; i++) {
        wdt_pet();
        sleep(1);
    }
    save_kmsg_snapshot();

    if (!cmdline_has("minios.skip_ko=1"))
        radio_load_modem_ko();
    else
        klog("boot: skip ko load (minios.skip_ko=1)");
    wdt_pet();

    if (!cmdline_has("minios.skip_modem=1")) {
        klog("boot: trigger auto radio init");
        plog_append("boot: radio_init_async (wifi bringup)");
        radio_init_async();
    } else {
        klog("boot: skip auto radio (minios.skip_modem=1)");
        plog_append("boot: skip auto radio — use COM `radio` to trigger");
    }

    /* immediate haptic ping — often works before LED drivers */
    vib_pulse(300);
    klog("vib pulse");

    mount("configfs", "/config", "configfs", 0, NULL);
    selinux_prepare();

    /* USB first — display DRM must not block or panic before gadget is up */
    led_prepare();
    vib_pulse(100);

    if (!cmdline_has("minios.skip_usb=1"))
        usb_kick_apsd_rerun();

    int usb_ok = 0;
    if (!cmdline_has("minios.skip_usb=1")) {
        usb_ok = (usb_setup() == 0);
        wdt_pet();
        klog(usb_ok ? "USB up" : "USB fail");
        if (usb_ok)
            led_blink(2);
    } else {
        klog("boot: skip USB (minios.skip_usb=1)");
    }

    /* Display disabled during boot-loop debug — kms_paint can panic DRM ~30s */
    klog("display: skip async (boot stable mode)");

    /* Radio only on user request — wlan power-on can glitch USB/COM. */

    if (usb_com_active && !cmdline_has("minios.skip_usb=1")) {
        pid_t com_pid = fork();
        if (com_pid == 0) {
            int tty_fd = usb_open_com_tty();
            if (tty_fd >= 0) {
                klog("COM shell start");
                setsid();
                dup2(tty_fd, 0);
                dup2(tty_fd, 1);
                dup2(tty_fd, 2);
                if (tty_fd > 2)
                    close(tty_fd);
                com_shell(0);
            }
            klog("COM child exit");
            _exit(1);
        }
    }

    klog("main: watchdog loop");
    for (;;) {
        wdt_pet();
        disable_cpu_idle_retry();
        plog_poll();
        /* Diagnostic (MEMORY.md §4.5b8 addendum): every forked descendant
         * of start_job() (radio.c) -- both plain fork() children and
         * exec_via_linker64()-spawned vendor daemons alike -- gets a real,
         * reproducible ENOENT resolving /vendor/firmware_mnt/image, while
         * main.c's own earlier-forked com_pid child (COM shell) always
         * sees it fine. Logging this exact check from the ORIGINAL,
         * never-(yet-)forked watchdog loop itself, on a slow interval,
         * tests directly whether THIS process ever loses the same
         * visibility, and if so, whether that lines up with when
         * radio_poll() first actually fires a job below. */
        {
            static int wd_diag_n;
            if ((wd_diag_n++ % 20) == 0) {
                char wdmsg[64];
                snprintf(wdmsg, sizeof(wdmsg), "wd-diag: fwmnt/image exists=%d",
                         path_exists("/vendor/firmware_mnt/image/wlanmdsp.mbn"));
                klog(wdmsg);
            }
        }
        /* rmt_storage (RMTFS server) has been observed dying silently after
         * a healthy startup (confirmed live: initializes fully -- "Done with
         * init now waiting for messages!" -- then later vanishes with no
         * crash/exit log at all). The modem polls RMTFS on its own schedule
         * for whatever EFS/NV/firmware files it needs (including
         * wlanmdsp.mbn, fetched only once the modem's internal wlan_pd
         * bring-up actually starts, which can be tens of seconds after
         * rmt_storage's first start) -- if the server is dead by the time
         * that later request happens, it fails silently and WLAN firmware
         * never loads, with no crash on either side to signal it. Re-check
         * every ~10s for the life of the boot; start_rmtfs_daemons_early()
         * is already a no-op via proc_running() whenever it's alive. */
        {
            static int rmtfs_keepalive_n;
            if ((rmtfs_keepalive_n++ % 10) == 0)
                start_rmtfs_daemons_early();
        }
        radio_poll();
        if (!cmdline_has("minios.skip_usb=1"))
            usb_com_maintain();
        triggers_poll();
        if (usb_com_active && access("/tmp/adb.on", F_OK) == 0) {
            unlink("/tmp/adb.on");
            klog("switch: -> ADB");
            pid_t job = fork();
            if (job == 0) {
                usb_adb_async();
                _exit(0);
            }
        }
        if (!usb_com_active && access("/tmp/com.on", F_OK) == 0) {
            unlink("/tmp/com.on");
            klog("switch: UI -> COM");
            usb_restore_com_only();
        }
        sleep(1);
    }
}
