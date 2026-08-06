#define _GNU_SOURCE
#include "firmware.h"
#include "blockdev.h"
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
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <linux/capability.h>

extern int modem_mounted, bt_fw_mounted;
void symlink_force(const char *target, const char *linkpath)
{
    if (!path_exists(target))
        return;
    unlink(linkpath);
    symlink(target, linkpath);
}


void ensure_linker64_real(void)
{
    const char *candidates[] = {
        "/lib64/linker64",
        "/vendor/bin/linker64",
        NULL
    };
    const char *src = NULL;
    struct stat st;

    for (int i = 0; candidates[i]; i++) {
        if (lstat(candidates[i], &st) == 0 && S_ISREG(st.st_mode)) {
            src = candidates[i];
            break;
        }
    }
    /* Fall back: check if system has a real linker64 */
    if (!src) {
        if (lstat("/system/bin/linker64", &st) == 0 && S_ISREG(st.st_mode))
            src = "/system/bin/linker64";
        else if (lstat("/system/lib64/bootstrap/linker64", &st) == 0 && S_ISREG(st.st_mode))
            src = "/system/lib64/bootstrap/linker64";
    }
    if (!src) {
        LOGI("radio", "%s", "linker64: not found in any candidate");
        return;
    }

    /* /lib64 is on our tmpfs ramdisk — always writable */
    if (lstat("/lib64/linker64", &st) != 0 || !S_ISREG(st.st_mode)) {
        if (lstat("/lib64/linker64", &st) == 0)
            unlink("/lib64/linker64");
        copy_file_bin(src, "/lib64/linker64");
        chmod("/lib64/linker64", 0755);
    }

    /* /apex is writable tmpfs — Android 12+ PT_INTERP chain resolves through here */
    md("/apex");
    md("/apex/com.android.runtime");
    md("/apex/com.android.runtime/bin");
    if (lstat("/apex/com.android.runtime/bin/linker64", &st) != 0 || !S_ISREG(st.st_mode)) {
        if (lstat("/apex/com.android.runtime/bin/linker64", &st) == 0)
            unlink("/apex/com.android.runtime/bin/linker64");
        copy_file_bin("/lib64/linker64", "/apex/com.android.runtime/bin/linker64");
        chmod("/apex/com.android.runtime/bin/linker64", 0755);
    }

    /* /system/bin/linker64 is on a read-only partition — skip if already a real file,
     * otherwise log a warning. The ELOOP/ENOENT is expected; we always use the
     * exec_via_linker64() fallback which directly calls /lib64/linker64. */
    if (lstat("/system/bin/linker64", &st) != 0 || !S_ISREG(st.st_mode)) {
        LOGI("radio", "%s", "linker64: /system/bin/linker64 not a regular file — using /lib64/linker64 fallback");
    }
}


void ensure_linkerconfig(void)
{
    const char *fallback_ld =
        "dir.system = /system/bin/\n"
        "dir.vendor = /vendor/bin/\n"
        "dir.product = /product/bin/\n"
        "[system]\n"
        "namespace.default.isolated = false\n"
        "namespace.default.search.paths = /system/lib64:/vendor/lib64:/lib64\n"
        "namespace.default.permitted.paths = /system/lib64:/vendor/lib64:/lib64\n"
        "[vendor]\n"
        "namespace.default.isolated = false\n"
        "namespace.default.search.paths = /vendor/lib64:/system/lib64:/lib64\n"
        "namespace.default.permitted.paths = /vendor/lib64:/system/lib64:/lib64\n"
        "[default]\n"
        "namespace.default.isolated = false\n"
        "namespace.default.search.paths = /lib64:/vendor/lib64:/system/lib64\n"
        "namespace.default.permitted.paths = /lib64:/vendor/lib64:/system/lib64\n";

    md("/linkerconfig");
    if (path_exists("/linkerconfig/ld.config.txt"))
        return;

    if (path_exists("/vendor/etc/ld.config.txt")) {
        copy_file_bin("/vendor/etc/ld.config.txt", "/linkerconfig/ld.config.txt");
        chmod("/linkerconfig/ld.config.txt", 0444);
        LOGI("radio", "%s", "linkerconfig: vendor ld.config.txt");
        return;
    }

    write_file("/linkerconfig/ld.config.txt", fallback_ld);
    chmod("/linkerconfig/ld.config.txt", 0444);
    LOGI("radio", "%s", "linkerconfig: fallback ld.config.txt");
}


void ensure_system_bin_tools(void)
{
    const char *tools[] = {
        "sh", "echo", "ls", "cat", "id", "mkdir", "touch", "chmod", NULL
    };

    md("/system/bin");
    if (!path_exists("/system/lib64") && path_exists("/lib64"))
        symlink_force("/lib64", "/system/lib64");

    for (int i = 0; tools[i]; i++) {
        char dst[128], src[128];
        snprintf(dst, sizeof(dst), "/system/bin/%s", tools[i]);
        if (path_exists(dst))
            continue;
        snprintf(src, sizeof(src), "/bin/%s", tools[i]);
        if (path_exists(src))
            symlink_force(src, dst);
        else if (path_exists("/bin/busybox"))
            symlink_force("/bin/busybox", dst);
    }
}


void ensure_android_roots(void)
{
    md("/data");
    md("/data/vendor");
    md("/data/vendor/wifi");
    md("/data/vendor/firmware");
    md("/data/vendor/tombstones");
    md("/data/misc");
    md("/data/misc/wifi");
    md("/data/misc/apexdata");
    md("/data/vendor/apex");
    md("/apex");
    if (access("/apex", W_OK) != 0)
        mount("tmpfs", "/apex", "tmpfs", 0, "mode=0755");
    md("/system");
    md("/system/etc");
    ensure_linkerconfig();
    if (!path_exists("/system/etc/ld.config.txt"))
        symlink_force("/linkerconfig/ld.config.txt", "/system/etc/ld.config.txt");
    ensure_system_bin_tools();
    md("/sbin");
    md("/etc");
    ensure_etc_group();
    ensure_etc_passwd();
    md("/firmware");
    md("/firmware/image");
    md("/bt_firmware");
    md("/dsp");

    ensure_linker64_real();
    if (modem_mounted && path_exists("/vendor/firmware_mnt/image")) {
        symlink_force("/vendor/firmware_mnt/image", "/firmware/image");
        symlink_force("/vendor/firmware_mnt/image", "/firmware/wlan");
    }
    if (bt_fw_mounted && path_exists("/vendor/bt_firmware"))
        symlink_force("/vendor/bt_firmware", "/bt_firmware/image");
}


void ensure_wifi_config(void)
{
    const char *srcs[] = {
        "/vendor/etc/wifi/WCNSS_qcom_cfg.ini",
        "/initramfs/vendor/etc/wifi/WCNSS_qcom_cfg.ini",
        "/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini",
        NULL
    };
    const char *dst = "/data/misc/wifi/WCNSS_qcom_cfg.ini";

    md("/data/misc/wifi");
    if (path_exists(dst))
        return;
    for (int i = 0; srcs[i]; i++) {
        if (!path_exists(srcs[i]))
            continue;
        {
            char cmd[384];
            snprintf(cmd, sizeof(cmd), "cp -f '%s' '%s' && chmod 644 '%s'",
                     srcs[i], dst, dst);
            run_sh(cmd);
        }
        if (path_exists(dst)) {
            LOGI("radio", "%s", "wifi cfg staged");
            return;
        }
    }
}


void copy_dir_if_present(const char *src, const char *dst)
{
    char cmd[512];
    if (!path_exists(src))
        return;
    snprintf(cmd, sizeof(cmd), "cp -a %s/. %s/ 2>/dev/null", src, dst);
    run_sh(cmd);
}


int try_mount_ro(const char *src, const char *dst, const char *fstype)
{
    md(dst);
    if (mount(src, dst, fstype, MS_RDONLY, NULL) == 0) {
        LOGI("radio", "%s %s", "mounted", dst);
        return 0;
    }
    {
        char msg[192];
        snprintf(msg, sizeof(msg), "mount %s %s fail: %s", dst, fstype,
                 strerror(errno));
        LOGI("radio", "%s", msg);
    }
    return -1;
}


int try_mount_ro_any(const char *src, const char *dst)
{
    const char *types[] = { "ext4", "erofs", "f2fs", "vfat", NULL };
    int last_errno = 0;

    for (int i = 0; types[i]; i++) {
        if (try_mount_ro(src, dst, types[i]) == 0)
            return 0;
        last_errno = errno;
    }
    if (last_errno) {
        char msg[160];
        snprintf(msg, sizeof(msg), "mount fail %s errno=%d", dst, last_errno);
        LOGI("radio", "%s", msg);
    }
    return -1;
}


/* persist (unlike system/vendor/modem/bluetooth firmware images) is meant
 * to be writable at runtime -- real Android mounts it read-write, since
 * tftp_server/rmt_storage create and lchown /mnt/vendor/persist/rfs/* and
 * /mnt/vendor/persist/hlos_rfs/* there during every boot's RFS/EFS sync.
 * try_mount_part()/try_mount_ro_any() forced MS_RDONLY unconditionally
 * here too, so every one of those mkdir/lchown calls failed with EROFS --
 * completely invisible all project until logd_stub started actually
 * capturing the liblog-routed tftp_server error output that revealed it
 * ("mkdir failed: [30] [/mnt/vendor/persist] [Read-only file system]"). */
int try_mount_rw(const char *src, const char *dst, const char *fstype)
{
    md(dst);
    if (mount(src, dst, fstype, 0, NULL) == 0) {
        LOGI("radio", "%s %s", "mounted rw", dst);
        return 0;
    }
    {
        char msg[192];
        snprintf(msg, sizeof(msg), "mount rw %s %s fail: %s", dst, fstype,
                 strerror(errno));
        LOGI("radio", "%s", msg);
    }
    return -1;
}


int try_mount_rw_any(const char *src, const char *dst)
{
    const char *types[] = { "ext4", "erofs", "f2fs", "vfat", NULL };
    int last_errno = 0;

    for (int i = 0; types[i]; i++) {
        if (try_mount_rw(src, dst, types[i]) == 0)
            return 0;
        last_errno = errno;
    }
    if (last_errno) {
        char msg[160];
        snprintf(msg, sizeof(msg), "mount rw fail %s errno=%d", dst, last_errno);
        LOGI("radio", "%s", msg);
    }
    return -1;
}


int try_mount_part_rw(const char *part, const char *dst)
{
    const char *dev = blockdev_by_name(part);
    if (!dev) {
        LOGI("radio", "%s %s", "mount: no blockdev", part);
        return -1;
    }
    return try_mount_rw_any(dev, dst);
}


int try_mount_part(const char *part, const char *dst)
{
    const char *dev = blockdev_by_name(part);
    if (!dev) {
        LOGI("radio", "%s %s", "mount: no blockdev", part);
        return -1;
    }
    return try_mount_ro_any(dev, dst);
}


int vendor_tree_visible(void)
{
    return path_exists("/vendor/bin") || path_exists("/vendor/lib64") ||
           path_exists("/mnt/vendor/bin") || path_exists("/mnt/vendor/lib64");
}


void stage_cnss_daemon_from_vendor(void)
{
    const char *src = NULL;

    if (access("/sbin/cnss-daemon", X_OK) == 0)
        return;
    if (path_exists("/vendor/bin/cnss-daemon"))
        src = "/vendor/bin/cnss-daemon";
    else if (path_exists("/mnt/vendor/bin/cnss-daemon"))
        src = "/mnt/vendor/bin/cnss-daemon";
    if (!src) {
        LOGI("radio", "%s", "cnss-daemon: missing on vendor");
        return;
    }
    if (copy_file_bin(src, "/sbin/cnss-daemon") == 0) {
        chmod("/sbin/cnss-daemon", 0755);
        LOGI("radio", "%s", "cnss-daemon staged to /sbin");
    } else
        LOGI("radio", "%s", "cnss-daemon: copy failed");
}


int mount_vendor_partition(void)
{
    const char *parts[] = { "vendor", "vendor_a", "vendor_b", NULL };
    int mounted = 0;

    if (vendor_mounted && vendor_tree_visible())
        return 0;

    ensure_block_layout();
    md("/mnt/vendor");
    md("/vendor");

    for (int i = 0; parts[i]; i++) {
        if (try_mount_part(parts[i], "/mnt/vendor") == 0) {
            mounted = 1;
            break;
        }
    }

    if (!mounted) {
        const char *dev = blockdev_by_name("vendor");
        if (dev && mount(dev, "/vendor", "ext4", MS_RDONLY, NULL) == 0) {
            LOGI("radio", "%s", "vendor: direct ext4 mount OK");
            vendor_mounted = 1;
            stage_cnss_daemon_from_vendor();
            return 0;
        }
        if (dev) {
            char msg[128];
            snprintf(msg, sizeof(msg), "mount vendor direct fail: %s",
                     strerror(errno));
            LOGI("radio", "%s", msg);
        }
        LOGI("radio", "%s", "vendor: all mount attempts failed");
        return -1;
    }

    if (mount("/mnt/vendor", "/vendor", NULL, MS_BIND, NULL) != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "vendor bind fail: %s", strerror(errno));
        LOGI("radio", "%s", msg);
        vendor_mounted = 1;
        stage_cnss_daemon_from_vendor();
        return 0;
    }
    LOGI("radio", "%s", "vendor: bind /mnt/vendor -> /vendor OK");
    vendor_mounted = 1;
    stage_cnss_daemon_from_vendor();
    return 0;
}


void ensure_block_layout(void)
{
    blockdev_wait_mmc(15);
    blockdev_ensure_by_name();
    md("/vendor");
    md("/vendor/firmware_mnt");
    md("/vendor/bt_firmware");
    md("/mnt/vendor/persist");
    md("/persist");
}


int mount_point_active(const char *dst)
{
    int fd = open("/proc/mounts", O_RDONLY);
    char buf[4096];
    ssize_t n;
    char needle[128];

    if (fd < 0)
        return access(dst, F_OK) == 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    snprintf(needle, sizeof(needle), " %s ", dst);
    return strstr(buf, needle) != NULL;
}


void sync_mount_flags(void)
{
    if (mount_point_active("/vendor"))
        vendor_mounted = 1;
    if (mount_point_active("/system"))
        system_mounted = 1;
    if (mount_point_active("/vendor/firmware_mnt"))
        modem_mounted = 1;
    if (mount_point_active("/vendor/bt_firmware"))
        bt_fw_mounted = 1;
    if (mount_point_active("/mnt/vendor/persist"))
        persist_mounted = 1;
    if (mount_point_active("/persist"))
        persist_mounted = 1;
}


void mount_radio_partitions(void)
{
    sync_mount_flags();
    blockdev_ensure_by_name();
    if (!vendor_mounted || !vendor_tree_visible())
        mount_vendor_partition();
    if (!system_mounted) {
        md("/mnt/system");
        if (try_mount_part("system", "/mnt/system") == 0 ||
            try_mount_part("system_a", "/mnt/system") == 0 ||
            try_mount_part("system_b", "/mnt/system") == 0) {
            struct stat st;
            if (stat("/mnt/system/system", &st) == 0 && S_ISDIR(st.st_mode)) {
                LOGI("radio", "%s", "system-as-root detected, bind mounting /mnt/system/system -> /system");
                if (mount("/mnt/system/system", "/system", NULL, MS_BIND, NULL) == 0)
                    system_mounted = 1;
                else
                    LOGI("radio", "%s", "failed to bind mount /system (system-as-root)");
            } else {
                LOGI("radio", "%s", "legacy system detected, bind mounting /mnt/system -> /system");
                if (mount("/mnt/system", "/system", NULL, MS_BIND, NULL) == 0)
                    system_mounted = 1;
                else
                    LOGI("radio", "%s", "failed to bind mount /system (legacy)");
            }
        }
    }
    if (!modem_mounted && try_mount_part("modem", "/vendor/firmware_mnt") == 0)
        modem_mounted = 1;
    if (!bt_fw_mounted && try_mount_part("bluetooth", "/vendor/bt_firmware") == 0)
        bt_fw_mounted = 1;
    /* /mnt/vendor/persist can't be created: it's not a pre-baked mountpoint
     * dir in this vendor.img (confirmed live: path_exists() is false), and
     * /mnt/vendor itself is mounted MS_RDONLY, so mkdir() into it fails
     * with EROFS -- every mount attempt targeting /mnt/vendor/persist was
     * therefore silently doomed regardless of the rw-vs-ro mount flags
     * fixed above (confirmed via tftp_server's own liblog output, only
     * visible now that logd_stub actually works: "mkdir failed: [30]
     * [/mnt/vendor/persist] [Read-only file system]"). Remount /mnt/vendor
     * rw just long enough to create the one directory, then back to ro --
     * this doesn't touch any real vendor.img file content, just adds an
     * empty mountpoint stub real Android's vendor.img apparently already
     * ships with (this one evidently doesn't). */
    if (!mount_point_active("/mnt/vendor/persist") &&
        access("/mnt/vendor/persist", F_OK) != 0 &&
        mount_point_active("/mnt/vendor")) {
        if (mount(NULL, "/mnt/vendor", NULL, MS_REMOUNT, NULL) == 0) {
            if (mkdir("/mnt/vendor/persist", 0755) == 0)
                LOGI("radio", "%s", "created /mnt/vendor/persist mountpoint");
            else
                LOGI("radio", "%s", "mkdir /mnt/vendor/persist still failed after remount rw");
            mount(NULL, "/mnt/vendor", NULL, MS_REMOUNT | MS_RDONLY, NULL);
        } else {
            LOGI("radio", "%s", "remount /mnt/vendor rw failed");
        }
    }
    if (!persist_mounted && try_mount_part_rw("persist", "/mnt/vendor/persist") == 0) {
        persist_mounted = 1;
    }
    if (!persist_mounted && try_mount_part_rw("persistbak", "/mnt/vendor/persist") == 0) {
        persist_mounted = 1;
    }
    if (!persist_mounted && try_mount_part_rw("persistbak", "/persist") == 0) {
        persist_mounted = 1;
    }
    if (persist_mounted && mount_point_active("/persist") &&
        !mount_point_active("/mnt/vendor/persist")) {
        md("/mnt/vendor/persist");
        if (mount("/persist", "/mnt/vendor/persist", NULL, MS_BIND, NULL) == 0)
            LOGI("radio", "%s", "bind /persist -> /mnt/vendor/persist");
        else {
            char msg[96];
            snprintf(msg, sizeof(msg), "persist bind errno=%d", errno);
            LOGI("radio", "%s", msg);
        }
    }
    if (persist_mounted && !path_exists("/persist/WCNSS_qcom_wlan_nv.bin"))
        run_sh("cp -a /mnt/vendor/persist/. /persist/ 2>/dev/null");
}


void symlink_firmware(const char *target, const char *linkpath)
{
    struct stat st;

    if (!path_exists(target) || access(target, R_OK) != 0)
        return;
    if (lstat(linkpath, &st) == 0) {
        if (S_ISREG(st.st_mode) && access(linkpath, R_OK) == 0)
            return;
        if (S_ISLNK(st.st_mode)) {
            char resolved[384];
            ssize_t n = readlink(linkpath, resolved, sizeof(resolved) - 1);
            if (n > 0) {
                resolved[n] = '\0';
                if (path_exists(resolved) && access(resolved, R_OK) == 0)
                    return;
            }
        }
    }
    unlink(linkpath);
    symlink(target, linkpath);
}


void symlink_if_missing(const char *target, const char *linkpath)
{
    if (!path_exists(target) || path_exists(linkpath))
        return;
    unlink(linkpath);
    symlink(target, linkpath);
}


void link_fw_bin(const char *srcdir, const char *name)
{
    char src[384], dst[384];

    snprintf(src, sizeof(src), "%s/%s", srcdir, name);
    snprintf(dst, sizeof(dst), "/lib/firmware/wlan/qca_cld/%s", name);
    symlink_firmware(src, dst);
}


static int modem_image_dir_ready(void)
{
    return path_exists("/vendor/firmware_mnt/image/modem.mdt") ||
           path_exists("/vendor/firmware_mnt/image/wlanmdsp.mbn");
}


static int mount_modem_image(const char *src)
{
    /* Guard against a real, confirmed live race: this function's own
     * unmount+remount is disruptive (the mountpoint briefly has nothing
     * mounted on it at all), and it has multiple independent callers
     * (ensure_modem_firmware_mounted()'s own retry loop, reached from at
     * least 4 different places across cnss.c/firmware.c/modem.c, several
     * of which run in their own forked processes). If /vendor/firmware_mnt
     * is already correctly populated, do nothing -- confirmed live via an
     * LD_PRELOAD opendir() trace that pd-mapper's opendir() on
     * /vendor/firmware_mnt/image got a real ENOENT at the exact moment
     * another caller's concurrent remount here would have torn the
     * mountpoint down, despite the directory being verified present and
     * populated a moment before and after. */
    if (modem_image_dir_ready())
        return 0;
    umount2("/vendor/firmware_mnt", MNT_DETACH);
    md("/vendor/firmware_mnt");
    if (try_mount_ro_any(src, "/vendor/firmware_mnt") == 0)
        return modem_image_dir_ready() ? 0 : -1;
    return -1;
}


static void try_modem_sd_fallback(void)
{
    static const char *imgs[] = {
        "/mnt/sdcard/minios/NON-HLOS.bin",
        "/mnt/sdcard/minios/modem/NON-HLOS.bin",
        "/persist/minios/NON-HLOS.bin",
        NULL
    };

    if (modem_image_dir_ready())
        return;
    for (int i = 0; imgs[i]; i++) {
        if (!path_exists(imgs[i]))
            continue;
        if (mount_modem_image(imgs[i]) == 0) {
            modem_mounted = 1;
            plog_append("modem-fw: mounted from SD NON-HLOS");
            radio_trace("modem-fw: SD NON-HLOS mount OK");
            return;
        }
    }
}


void set_firmware_class_path(void)
{
    const char *fwdir = "/lib/firmware";
    const char *sysfs = "/sys/module/firmware_class/parameters/path";
    char cur[256];
    int fd;

    if (path_exists("/vendor/firmware_mnt/image/modem.mdt"))
        fwdir = "/vendor/firmware_mnt/image";
    else if (!path_exists("/lib/firmware/modem.mdt"))
        return;
    fd = open(sysfs, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, cur, sizeof(cur) - 1);
        close(fd);
        if (n > 0) {
            cur[n] = '\0';
            {
                char *nl = strchr(cur, '\n');
                if (nl)
                    *nl = '\0';
            }
            if (!strcmp(cur, fwdir))
                return;
        }
    }
    if (wf_checked(sysfs, fwdir) == 0) {
        LOGI("radio", "%s %s", "fw: firmware_class.path", fwdir);
        plog_append("fw: firmware_class.path=/vendor/firmware_mnt/image");
    } else {
        LOGI("radio", "%s", "fw: firmware_class.path write failed");
        plog_append("fw: firmware_class.path write failed");
    }
}


int link_modem_pil_firmware_count(void)
{
    const char *srcdir = "/vendor/firmware_mnt/image";
    DIR *d;
    struct dirent *e;
    char src[384], dst[384];
    int n = 0;

    if (!path_exists(srcdir))
        return 0;

    d = opendir(srcdir);
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (strncmp(e->d_name, "modem.", 6) != 0)
            continue;
        snprintf(src, sizeof(src), "%s/%s", srcdir, e->d_name);
        snprintf(dst, sizeof(dst), "/lib/firmware/%s", e->d_name);
        symlink_firmware(src, dst);
        if (path_exists(dst))
            n++;
    }
    closedir(d);
    return n;
}


void link_modem_pil_firmware(void)
{
    (void)link_modem_pil_firmware_count();
}


int ensure_modem_pil_firmware(void)
{
    char line[128];
    int links;
    int ok_mdt = 0;
    int ok_b = 0;
    const char *part_mdt = "/vendor/firmware_mnt/image/modem.mdt";

    ensure_modem_firmware_mounted();
    set_firmware_class_path();
    links = link_modem_pil_firmware_count();
    ok_mdt = path_exists("/lib/firmware/modem.mdt") || path_exists(part_mdt);
    if (path_exists(part_mdt)) {
        if (copy_file_bin(part_mdt, "/lib/firmware/modem.mdt") == 0)
            plog_append("modem-pil-fw: copied modem.mdt to /lib/firmware");
        run_sh("cp /vendor/firmware_mnt/image/modem.b* /lib/firmware/ 2>/dev/null");
        ok_mdt = path_exists("/lib/firmware/modem.mdt");
        DIR *d = opendir("/vendor/firmware_mnt/image");
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) != NULL) {
                if (!strncmp(e->d_name, "modem.b", 7)) {
                    ok_b = 1;
                    break;
                }
            }
            closedir(d);
        }
    }
    if (!ok_b) {
        for (int i = 0; i < 30; i++) {
            char probe[64];
            snprintf(probe, sizeof(probe), "/lib/firmware/modem.b%02d", i);
            if (path_exists(probe)) {
                ok_b = 1;
                break;
            }
        }
    }
    snprintf(line, sizeof(line),
             "modem-pil-fw: links=%d mdt=%d bseg=%d mounted=%d",
             links, ok_mdt, ok_b, modem_mounted);
    LOGI("radio", "%s", line);
    plog_append(line);
    if (!ok_mdt && path_exists("/lib/firmware/modem.mdt"))
        ok_mdt = 1;
    if (!ok_b) {
        for (int i = 0; i < 30; i++) {
            char probe[64];
            snprintf(probe, sizeof(probe), "/lib/firmware/modem.b%02d", i);
            if (path_exists(probe) && access(probe, R_OK) == 0) {
                ok_b = 1;
                break;
            }
        }
    }
    set_firmware_class_path();
    if (!ok_mdt) {
        plog_append("modem-pil-fw: ERROR no modem.mdt");
        return -1;
    }
    if (!ok_b)
        plog_append("modem-pil-fw: WARN no modem.b* visible (PIL may still use partition path)");
    return 0;
}


void harvest_fw_bins(const char *srcdir, int depth)
{
    DIR *d;
    struct dirent *e;
    struct stat st;
    char path[384];

    if (depth <= 0 || !path_exists(srcdir))
        return;
    d = opendir(srcdir);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", srcdir, e->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            harvest_fw_bins(path, depth - 1);
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            size_t n = strlen(e->d_name);
            if ((n > 4 && !strcmp(e->d_name + n - 4, ".bin")) ||
                strstr(e->d_name, ".mbn") || strstr(e->d_name, ".tlv"))
                link_fw_bin(srcdir, e->d_name);
        }
    }
    closedir(d);
}


void link_board_data_variants(const char *srcdir)
{
    DIR *d = opendir(srcdir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char dst30[384], link30[384], linkbase[384];

        if (strncmp(e->d_name, "bdwlan.", 7) != 0)
            continue;
        link_fw_bin(srcdir, e->d_name);
        snprintf(dst30, sizeof(dst30), "bdwlan30.%s", e->d_name + 7);
        snprintf(link30, sizeof(link30), "/lib/firmware/wlan/qca_cld/%s", dst30);
        snprintf(linkbase, sizeof(linkbase), "/lib/firmware/wlan/qca_cld/%s", e->d_name);
        if (!path_exists(link30) && path_exists(linkbase))
            symlink_firmware(linkbase, link30);
    }
    closedir(d);
}


void link_fw_version_aliases(void)
{
    const char *pairs[][2] = {
        { "bdwlan30.bin", "bdwlan.bin" },
        { "bdwlan30.bin", "bdwlan20.bin" },
        { "qwlan30.bin", "qwlan.bin" },
        { "qwlan30.bin", "qwlan20.bin" },
        { "otp30.bin", "otp.bin" },
        { "otp30.bin", "otp20.bin" },
        { "athwlan.bin", "qwlan30.bin" },
        { "athwlan.bin", "qwlan.bin" },
        { "fakeboar.bin", "bdwlan30.bin" },
        { "fakeboar.bin", "bdwlan.bin" },
        { NULL, NULL }
    };
    char base[] = "/lib/firmware/wlan/qca_cld";
    char want[384], alt[384];

    for (int i = 0; pairs[i][0]; i++) {
        snprintf(want, sizeof(want), "%s/%s", base, pairs[i][0]);
        if (path_exists(want))
            continue;
        snprintf(alt, sizeof(alt), "%s/%s", base, pairs[i][1]);
        if (path_exists(alt))
            symlink_firmware(alt, want);
    }
}


void link_known_firmware_bins(void)
{
    const char *bins[] = {
        "bdwlan30.bin", "qwlan30.bin", "otp30.bin", "utf30.bin", "utfbd30.bin",
        "bdwlan20.bin", "qwlan20.bin", "otp20.bin",
        "bdwlan.bin", "qwlan.bin", "otp.bin",
        NULL
    };
    const char *dirs[] = {
        "/vendor/firmware/wlan/qca_cld",
        "/vendor/firmware_mnt/image",
        "/lib/firmware/wlan/vendor/wlan/qca_cld",
        NULL
    };

    for (int d = 0; dirs[d]; d++) {
        for (int b = 0; bins[b]; b++)
            link_fw_bin(dirs[d], bins[b]);
    }
    symlink_if_missing("/mnt/vendor/persist/wlan_mac.bin",
                       "/lib/firmware/wlan/qca_cld/wlan_mac.bin");
    symlink_if_missing("/mnt/vendor/persist/wlan/wlan_mac.bin",
                       "/lib/firmware/wlan/qca_cld/wlan_mac.bin");
    symlink_if_missing("/persist/wlan_mac.bin",
                       "/lib/firmware/wlan/qca_cld/wlan_mac.bin");
    symlink_if_missing("/persist/wlan/wlan_mac.bin",
                       "/lib/firmware/wlan/qca_cld/wlan_mac.bin");
    symlink_if_missing("/mnt/vendor/persist/WCNSS_qcom_wlan_nv.bin",
                       "/lib/firmware/wlan/qca_cld/WCNSS_qcom_wlan_nv.bin");
    symlink_if_missing("/persist/WCNSS_qcom_wlan_nv.bin",
                       "/lib/firmware/wlan/qca_cld/WCNSS_qcom_wlan_nv.bin");
}


static const char *find_wlan_fw_src(const char *name)
{
    static const char *bases[] = {
        "/vendor/firmware_mnt/image",
        "/vendor/firmware/wlan/qca_cld",
        "/lib/firmware/wlan/qca_cld",
        NULL
    };
    static char path[384];

    for (int i = 0; bases[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", bases[i], name);
        if (path_exists(path))
            return path;
    }
    return NULL;
}


void ensure_rmtfs_readonly_layout(void)
{
    static const char *bins[] = {
        "wlanmdsp.mbn",
        "bdwlan.bin", "bdwlan.b02", "bdwlan.b04", "bdwlan.b05",
        "bdwlan.b06", "bdwlan.b07", "bdwlan.b08", "bdwlan.b09",
        "bdwlan.b0a", "bdwlan.b0b",
        NULL
    };
    static const char *link_fmt[] = {
        "/readonly/firmware/image/%s",
        "/readonly/vendor/firmware/%s",
        "/readonly/vendor/firmware_mnt/image/%s",
        NULL
    };
    char link[384];
    int linked = 0;

    md("/readonly/firmware/image");
    md("/readonly/vendor/firmware");
    md("/readonly/vendor/firmware_mnt/image");

    for (int i = 0; bins[i]; i++) {
        const char *src = find_wlan_fw_src(bins[i]);
        if (!src)
            continue;
        for (int j = 0; link_fmt[j]; j++) {
            snprintf(link, sizeof(link), link_fmt[j], bins[i]);
            symlink_firmware(src, link);
            if (path_exists(link))
                linked++;
        }
    }
    {
        char line[96];
        const char *wm = find_wlan_fw_src("wlanmdsp.mbn");
        snprintf(line, sizeof(line), "rmtfs-readonly: wlanmdsp=%s links=%d",
                 wm ? wm : "missing", linked);
        radio_trace(line);
    }
}


void ensure_rmtfs_boot_paths(void)
{
    static const struct {
        const char *link;
        const char *parts[4];
    } map[] = {
        { "/boot/modem_fs1", { "modemst1", "fsc", NULL } },
        { "/boot/modem_fs2", { "modemst2", NULL } },
        { "/boot/modem_fsg", { "fsg", NULL } },
        { "/boot/modem_fsc", { "fsc", NULL } },
        { NULL, { NULL } }
    };
    int n = 0;

    md("/boot");
    for (int i = 0; map[i].link; i++) {
        if (path_exists(map[i].link))
            continue;
        for (int j = 0; map[i].parts[j]; j++) {
            const char *dev = blockdev_by_name(map[i].parts[j]);
            if (!dev)
                continue;
            symlink_if_missing(dev, map[i].link);
            if (path_exists(map[i].link)) {
                char line[128];
                snprintf(line, sizeof(line), "rmtfs-boot: %s -> %s",
                         map[i].link, dev);
                radio_trace(line);
                n++;
                break;
            }
        }
    }
    if (n == 0)
        radio_trace("rmtfs-boot: no /boot/modem_* symlinks created");
}


int ensure_modem_firmware_mounted(void)
{
    static const char *parts[] = { "modem", "modem_a", "modem_b", NULL };
    static const char *probe = "/vendor/firmware_mnt/image/wlanmdsp.mbn";

    if (modem_image_dir_ready())
        return 0;

    for (int i = 0; i < 30; i++) {
        blockdev_ensure_by_name();
        sync_mount_flags();
        if (!modem_image_dir_ready()) {
            for (int p = 0; parts[p]; p++) {
                const char *dev = blockdev_by_name(parts[p]);
                if (!dev)
                    continue;
                if (mount_modem_image(dev) == 0) {
                    modem_mounted = 1;
                    plog_append("modem-fw: GPT modem partition OK");
                    break;
                }
            }
        }
        if (!modem_image_dir_ready())
            try_modem_sd_fallback();
        if (path_exists(probe) || modem_image_dir_ready()) {
            char line[128];
            snprintf(line, sizeof(line), "modem-fw: ready mdt=%d wlanmdsp=%d",
                     path_exists("/vendor/firmware_mnt/image/modem.mdt"),
                     path_exists(probe));
            radio_trace(line);
            plog_append(line);
            return 0;
        }
        if (i == 0 || (i % 4) == 0) {
            char line[96];
            snprintf(line, sizeof(line), "modem-fw: wait i=%d mounted=%d",
                     i, modem_mounted);
            radio_trace(line);
        }
        usleep(500000);
    }
    radio_trace("modem-fw: modem partition empty/missing after 15s");
    plog_append("modem-fw: ERROR partition empty — flash NON-HLOS.bin");
    return -1;
}


void ensure_rmtfs_firmware_paths(void)
{
    ensure_modem_firmware_mounted();
    ensure_rmtfs_boot_paths();
    link_modem_pil_firmware();
    harvest_fw_bins("/vendor/firmware_mnt/image", 1);
    ensure_rmtfs_readonly_layout();
    if (!path_exists("/readonly/firmware/image/wlanmdsp.mbn")) {
        const char *src = find_wlan_fw_src("wlanmdsp.mbn");
        if (src) {
            symlink_firmware(src, "/readonly/firmware/image/wlanmdsp.mbn");
            symlink_firmware(src, "/readonly/vendor/firmware/wlanmdsp.mbn");
            symlink_firmware(src, "/readonly/vendor/firmware_mnt/image/wlanmdsp.mbn");
            radio_trace("rmtfs-readonly: wlanmdsp symlinks forced from late mount");
        }
    }
}


void link_firmware_tree(void)
{
    const char *pairs[] = {
        "/vendor/firmware", "/lib/firmware/vendor",
        "/vendor/firmware_mnt", "/lib/firmware/vendor_mnt",
        "/vendor/bt_firmware", "/lib/firmware/bt_firmware",
        "/vendor/firmware/wlan", "/lib/firmware/wlan/vendor",
        NULL, NULL
    };

    md("/lib/firmware/wlan/qca_cld");
    md("/lib/firmware/wlan/vendor-pull");
    for (int i = 0; pairs[i]; i += 2)
        symlink_if_missing(pairs[i], pairs[i + 1]);

    symlink_if_missing("/vendor/etc/wifi/WCNSS_qcom_cfg.ini",
                       "/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini");
    symlink_if_missing("/initramfs/vendor/etc/wifi/WCNSS_qcom_cfg.ini",
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

    /* Flatten vendor WLAN blobs when mounted */
    copy_dir_if_present("/lib/firmware/wlan/vendor-pull/wlan/qca_cld",
                        "/lib/firmware/wlan/qca_cld");
    copy_dir_if_present("/lib/firmware/wlan/vendor/wlan/qca_cld",
                        "/lib/firmware/wlan/qca_cld");
    copy_dir_if_present("/vendor/firmware/wlan/qca_cld",
                        "/lib/firmware/wlan/qca_cld");
    copy_dir_if_present("/vendor/firmware_mnt/image",
                        "/lib/firmware/wlan/qca_cld");
    {
        const char *harvest[] = {
            "/vendor/firmware_mnt/image",
            "/vendor/firmware/wlan/qca_cld",
            "/vendor/firmware/wlan",
            "/lib/firmware/wlan/vendor/wlan/qca_cld",
            NULL
        };
        for (int i = 0; harvest[i]; i++)
            harvest_fw_bins(harvest[i], 2);
    }
    link_known_firmware_bins();
    link_modem_pil_firmware();
    link_board_data_variants("/vendor/firmware_mnt/image");
    link_fw_version_aliases();
}


int dir_has_files(const char *path)
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


int path_has_bin_files(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;

    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".bin"))
            {
                closedir(d);
                return 1;
            }
        if (strstr(e->d_name, ".tlv") || strstr(e->d_name, ".mbn"))
            {
                closedir(d);
                return 1;
            }
    }
    closedir(d);
    return 0;
}


int has_wlan_firmware(void)
{
    const char *paths[] = {
        "/lib/firmware/wlan/qca_cld",
        "/lib/firmware/wlan/vendor/wlan/qca_cld",
        "/lib/firmware/wlan/vendor-pull/wlan/qca_cld",
        "/vendor/firmware/wlan/qca_cld",
        "/vendor/firmware_mnt/image",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        if (path_has_bin_files(paths[i]))
            return 1;
    }
    return dir_has_files("/lib/firmware/wlan/qca_cld") &&
           path_exists("/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini");
}


void radio_stage_early_bins(void)
{
    stage_cnss_daemon_from_vendor();
}


void ensure_debugfs(void)
{
    if (access("/sys/kernel/debug/icnss", F_OK) == 0)
        return;
    if (access("/sys/kernel", F_OK) != 0)
        return;
    if (access("/sys/kernel/debug", F_OK) != 0)
        mkdir("/sys/kernel/debug", 0755);
    if (access("/sys/kernel/debug/icnss", F_OK) != 0)
        mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
}


