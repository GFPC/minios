#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/com.h"
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

/* ── UDC helpers ── */
const char *find_udc(char *buf, int len)
{
    DIR *d = opendir("/sys/class/udc");
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') {
            strncpy(buf, e->d_name, len-1); buf[len-1] = '\0';
            closedir(d); return buf;
        }
    closedir(d); return NULL;
}

void dump_udc(void)
{
    DIR *d = opendir("/sys/class/udc");
    if (!d) { klog("USB: /sys/class/udc MISSING"); return; }
    struct dirent *e; int n = 0;
    while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') {
        klogf("USB: udc[%d]=%s", n++, e->d_name);
    }
    closedir(d);
    if (!n) klog("USB: /sys/class/udc is EMPTY");
}

/* Disable runtime-PM autosuspend on every usb/dwc3/ssusb platform device under
 * /sys/devices/platform/soc, one level deep. Device-tree node names/addresses
 * vary across kernel trees (the previously hardcoded "4e00000.dwc3/dwc3-msm.0"
 * and "a600000.*" paths never matched this device's actual DT layout —
 * ssusb@4e00000/dwc3@4e00000 — so the writes silently no-op'd via sysfs_write
 * and autosuspend stayed enabled, causing the classic 10s "USB device not
 * recognized" PHY power-toggle failure). Scanning avoids hardcoding guesses.
 */
static void usb_disable_autosuspend_at(const char *base)
{
    char p[224];

    snprintf(p, sizeof(p), "%s/power/autosuspend_delay_ms", base);
    sysfs_write(p, "-1");
    snprintf(p, sizeof(p), "%s/power/control", base);
    sysfs_write(p, "on");
    klogf("USB: disabled autosuspend on %s", base);
}

void usb_force_peripheral(void)
{
    /* Various paths tried by different kernel/HAL versions */
    const char *paths[] = {
        "/sys/devices/platform/soc/4e00000.dwc3/role",
        "/sys/devices/platform/soc/4e00000.ssusb/role",
        "/sys/class/dual_role_usb/dual_role_usb0/mode",
        "/sys/class/typec/port0/data_role",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], W_OK) == 0) {
            sysfs_write(paths[i], "peripheral");
            klogf("USB: wrote peripheral to %s", paths[i]);
        }
    }

    const char *soc = "/sys/devices/platform/soc";
    DIR *d = opendir(soc);
    if (!d) {
        klog("USB: /sys/devices/platform/soc missing (autosuspend fix skipped)");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (!strstr(e->d_name, "usb") && !strstr(e->d_name, "dwc3") && !strstr(e->d_name, "ssusb"))
            continue;
        char base[192];
        snprintf(base, sizeof(base), "%s/%s", soc, e->d_name);
        usb_disable_autosuspend_at(base);

        DIR *d2 = opendir(base);
        if (!d2)
            continue;
        struct dirent *e2;
        while ((e2 = readdir(d2)) != NULL) {
            if (e2->d_name[0] == '.')
                continue;
            if (!strstr(e2->d_name, "dwc3"))
                continue;
            char child[224];
            snprintf(child, sizeof(child), "%s/%s", base, e2->d_name);
            usb_disable_autosuspend_at(child);
        }
        closedir(d2);
    }
    closedir(d);
}
/* Unique per-boot serial number. Windows keys its PnP device-instance cache
 * off the USB serial-number string; a fixed "minios00" means a host that
 * once failed to enumerate this device (e.g. cached Error state) reuses that
 * same poisoned instance forever, even across reboots/reflashes. Randomizing
 * it makes each boot look like a brand-new device to the host. */
const char *usb_gen_serial(char *buf, size_t len)
{
    unsigned int r = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &r, sizeof(r)) != sizeof(r))
            r = 0;
        close(fd);
    }
    if (!r)
        r = (unsigned int)getpid() ^ (unsigned int)time(NULL);
    snprintf(buf, len, "minios%06x", r & 0xffffffu);
    return buf;
}

void usb_gadget_reset(void)
{
    sysfs_write(USB_G "/UDC", "");
    usleep(300000);
}

int usb_link_function(const char *func_name)
{
    char fn[128], ln[128];

    snprintf(fn, sizeof(fn), USB_G "/functions/%s", func_name);
    snprintf(ln, sizeof(ln), USB_G "/configs/c.1/%s", func_name);
    if (symlink(fn, ln) != 0)
        return -1;
    klogf("USB: linked %s", func_name);
    return 0;
}

int usb_add_ncm(void)
{
    const char *names[] = {
        "ncm.0", "ncm.usb0", "rndis.rndis", "rndis_bam.rndis", "gsi.rndis", NULL
    };

    for (int i = 0; names[i]; i++) {
        char fn[128], addr_path[160];

        snprintf(fn, sizeof(fn), USB_G "/functions/%s", names[i]);
        if (mkdir(fn, 0755) != 0 && errno != EEXIST)
            continue;
        snprintf(addr_path, sizeof(addr_path), "%s/dev_addr", fn);
        if (access(addr_path, F_OK) == 0)
            sysfs_write(addr_path, "42:00:00:00:00:01");
        snprintf(addr_path, sizeof(addr_path), "%s/host_addr", fn);
        if (access(addr_path, F_OK) == 0)
            sysfs_write(addr_path, "42:00:00:00:00:02");
        if (usb_link_function(names[i]) == 0) {
            usb_ncm_active = 1;
            if (!strncmp(names[i], "ncm", 3))
                snprintf(usb_net_if, sizeof(usb_net_if), "ncm0");
            else
                snprintf(usb_net_if, sizeof(usb_net_if), "usb0");
            return 0;
        }
        rmdir(fn);
    }
    klog("USB: no NCM/RNDIS function");
    return -1;
}

int usb_enable_ncm(void)
{
    if (usb_ncm_active)
        return 0;

    usb_udc_load();
    if (!usb_udc_name[0])
        return -1;

    klog("USB: enabling NCM for adb-tcp");
    sysfs_write(USB_G "/UDC", "");
    usleep(500000);
    if (usb_add_ncm() != 0) {
        sysfs_write(USB_G "/UDC", usb_udc_name);
        return -1;
    }
    sysfs_write(USB_G "/configs/c.1/strings/0x409/configuration", "ACM+NCM");
    sysfs_write(USB_G "/UDC", usb_udc_name);
    usleep(500000);
    return usb_ncm_active ? 0 : -1;
}

/* Qualcomm DIAG-over-USB gadget function. Instance name MUST be "diag" —
 * matches DIAG_LEGACY ("diag" in include/linux/usb/usbdiag.h), the channel
 * name the diag char driver's diag_usb.c registers via usb_diag_open().
 * Once linked+bound, the kernel's existing /dev/diag transport auto-routes
 * over USB with no MiniOS-side daemon: lets a real desktop DIAG client
 * (which does the full session/control-channel handshake our own minimal
 * ioctl tool can't) capture modem F3 logs directly. Do not run our own
 * MEMORY_DEVICE_MODE /dev/diag reader at the same time — the driver only
 * supports one active logging mode. */
int usb_add_diag(void)
{
    const char *fn = USB_G "/functions/diag.diag";

    if (mkdir(fn, 0755) != 0 && errno != EEXIST) {
        klogf("USB: diag.diag mkdir errno=%d", errno);
        return -1;
    }
    if (usb_link_function("diag.diag") != 0) {
        klogf("USB: diag.diag link errno=%d", errno);
        return -1;
    }
    klog("USB: diag function linked");
    return 0;
}

int usb_add_cser(void)
{
    /* Lineage willow: CONFIG_USB_CONFIGFS_ACM is off — Qualcomm cser only */
    const char *cser[] = { "cser.dun.3", "cser.dun.0", "cser.dun.1", NULL };

    for (int i = 0; cser[i]; i++) {
        char fn[128], ln[128], dev[32];
        int port = cser[i][strlen(cser[i]) - 1] - '0';
        snprintf(fn, sizeof(fn), USB_G "/functions/%s", cser[i]);
        if (mkdir(fn, 0755) != 0)
            continue;
        snprintf(ln, sizeof(ln), USB_G "/configs/c.1/%s", cser[i]);
        symlink(fn, ln);
        snprintf(dev, sizeof(dev), "/dev/at_usb%d", port);
        snprintf(com_dev_path, sizeof(com_dev_path), "%s", dev);
        klogf("USB: cser %s (%s)", cser[i], dev);
        return 0;
    }

    klog("USB: no cser function");
    return -1;
}
int usb_open_com_tty(void)
{
    klogf("COM: waiting for %s", com_dev_path);
    for (int pass = 0; pass < 600; pass++) {
        wdt_pet();
        devnodes_ensure_cser();
        if (access(com_dev_path, F_OK) == 0)
            break;
        usleep(100000);
    }
    if (access(com_dev_path, F_OK) != 0) {
        klogf("COM: %s missing", com_dev_path);
        return -1;
    }

    klogf("COM: opening %s (blocks until host connects)", com_dev_path);
    wdt_pet();
    int fd = open(com_dev_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        klogf("COM: open errno=%d", errno);
    else
        klogf("COM: opened %s", com_dev_path);
    return fd;
}

/* Write UDC name to bind the gadget, verify it took, then do a deliberate
 * unbind+rebind "kick". This second, unraced attempt is what actually makes
 * enumeration reliable on Windows hosts, which fail "device descriptor
 * request" outright (no retry) if the very first enumeration races a
 * controller/PHY that isn't fully settled yet — unlike Linux hosts, which
 * just retry on their own. Any caller that binds the UDC (cold boot via
 * usb_bind_udc(), or a live COM->ADB switch via usb_adb_async()) needs this
 * same robustness, not just the boot path — a leaner, unkicked single-write
 * bind is exactly what let the runtime ADB switch drop off the bus entirely
 * with no Windows connection sound at all (confirmed live, twice, same
 * night this was written). */
static int usb_udc_bind_verify_kick(const char *name)
{
    sysfs_write(USB_G "/UDC", name);
    usleep(800000);
    char path[128], rd[64];
    int n;
    snprintf(path, sizeof(path), USB_G "/UDC");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = read(fd, rd, sizeof(rd) - 1);
        close(fd);
        if (n > 1) {
            sysfs_write(USB_G "/UDC", "");
            usleep(400000);
            sysfs_write(USB_G "/UDC", name);
            usleep(800000);
            return 0;
        }
    }
    klog("USB: bind verify failed");
    sysfs_write(USB_G "/UDC", "");
    return -1;
}

int usb_bind_udc(void)
{
    klog("USB: waiting for UDC (60 s)...");
    for (int i = 0; i < 600; i++) {
        wdt_pet();
        if (find_udc(usb_udc_name, sizeof(usb_udc_name))) {
            klogf("USB: UDC=%s found at t=%ds", usb_udc_name, i / 10);
            /* Let clocks/PHY settle before the first pull-up (see
             * usb_udc_bind_verify_kick()'s own comment for why). */
            wdt_pet();
            usleep(1500000);
            if (usb_udc_bind_verify_kick(usb_udc_name) == 0)
                return 0;
            klog("USB: bind verify failed, retry");
            usb_udc_name[0] = '\0';
            usleep(500000);
        }
        if (i % 100 == 99)
            dump_udc();
        usleep(100000);
    }
    dump_udc();
    klog("USB: FAILED — no UDC in 60 s");
    return -1;
}

void usb_unlink_cser(void)
{
    char dir[128], path[256];
    DIR *d;

    snprintf(dir, sizeof(dir), USB_G "/configs/c.1");
    d = opendir(dir);
    if (!d)
        return;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (strncmp(e->d_name, "cser.", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
        klogf("USB: unlinked %s", e->d_name);
    }
    closedir(d);
}

void usb_stop_adb_daemons(void)
{
    if (ffs_adb_pid > 0) {
        kill(ffs_adb_pid, SIGTERM);
        ffs_adb_pid = -1;
        usleep(300000);
    }
    if (adb_pid_alive() > 0) {
        kill(adb_pid_alive(), SIGTERM);
        adbd_pid = -1;
        unlink("/tmp/adbd.pid");
        usleep(300000);
    }
}

int usb_prepare_adb(void)
{
    if (access("/sbin/adbd", X_OK) != 0)
        return 0;

    usb_stop_adb_daemons();
    adb_env_prepare();

    sysfs_mkdir("/dev/socket");
    chmod("/dev/socket", 0777);
    unlink("/tmp/ffs_adb.log");
    unlink("/tmp/adbd.log");

    if (mkdir(USB_G "/functions/ffs.adb", 0755) != 0 && errno != EEXIST) {
        klogf("adb: ffs.adb mkdir errno=%d", errno);
        return 0;
    }
    if (usb_mount_ffs_adb() != 0)
        return 0;

    if (adb_start_daemon() <= 0)
        return 0;
    /* Don't just hope adbd has opened ep0/written its descriptors by a fixed
     * deadline — actively wait for the kernel to have actually created ep1
     * (which only happens after that) before linking ffs.adb into the
     * active config. A blind sleep here raced adbd on a loaded system. */
    wait_ffs_ep1(5);

    if (usb_link_ffs_adb() != 0) {
        usb_stop_adb_daemons();
        return 0;
    }

    sysfs_write(USB_G "/configs/c.1/strings/0x409/configuration", "adb");
    sysfs_write(USB_G "/idVendor", "0x18d1");
    sysfs_write(USB_G "/idProduct", "0x4ee7");
    klog("adb: adbd+ffs linked (bind UDC next)");
    return 1;
}

int boot_com_requested(void)
{
    char buf[512];
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    /* NOTE: usb_setup_adb_only() (ffs.adb/FunctionFS path) has an independent
     * boot-hang bug unrelated to the SD-card mount fix — confirmed by
     * reproducing the exact same "stuck on splash, no USB ever" symptom
     * again after forcing ADB here, on a card that was already repaired.
     * Until that's root-caused, stay on the COM (cser) path, which does
     * complete enumeration correctly (Windows Code 28 "no driver" is a
     * separate, easy, host-side fix — not a boot hang). */
    if (strstr(buf, "minios.usb=com"))
        return 1;
    /* ACM initramfs builds always add minios=1 — prefer COM+TCP adb over USB adb-only. */
    if (strstr(buf, "minios=1"))
        return 1;
    return 0;
}

int usb_adb_postbind(void)
{
    wait_ffs_ep1(15);
    if (wait_adbd_handoff(5) != 0 && adb_pid_alive() <= 0) {
        klog("adb: postbind handoff failed");
        return -1;
    }
    klog("adb: USB adbd ready");
    return 0;
}

void usb_adb_postbind_async(void)
{
    pid_t p = fork();
    if (p != 0)
        return;
    if (usb_adb_postbind() != 0)
        klog("adb: postbind async failed");
    _exit(0);
}

int usb_setup_adb_only(void)
{
    klog("USB: ADB-only boot");
    usb_gadget_reset();
    sysfs_mkdir(USB_G);
    sysfs_mkdir(USB_G "/strings/0x409");
    sysfs_mkdir(USB_G "/configs/c.1");
    sysfs_mkdir(USB_G "/configs/c.1/strings/0x409");

    adb_env_prepare();

    sysfs_write(USB_G "/idVendor",  "0x18d1");
    sysfs_write(USB_G "/idProduct", "0x4ee7");
    sysfs_write(USB_G "/strings/0x409/manufacturer",           "MiniOS");
    sysfs_write(USB_G "/strings/0x409/product",                "Redmi Note 8T Demo");
    { char ser[32]; sysfs_write(USB_G "/strings/0x409/serialnumber", usb_gen_serial(ser, sizeof(ser))); }
    sysfs_write(USB_G "/configs/c.1/strings/0x409/configuration", "adb");
    sysfs_write(USB_G "/configs/c.1/MaxPower", "500");

    if (!usb_prepare_adb())
        return -1;

    /* usb_add_diag() disabled for now — confirmed via Windows Device Manager
     * (ConfigManagerErrorCode 10, "device cannot start") that adding the
     * diag.diag function breaks usbser.sys's binding to the cser interface
     * on this host, even though it bound fine on many boots before this
     * function existed. Re-enable only once that's root-caused (MEMORY.md
     * §4.5i/§4.5j) — not needed for the current pd-mapper/wifi test. */
    /* usb_add_diag(); */

    usb_force_peripheral();
    if (usb_bind_udc() != 0)
        return -1;

    usb_adb_postbind_async();

    usb_com_active = 0;
    sysfs_write("/tmp/adb.active", "1");
    klog("USB: BOUND ADB (adbd + ffs.adb)");
    return 0;
}

int usb_setup_com_only(void)
{
    klog("USB: COM-only boot");
    usb_gadget_reset();
    sysfs_mkdir(USB_G);
    sysfs_mkdir(USB_G "/strings/0x409");
    sysfs_mkdir(USB_G "/configs/c.1");
    sysfs_mkdir(USB_G "/configs/c.1/strings/0x409");

    if (usb_add_cser() != 0)
        return -1;

    /* Re-enabled for the DIAG-over-USB capture (MEMORY.md §4.5i/§4.5m) —
     * adding this function breaks Windows' usbser.sys binding to the cser
     * interface (ConfigManagerErrorCode 10), so this test plan moves the
     * whole USB device into WSL via usbipd instead of relying on the
     * Windows COM driver at all. */
    usb_add_diag();

    /* NCM deferred — composite ACM+NCM can glitch dwc3 on idle WSL/usbipd hosts.
     * Use COM command `adb-tcp` to bring up NCM + TCP adbd on demand. */

    devnodes_ensure_cser();

    sysfs_write(USB_G "/idVendor",  "0x18d1");
    sysfs_write(USB_G "/idProduct", "0xd001");
    sysfs_write(USB_G "/strings/0x409/manufacturer",           "MiniOS");
    sysfs_write(USB_G "/strings/0x409/product",                "Redmi Note 8T Demo");
    { char ser[32]; sysfs_write(USB_G "/strings/0x409/serialnumber", usb_gen_serial(ser, sizeof(ser))); }
    sysfs_write(USB_G "/configs/c.1/strings/0x409/configuration", "ACM");
    sysfs_write(USB_G "/configs/c.1/MaxPower", "500");

    usb_force_peripheral();

    if (usb_bind_udc() != 0)
        return -1;

    usb_com_active = 1;
    klog("USB: BOUND COM");
    return 0;
}

void usb_config_clear_links(void)
{
    char path[256];
    DIR *d = opendir(USB_G "/configs/c.1");

    if (!d)
        return;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        if (!strcmp(e->d_name, "strings") || !strcmp(e->d_name, "bmAttributes") ||
            !strcmp(e->d_name, "MaxPower"))
            continue;
        snprintf(path, sizeof(path), USB_G "/configs/c.1/%s", e->d_name);
        unlink(path);
    }
    closedir(d);
}

void usb_restore_com_only(void)
{
    usb_udc_load();
    if (usb_udc_name[0] == '\0')
        return;

    unlink("/tmp/adb.active");

    if (ffs_adb_pid > 0) {
        kill(ffs_adb_pid, SIGTERM);
        ffs_adb_pid = -1;
    }
    if (adb_pid_alive() > 0) {
        kill(adb_pid_alive(), SIGTERM);
        adbd_pid = -1;
        unlink("/tmp/adbd.pid");
    }

    sysfs_write(USB_G "/UDC", "");
    usleep(800000);
    usb_config_clear_links();

    if (usb_setup_com_only() != 0)
        klog("USB: COM restore failed");
    else
        usb_start_com_shell();
}

void usb_udc_load(void)
{
    char path[128], buf[64];
    int fd, n;

    if (usb_udc_name[0])
        return;
    snprintf(path, sizeof(path), USB_G "/UDC");
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    for (char *p = buf; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = '\0';
            break;
        }
    }
    if (buf[0])
        snprintf(usb_udc_name, sizeof(usb_udc_name), "%s", buf);
}

void usb_adb_async(void)
{
    usb_udc_load();
    if (access("/sbin/adbd", X_OK) != 0 || usb_udc_name[0] == '\0')
        return;

    klog("adb: switch to ADB-only");
    sysfs_write(USB_G "/UDC", "");
    usleep(1000000);
    usb_config_clear_links();
    usb_unlink_cser();

    adb_env_prepare();

    if (!usb_prepare_adb()) {
        klog("adb: prepare failed");
        usb_restore_com_only();
        return;
    }

    /* usb_bind_udc() (cold-boot path) has this same 1.5s settle delay
     * right before its first UDC write — "let clocks/PHY settle" — but it
     * was never carried over here when usb_udc_bind_verify_kick() was
     * extracted as a shared helper (§4.5am). A live COM->ADB switch leaves
     * the controller unbound for a while during usb_prepare_adb() (mounts
     * FunctionFS, starts adbd, can take up to the full adbd-handoff
     * timeout), then goes straight into the bind/verify/kick sequence with
     * no settle pause at all — real, reproducible symptom (confirmed twice
     * via direct SD-card boot.log reads, §4.5ap/this section): "dwc3
     * ...dwc3: request ... was not queued to ep0out" immediately after
     * re-enumeration starts, then an unbounded "read descriptors"/"read
     * strings" host-retry loop that never resolves. Matching the cold-boot
     * path's own settle delay here is the cheapest, most direct fix to try
     * first. */
    usleep(1500000);
    if (usb_udc_bind_verify_kick(usb_udc_name) != 0) {
        klog("adb: udc bind failed");
        usb_restore_com_only();
        return;
    }
    if (usb_adb_postbind() != 0) {
        klog("adb: postbind failed");
        usb_restore_com_only();
        return;
    }

    usb_com_active = 0;
    sysfs_write("/tmp/adb.active", "1");
    klog("adb: ADB-only bound (replug USB, adb devices)");
}

void usb_start_com_shell(void)
{
    pid_t com_pid = fork();

    if (com_pid != 0)
        return;
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

int usb_setup(void)
{
    if (boot_com_requested()) {
        if (usb_setup_com_only() != 0)
            return -1;
        klog("USB: COM-only boot (cmdline minios.usb=com, no NCM/adbd until adb-tcp)");
        return 0;
    }

    if (usb_setup_adb_only() == 0) {
        klog("USB: ADB-only boot (18d1:4ee7)");
        return 0;
    }

    klog("USB: ADB-only failed, COM fallback");
    if (usb_setup_com_only() != 0)
        return -1;
    return 0;
}
