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
