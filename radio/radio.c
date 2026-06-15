#define _GNU_SOURCE
#include "radio.h"
#include "blockdev.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char wifi_st[80] = "WiFi: idle";
static char bt_st[80] = "BT: idle";
static int vendor_mounted;
static int persist_mounted;
static int modem_mounted;
static int bt_fw_mounted;
static pid_t radio_pid;
static pid_t scan_pid;

static void klog(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[192];
        int n = snprintf(b, sizeof(b), "<6>radio: %s\n", s);
        if (n > 0)
            (void)write(fd, b, n);
        close(fd);
    }
}

static void klogf2(const char *a, const char *b)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s %s", a, b);
    klog(msg);
}

static void wf(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    }
}

static void md(const char *p)
{
    mkdir(p, 0755);
}

static void run_sh(const char *cmd)
{
    pid_t p = fork();
    if (p == 0) {
        setenv("PATH", "/bin:/sbin", 1);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

static int path_exists(const char *p)
{
    return access(p, F_OK) == 0;
}

static int pid_alive(pid_t p)
{
    return p > 0 && kill(p, 0) == 0;
}

static int try_mount_ro(const char *src, const char *dst, const char *fstype)
{
    md(dst);
    if (mount(src, dst, fstype, MS_RDONLY, NULL) == 0) {
        klogf2("mounted", dst);
        return 0;
    }
    return -1;
}

static int try_mount_part(const char *part, const char *dst, const char *fstype)
{
    const char *dev = blockdev_by_name(part);
    if (!dev)
        return -1;
    return try_mount_ro(dev, dst, fstype);
}

static void ensure_block_layout(void)
{
    blockdev_ensure_by_name();
    md("/vendor");
    md("/vendor/firmware_mnt");
    md("/vendor/bt_firmware");
    md("/mnt/vendor/persist");
    md("/persist");
}

static void mount_radio_partitions(void)
{
    if (!vendor_mounted) {
        if (try_mount_part("vendor", "/vendor", "ext4") == 0 ||
            try_mount_part("vendor_a", "/vendor", "ext4") == 0 ||
            try_mount_part("vendor", "/vendor", "erofs") == 0)
            vendor_mounted = 1;
    }
    if (!modem_mounted && try_mount_part("modem", "/vendor/firmware_mnt", "vfat") == 0)
        modem_mounted = 1;
    if (!bt_fw_mounted && try_mount_part("bluetooth", "/vendor/bt_firmware", "vfat") == 0)
        bt_fw_mounted = 1;
    if (!persist_mounted && try_mount_part("persist", "/mnt/vendor/persist", "ext4") == 0) {
        persist_mounted = 1;
        if (!path_exists("/persist/WCNSS_qcom_wlan_nv.bin"))
            run_sh("cp -a /mnt/vendor/persist/. /persist/ 2>/dev/null");
    }
}

static void symlink_if_missing(const char *target, const char *linkpath)
{
    if (!path_exists(target) || path_exists(linkpath))
        return;
    unlink(linkpath);
    symlink(target, linkpath);
}

static void link_firmware_tree(void)
{
    const char *pairs[] = {
        "/vendor/firmware", "/lib/firmware/vendor",
        "/vendor/firmware_mnt", "/lib/firmware/vendor_mnt",
        "/vendor/bt_firmware", "/lib/firmware/bt_firmware",
        "/vendor/firmware/wlan", "/lib/firmware/wlan/vendor",
        NULL, NULL
    };

    md("/lib/firmware/wlan/qca_cld");
    for (int i = 0; pairs[i]; i += 2)
        symlink_if_missing(pairs[i], pairs[i + 1]);

    symlink_if_missing("/vendor/etc/wifi/WCNSS_qcom_cfg.ini",
                       "/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini");
    symlink_if_missing("/mnt/vendor/persist/WCNSS_qcom_wlan_nv.bin",
                       "/lib/firmware/wlan/qca_cld/WCNSS_qcom_wlan_nv.bin");
    symlink_if_missing("/persist/WCNSS_qcom_wlan_nv.bin",
                       "/lib/firmware/wlan/qca_cld/WCNSS_qcom_wlan_nv.bin");
    symlink_if_missing("/vendor/firmware_mnt/wlan_mac.bin",
                       "/lib/firmware/wlan/qca_cld/wlan_mac.bin");
    symlink_if_missing("/vendor/bt_firmware/image/crbtfw21.tlv",
                       "/lib/firmware/qca/crbtfw21.tlv");
    symlink_if_missing("/vendor/bt_firmware/image/crnv21.bin",
                       "/lib/firmware/qca/crnv21.bin");
}

static int dir_has_files(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;

    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        closedir(d);
        return 1;
    }
    closedir(d);
    return 0;
}

static int has_wlan_firmware(void)
{
    static const char *paths[] = {
        "/lib/firmware/wlan/qca_cld",
        "/lib/firmware/vendor/firmware/wlan/qca_cld",
        "/vendor/firmware/wlan/qca_cld",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        if (dir_has_files(paths[i]))
            return 1;
    }
    return 0;
}

static void rfkill_unblock_all(void)
{
    run_sh("for f in /sys/class/rfkill/rfkill*/state; do "
           "[ -f \"$f\" ] && echo 1 > \"$f\" 2>/dev/null; done");
}

static void trigger_wlan_power(void)
{
    const char *triggers[] = {
        "/sys/kernel/boot_wlan/boot_wlan",
        "/sys/module/wlan/parameters/con_mode",
        NULL
    };

    wf("/sys/kernel/shutdown_wlan/shutdown", "0");
    for (int i = 0; triggers[i]; i++) {
        if (access(triggers[i], W_OK) == 0)
            wf(triggers[i], "1");
    }
    run_sh("for p in /sys/devices/platform/soc/*/wcnss_wlan "
           "/sys/bus/platform/drivers/icnss*/*/wcnss_wlan; do "
           "[ -e \"$p\" ] && echo 1 > \"$p\" 2>/dev/null; done");
}

static void wait_for_iface(const char *sys_path, int sec)
{
    for (int i = 0; i < sec * 2; i++) {
        if (path_exists(sys_path))
            return;
        usleep(500000);
    }
}

static void try_wlan_enable(void)
{
    trigger_wlan_power();
    wait_for_iface("/sys/class/net/wlan0", 10);

    if (path_exists("/sys/class/net/wlan0")) {
        run_sh("ip link set wlan0 up 2>/dev/null");
        char mac[32] = "";
        int fd = open("/sys/class/net/wlan0/address", O_RDONLY);
        if (fd >= 0) {
            char b[32];
            ssize_t n = read(fd, b, sizeof(b) - 1);
            close(fd);
            if (n > 0) {
                b[n] = '\0';
                char *nl = strchr(b, '\n');
                if (nl)
                    *nl = '\0';
                snprintf(mac, sizeof(mac), " %s", b);
            }
        }
        snprintf(wifi_st, sizeof(wifi_st), "WiFi: wlan0 up%s", mac);
        klog("wlan0 up");
        return;
    }

    if (has_wlan_firmware())
        snprintf(wifi_st, sizeof(wifi_st), vendor_mounted ?
                 "WiFi: fw ok no iface" : "WiFi: fw bundled no iface");
    else if (vendor_mounted)
        snprintf(wifi_st, sizeof(wifi_st), "WiFi: vendor no wlan fw");
    else
        snprintf(wifi_st, sizeof(wifi_st), "WiFi: need vendor mount");
}

static void bt_power_on(void)
{
    wf("/sys/module/bluetooth_power/parameters/power", "1");
    run_sh("for f in /sys/class/rfkill/rfkill*/state; do "
           "[ -f \"$f\" ] && echo 1 > \"$f\" 2>/dev/null; done");
}

static void try_bt_enable(void)
{
    bt_power_on();
    wait_for_iface("/sys/class/bluetooth/hci0", 8);

    if (path_exists("/sys/class/bluetooth/hci0")) {
        snprintf(bt_st, sizeof(bt_st), "BT: hci0 up");
        klog("hci0 up");
        return;
    }
    if (path_exists("/lib/firmware/qca/crbtfw21.tlv") ||
        path_exists("/vendor/bt_firmware/image/crbtfw21.tlv"))
        snprintf(bt_st, sizeof(bt_st), bt_fw_mounted ?
                 "BT: fw ok no hci" : "BT: fw bundled no hci");
    else if (bt_fw_mounted)
        snprintf(bt_st, sizeof(bt_st), "BT: bt part empty");
    else
        snprintf(bt_st, sizeof(bt_st), "BT: need bt part");
}

static void write_radio_log(void)
{
    int fd = open("/tmp/radio.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    dprintf(fd, "vendor=%d persist=%d modem=%d bt_fw=%d wlan_fw=%d\n",
            vendor_mounted, persist_mounted, modem_mounted, bt_fw_mounted,
            has_wlan_firmware());
    close(fd);
}

static void write_radio_status(void)
{
    int fd = open("/tmp/radio.status", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dprintf(fd, "%s\n%s\n", wifi_st, bt_st);
        close(fd);
    }
}

static void radio_work(void)
{
    klog("bringup start");
    ensure_block_layout();
    rfkill_unblock_all();
    mount_radio_partitions();
    link_firmware_tree();
    write_radio_log();
    try_wlan_enable();
    try_bt_enable();
    write_radio_status();
    klog("bringup done");
}

static void scan_work(void)
{
    int fd = open("/tmp/wifi-scan.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);

    if (!path_exists("/sys/class/net/wlan0")) {
        run_sh("echo 'wlan0 down — run: radio' > /tmp/wifi-scan.txt");
        return;
    }
    run_sh("/sbin/wlan_scan > /tmp/wifi-scan.txt 2>&1");
}

static void start_job(pid_t *slot, void (*fn)(void))
{
    if (pid_alive(*slot))
        return;
    pid_t p = fork();
    if (p != 0) {
        if (p > 0)
            *slot = p;
        return;
    }
    fn();
    _exit(0);
}

void radio_request_async(void)
{
    start_job(&radio_pid, radio_work);
}

void radio_init_async(void)
{
    radio_request_async();
}

void radio_probe_now(void)
{
    radio_request_async();
}

void radio_scan_request_async(void)
{
    start_job(&scan_pid, scan_work);
}

void radio_poll(void)
{
    int st;
    pid_t p;

    if (radio_pid > 0) {
        p = waitpid(radio_pid, &st, WNOHANG);
        if (p == radio_pid)
            radio_pid = 0;
    }
    if (scan_pid > 0) {
        p = waitpid(scan_pid, &st, WNOHANG);
        if (p == scan_pid)
            scan_pid = 0;
    }
}

int radio_job_running(void)
{
    return pid_alive(radio_pid);
}

int radio_scan_running(void)
{
    return pid_alive(scan_pid);
}

const char *radio_wifi_status(void)
{
    return wifi_st;
}

const char *radio_bt_status(void)
{
    return bt_st;
}

int radio_format_status(char *buf, size_t bufsz)
{
    char wline[80] = "WiFi: ?";
    char bline[80] = "BT: ?";
    int fd = open("/tmp/radio.status", O_RDONLY);

    if (fd >= 0) {
        char raw[160];
        ssize_t n = read(fd, raw, sizeof(raw) - 1);
        close(fd);
        if (n > 0) {
            char *nl;
            raw[n] = '\0';
            snprintf(wline, sizeof(wline), "%s", raw);
            nl = strchr(wline, '\n');
            if (nl) {
                *nl = '\0';
                snprintf(bline, sizeof(bline), "%s", nl + 1);
                nl = strchr(bline, '\n');
                if (nl)
                    *nl = '\0';
            }
        }
    }

    return snprintf(buf, bufsz,
                    "radio\r\n"
                    "%s\r\n%s\r\n"
                    "wlan0=%s hci0=%s\r\n"
                    "bringup=%s scan=%s\r\n",
                    wline, bline,
                    path_exists("/sys/class/net/wlan0") ? "yes" : "no",
                    path_exists("/sys/class/bluetooth/hci0") ? "yes" : "no",
                    radio_job_running() ? "running" : "idle",
                    radio_scan_running() ? "running" : "idle");
}
