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

/* Try to force USB peripheral mode on Qualcomm SM6125 */
void usb_force_peripheral(void)
{
    /* Various paths tried by different kernel/HAL versions */
    const char *paths[] = {
        "/sys/devices/platform/soc/4e00000.dwc3/role",
        "/sys/devices/platform/soc/a600000.ssusb/role",
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
    /* extcon style */
    sysfs_write("/sys/devices/platform/soc/4e00000.dwc3/dwc3-msm.0/power/autosuspend_delay_ms", "0");
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

int usb_bind_udc(void)
{
    klog("USB: waiting for UDC (60 s)...");
    for (int i = 0; i < 600; i++) {
        wdt_pet();
        if (find_udc(usb_udc_name, sizeof(usb_udc_name))) {
            klogf("USB: UDC=%s found at t=%ds", usb_udc_name, i / 10);
            sysfs_write(USB_G "/UDC", usb_udc_name);
            usleep(800000);
            char path[128], rd[64];
            int n;
            snprintf(path, sizeof(path), USB_G "/UDC");
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                n = read(fd, rd, sizeof(rd) - 1);
                close(fd);
                if (n > 1)
                    return 0;
            }
            klog("USB: bind verify failed, retry");
            sysfs_write(USB_G "/UDC", "");
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
    usleep(500000);

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
    return strstr(buf, "minios.usb=com") != NULL;
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
    sysfs_write(USB_G "/strings/0x409/serialnumber",           "minios00");
    sysfs_write(USB_G "/configs/c.1/strings/0x409/configuration", "adb");
    sysfs_write(USB_G "/configs/c.1/MaxPower", "500");

    if (!usb_prepare_adb())
        return -1;

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

    usb_add_ncm();

    devnodes_ensure_cser();

    sysfs_write(USB_G "/idVendor",  "0x18d1");
    sysfs_write(USB_G "/idProduct", "0xd001");
    sysfs_write(USB_G "/strings/0x409/manufacturer",           "MiniOS");
    sysfs_write(USB_G "/strings/0x409/product",                "Redmi Note 8T Demo");
    sysfs_write(USB_G "/strings/0x409/serialnumber",           "minios00");
    sysfs_write(USB_G "/configs/c.1/strings/0x409/configuration",
       usb_ncm_active ? "ACM+NCM" : "ACM");
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

    sysfs_write(USB_G "/UDC", usb_udc_name);
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
        usb_tcp_adb_async();
        klog("USB: COM+NCM boot (cmdline minios.usb=com)");
        return 0;
    }

    if (usb_setup_adb_only() == 0) {
        klog("USB: ADB-only boot (18d1:4ee7)");
        return 0;
    }

    klog("USB: ADB-only failed, COM fallback");
    if (usb_setup_com_only() != 0)
        return -1;
    usb_tcp_adb_async();
    return 0;
}
