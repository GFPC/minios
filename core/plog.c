#define _GNU_SOURCE
#include "minios/plog.h"
#include "blockdev.h"
#include "minios/log.h"
#include "minios/watchdog.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char log_dir[128]  = "";
static char log_file[160] = "";
static char log_src[64]   = "?";
static unsigned boot_seq  = 0;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int path_writable(const char *path)
{
    return access(path, W_OK) == 0;
}

/* mount() can block indefinitely (uninterruptible D-state) reading a
 * superblock on a card with a damaged filesystem — this happened for real
 * after repeated hard power-cycles left the SD card's FAT32 dirty/corrupt,
 * and it hung all of boot forever (no USB, no further logging — the
 * hardware watchdog doesn't work on this device either, so there was no
 * automatic recovery). Do the mount in a child with a hard deadline: if it
 * doesn't return in time, abandon it (it may stay stuck in D-state forever,
 * but only that orphan is stuck — the rest of boot proceeds). */
static int mount_with_timeout(const char *dev, const char *mnt,
                               const char *type, int timeout_sec)
{
    pid_t p = fork();

    if (p < 0)
        return -1;
    if (p == 0)
        _exit(mount(dev, mnt, type, 0, NULL) == 0 ? 0 : 1);

    for (int i = 0; i < timeout_sec * 10; i++) {
        int status;
        pid_t r = waitpid(p, &status, WNOHANG);
        if (r == p)
            return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
        usleep(100000);
    }
    return -1; /* child abandoned (possibly stuck in D-state) */
}

static int try_mount_dev(const char *dev, const char *mnt)
{
    static const char *types[] = { "vfat", "exfat", "ext4", "f2fs", NULL };
    int i;

    mkdir(mnt, 0755);
    for (i = 0; types[i]; i++) {
        if (mount_with_timeout(dev, mnt, types[i], 5) == 0)
            return path_writable(mnt) ? 0 : -1;
    }
    return -1;
}

/*
 * Try to mount the SD card.  Redmi Note 8T exposes the external SD as
 * /dev/mmcblk1 (whole disk) or /dev/mmcblk1p1 (first partition).
 * blockdev_by_name() may not find it because udev/by-name symlinks are
 * not created by our minimal initramfs — so we probe the raw device nodes
 * first, then fall back to blockdev_by_name().
 */
static int mount_sd_storage(void)
{
    /* --- Direct device probe (highest priority) --- */
    static const char *direct[] = {
        "/dev/mmcblk1p1",
        "/dev/mmcblk1p2",
        "/dev/mmcblk1",
        "/dev/mmcblk2p1",
        "/dev/mmcblk2",
        NULL
    };
    for (int i = 0; direct[i]; i++) {
        if (access(direct[i], F_OK) != 0)
            continue;
        if (try_mount_dev(direct[i], "/mnt/sdcard") == 0) {
            snprintf(log_src, sizeof(log_src), "%s", direct[i] + 5); /* skip /dev/ */
            return 0;
        }
    }

    /* --- blockdev_by_name() partition names --- */
    static const char *parts[] = {
        "external_sd", "sdcard", "sdcard1", "ext_sd", "sd",
        "sdcard0", "externalsd", NULL
    };
    for (int i = 0; parts[i]; i++) {
        const char *dev = blockdev_by_name(parts[i]);
        if (dev && try_mount_dev(dev, "/mnt/sdcard") == 0) {
            snprintf(log_src, sizeof(log_src), "sd:%s", parts[i]);
            return 0;
        }
    }
    return -1;
}

static int mount_persist_storage(void)
{
    const char *dev;

    dev = blockdev_by_name("persist");
    if (!dev)
        dev = blockdev_by_name("persistbak");
    if (!dev)
        return -1;
    if (try_mount_dev(dev, "/mnt/persist") == 0) {
        snprintf(log_src, sizeof(log_src), "persist");
        return 0;
    }
    return -1;
}

static void setup_log_paths(const char *root)
{
    snprintf(log_dir, sizeof(log_dir), "%s/minios", root);
    mkdir(log_dir, 0755);
    snprintf(log_file, sizeof(log_file), "%s/boot.log", log_dir);
}

static unsigned read_boot_seq(void)
{
    char path[192];
    char buf[24];
    int fd;
    unsigned n = 0;

    snprintf(path, sizeof(path), "%s/boot.seq", log_dir);
    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return 1;
    if (read(fd, buf, sizeof(buf) - 1) > 0) {
        buf[sizeof(buf) - 1] = '\0';
        n = (unsigned)atoi(buf);
    }
    n++;
    lseek(fd, 0, SEEK_SET);
    dprintf(fd, "%u\n", n);
    close(fd);
    return n;
}

/* If a write to the SD-backed log fails, the card's host controller may
 * have silently reset (power glitch, shared-rail brownout, etc.) leaving
 * the old mount stale. Force-unmount and remount so the *next* write has
 * a chance to succeed instead of failing forever for the rest of boot. */
static int recover_sd_mount(void)
{
    umount2("/mnt/sdcard", MNT_DETACH);
    return mount_sd_storage() == 0 ? 0 : -1;
}

static void append_text(const char *s)
{
    int fd;
    ssize_t w;
    size_t len;

    if (!log_file[0] || !s || !s[0])
        return;
    len = strlen(s);
    fd = open(log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    w = (fd >= 0) ? write(fd, s, len) : -1;
    if (w != (ssize_t)len) {
        int saved_errno = errno;
        if (fd >= 0)
            close(fd);
        if (recover_sd_mount() == 0) {
            fd = open(log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                char note[96];
                int n = snprintf(note, sizeof(note),
                         "[plog: write failed errno=%d, recovered via remount]\n",
                         saved_errno);
                write(fd, note, (size_t)n);
                write(fd, s, len);
            }
        }
    }
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static void write_boot_marker(void)
{
    char line[256];
    char subsys[32] = "?";
    int fd;

    fd = open("/sys/bus/msm_subsys/devices/subsys0/state", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, subsys, sizeof(subsys) - 1);
        close(fd);
        if (n > 0) {
            subsys[n] = '\0';
            char *nl = strchr(subsys, '\n');
            if (nl)
                *nl = '\0';
        }
    }
    snprintf(line, sizeof(line),
             "\n======== BOOT #%u src=%s subsys=%s msm_dis=%d ========\n",
             boot_seq, log_src, subsys, wdt_msm_disabled);
    append_text(line);

    /* Dump current mounts */
    fd = open("/proc/mounts", O_RDONLY);
    if (fd >= 0) {
        char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            append_text("--- mounts ---\n");
            append_text(buf);
            append_text("--- end mounts ---\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* kmsg drain child — continuously copies /dev/kmsg to the log file    */
/* ------------------------------------------------------------------ */

static void kmsg_drain_child(void)
{
    int kfd, lfd;
    char buf[4096];

    if (!log_file[0])
        _exit(0);

    /* O_NONBLOCK so read() returns immediately when ring is drained. */
    kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    lfd = open(log_file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (kfd < 0 || lfd < 0) {
        if (kfd >= 0) close(kfd);
        if (lfd >= 0) close(lfd);
        _exit(1);
    }

    /* Seek to end of existing ring so we only write new messages. */
    lseek(kfd, 0, SEEK_END);

    /* fsync periodically (not on every write — bursts of dozens of kmsg
     * lines per 100ms would otherwise thrash a slow SD card). This is a
     * real corruption-window mitigation: without any fsync at all, every
     * card pull risks a dirty FAT32 (seen repeatedly in practice — SD
     * needed chkdsk /f after nearly every hard removal). Bounding the
     * unflushed window to ~2s bounds how much of the tail we can lose. */
    time_t last_sync = time(NULL);
    int dirty = 0;

    for (;;) {
        ssize_t n = read(kfd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t w = write(lfd, buf, (size_t)n);
            if (w != n) {
                /* Log file's underlying storage stopped accepting writes
                 * (SD card reset/detached) — remount and reopen so the
                 * drain can keep going instead of silently dying here. */
                close(lfd);
                umount2("/mnt/sdcard", MNT_DETACH);
                if (mount_sd_storage() == 0) {
                    lfd = open(log_file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
                    if (lfd >= 0) {
                        const char note[] = "[kmsg_drain: write failed, recovered via remount]\n";
                        write(lfd, note, sizeof(note) - 1);
                    }
                }
                if (lfd < 0)
                    _exit(1);
            }
            dirty = 1;
        } else {
            /* EAGAIN = ring drained; sleep briefly then retry. */
            usleep(200000);  /* 200ms */
        }
        if (dirty && time(NULL) - last_sync >= 2) {
            fsync(lfd);
            dirty = 0;
            last_sync = time(NULL);
        }
    }
}

static void start_kmsg_drain(void)
{
    pid_t p = fork();
    if (p == 0) {
        kmsg_drain_child();
        _exit(0);
    }
}

/* ------------------------------------------------------------------ */
/* Snapshot the klog ring into the log file at boot                    */
/* ------------------------------------------------------------------ */

static void snapshot_klog_ring(void)
{
    char buf[65536];
    ssize_t n;

    if (!log_file[0])
        return;
    n = klogctl(3, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    append_text("--- klog ring at boot ---\n");
    append_text(buf);
    append_text("--- end klog ---\n");
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void plog_init(void)
{
    /* Wait longer for MMC — SD card can take up to 8s to enumerate. */
    blockdev_wait_mmc(8);
    blockdev_ensure_by_name();
    blockdev_ensure_devnodes();

    if (mount_sd_storage() == 0)
        setup_log_paths("/mnt/sdcard");
    else if (mount_persist_storage() == 0)
        setup_log_paths("/mnt/persist");
    else {
        klog("plog: no writable storage (SD or persist)");
        return;
    }

    boot_seq = read_boot_seq();
    write_boot_marker();
    snapshot_klog_ring();
    /* Grab any pstore/ramoops content left by a PREVIOUS boot's crash
     * before anything else touches the log — if the ~80-85s USB-drop
     * event was actually a kernel oops/panic, this is the one place that
     * would survive it (pstore is backed by reserved DRAM the reboot
     * doesn't clear), independent of whether our own SD writes or COM
     * link were still alive at the time it happened. */
    plog_save_pstore();
    start_kmsg_drain();
    klogf("plog: logging to", log_file);
}

void plog_poll(void)
{
    static unsigned last_hb;
    static unsigned last_save;
    unsigned now = (unsigned)(time(NULL) & 0x7fffffff);
    char line[192];
    char subsys[32] = "?";
    int fd;

    if (!log_file[0])
        return;

    /* Heartbeat every 15s */
    if (now - last_hb >= 15) {
        last_hb = now;

        fd = open("/sys/bus/msm_subsys/devices/subsys0/state", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, subsys, sizeof(subsys) - 1);
            close(fd);
            if (n > 0) {
                subsys[n] = '\0';
                char *nl = strchr(subsys, '\n');
                if (nl)
                    *nl = '\0';
            }
        }

        /* Read radio status if available */
        char wst[64] = "";
        int sfd = open("/tmp/radio.status", O_RDONLY);
        if (sfd >= 0) {
            ssize_t r = read(sfd, wst, sizeof(wst) - 1);
            close(sfd);
            if (r > 0) {
                wst[r] = '\0';
                char *nl = strchr(wst, '\n');
                if (nl) *nl = '\0';
            }
        }

        snprintf(line, sizeof(line),
                 "[hb boot=%u msm_dis=%d subsys=%s radio=%s]\n",
                 boot_seq, wdt_msm_disabled, subsys,
                 wst[0] ? wst : "?");
        append_text(line);

        /* Global sync() every 15s, not just fsync() on the log fd.
         * Confirmed necessary this session: even with per-write fsync()
         * already in place (append_text() and the kmsg drain both do
         * this), live COM checks during WiFi bring-up were repeatedly
         * ahead of what a chkdsk-recovered boot.log showed for the exact
         * same boot, cutting off at a similar real-elapsed-time point
         * every time regardless of what code was running — consistent
         * with the FAT directory entry's file-size field lagging behind
         * the actual data fsync, so a hard SD pull rolls the visible file
         * back to the last time that metadata itself was committed. A
         * periodic full sync() (independent of whether a COM session is
         * even alive to run the manual `sync` command) is the automatic
         * fix — data can still be lost between syncs, but no longer
         * silently for however long a test happens to run. */
        sync();
    }

    /* Copy tmp logs to SD every 60s */
    if (now - last_save >= 60) {
        last_save = now;
        plog_save_tmp_logs();
    }
}

void plog_append(const char *s)
{
    if (!s || !s[0])
        return;
    append_text(s);
    /* Ensure newline */
    if (s[strlen(s) - 1] != '\n')
        append_text("\n");
}

/*
 * plog_save_tmp_logs — copy runtime log files from /tmp into the persistent
 * log directory on SD/persist.  Called after radio bringup and periodically.
 */
void plog_save_tmp_logs(void)
{
    static const char *srcs[] = {
        "/tmp/radio.log",
        "/tmp/cnss.exec.log",
        "/tmp/qrtr-ns.log",
        "/tmp/pd-mapper.log",
        "/tmp/radio.status",
        "/tmp/wifi-scan.txt",
        "/tmp/km.txt",
        "/tmp/hwservicemanager.log",
        "/tmp/servicemanager.log",
        "/tmp/vndservicemanager.log",
        NULL
    };
    char dst[256];
    char buf[8192];
    ssize_t n;

    if (!log_dir[0])
        return;

    for (int i = 0; srcs[i]; i++) {
        /* Only copy if the source exists and is non-empty */
        int fds = open(srcs[i], O_RDONLY);
        if (fds < 0)
            continue;

        /* Derive destination name from source basename */
        const char *base = strrchr(srcs[i], '/');
        base = base ? base + 1 : srcs[i];
        snprintf(dst, sizeof(dst), "%s/%s", log_dir, base);

        int fdd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fdd < 0) {
            close(fds);
            continue;
        }
        while ((n = read(fds, buf, sizeof(buf))) > 0)
            write(fdd, buf, (size_t)n);
        close(fds);
        close(fdd);
    }

    /* Also append a separator to the main boot log */
    {
        char sep[128];
        snprintf(sep, sizeof(sep),
                 "\n--- plog_save_tmp_logs at boot=%u ---\n", boot_seq);
        append_text(sep);
    }
}

void plog_save_pstore(void)
{
    DIR *d;
    struct dirent *e;
    char src[320], dst[320], buf[4096];
    ssize_t n;

    if (!log_dir[0])
        return;
    d = opendir("/sys/fs/pstore");
    if (!d)
        return;

    snprintf(dst, sizeof(dst), "%s/pstore", log_dir);
    mkdir(dst, 0755);

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(src, sizeof(src), "/sys/fs/pstore/%s", e->d_name);
        snprintf(dst, sizeof(dst), "%s/pstore/%s", log_dir, e->d_name);
        int fds = open(src, O_RDONLY);
        if (fds < 0)
            continue;
        int fdd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fdd < 0) {
            close(fds);
            continue;
        }
        while ((n = read(fds, buf, sizeof(buf))) > 0)
            write(fdd, buf, (size_t)n);
        close(fds);
        close(fdd);

        /* Unlink the pstore/ramoops record once it's safely copied to SD.
         * Without this, a real panic captured here (e.g. the modem-crash
         * subsys-restart panic found via this exact mechanism) sits in the
         * ramoops backend forever and every later boot's plog_save_pstore()
         * just re-copies the same stale record — so a NEW crash either has
         * no free slot to land in, or we can no longer tell a fresh crash
         * from old data once copied. Removing the pstore file is the
         * documented way to free that zone for the next real crash. */
        unlink(src);
    }
    closedir(d);
    append_text("--- pstore saved (and cleared) ---\n");
}

const char *plog_path(void)
{
    return log_file[0] ? log_file : "";
}

int plog_format_tail(char *buf, size_t bufsz, size_t max_bytes)
{
    int fd;
    ssize_t n;
    off_t pos;
    char *boot;
    size_t show;

    if (!log_file[0] || !buf || bufsz < 64)
        return 0;
    fd = open(log_file, O_RDONLY);
    if (fd < 0)
        return snprintf(buf, bufsz, "plog: no file %s\r\n", log_file);

    pos = lseek(fd, 0, SEEK_END);
    if (pos > (off_t)max_bytes)
        lseek(fd, pos - (off_t)max_bytes, SEEK_SET);
    else
        lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0)
        return snprintf(buf, bufsz, "plog: empty %s\r\n", log_file);
    buf[n] = '\0';

    boot = strstr(buf, "======== BOOT");
    if (boot) {
        show = (size_t)(n - (boot - buf));
        if (show > bufsz - 32)
            show = bufsz - 32;
        memcpy(buf, boot, show);
        buf[show] = '\0';
        n = (ssize_t)show;
    }
    ssize_t extra = (ssize_t)snprintf(buf + n, bufsz - (size_t)n,
                                      "\r\n--- path=%s ---\r\n", log_file);
    return (int)(n + extra);
}
