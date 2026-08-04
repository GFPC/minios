#include <sys/sysmacros.h>
#define _GNU_SOURCE
#include "wlan.h"
#include "radio.h"
#include "radio_state.h"
#include "radio_utils.h"
#include "cnss.h"
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
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <poll.h>
#include <net/if.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <linux/capability.h>

#define WLAN_FWPATH_MAX 18
void rfkill_unblock_all(void)
{
    DIR *d = opendir("/sys/class/rfkill");
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        {
            char path[384];
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/state", e->d_name);
            if (access(path, W_OK) == 0)
                wf(path, "1");
        }
    }
    closedir(d);
}


void walk_wcnss_in_dir(const char *parent)
{
    DIR *d = opendir(parent);
    struct dirent *e;
    struct stat st;
    char path[384];

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", parent, e->d_name);
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        wf_path_join(path, "wcnss_wlan");
    }
    closedir(d);
}


void try_bind_icnss_driver(const char *drv)
{
    char bind[128];
    DIR *d;
    struct dirent *e;

    snprintf(bind, sizeof(bind), "/sys/bus/platform/drivers/%s/bind", drv);
    if (access(bind, W_OK) != 0)
        return;

    d = opendir("/sys/devices/platform/soc");
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (strstr(e->d_name, "icnss") || strstr(e->d_name, "wcnss"))
            wf(bind, e->d_name);
    }
    closedir(d);
}


void modprobe_one(const char *mod)
{
    pid_t p = fork();
    if (p != 0) {
        if (p > 0)
            waitpid(p, NULL, 0);
        return;
    }
    radio_child_setup();
    execl("/sbin/modprobe", "modprobe", mod, NULL);
    execl("/bin/modprobe", "modprobe", mod, NULL);
    _exit(0);
}


int icnss_fw_ready_check(void)
{
    char buf[32] = "";
    int fd = open("/sys/kernel/debug/icnss/state", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0)
        return 0;
    buf[r] = '\0';
    unsigned long st = strtoul(buf, NULL, 16);
    return (st & (1UL << 2)) ? 1 : 0; /* ICNSS_FW_READY = bit 2 */
}


void wait_icnss_fw_ready(int max_sec)
{
    int got = 0;
    for (int i = 0; i < max_sec * 2; i++) {
        if (icnss_fw_ready_check()) {
            got = 1;
            break;
        }
        usleep(500000);
    }
    if (got)
        LOGI("radio", "%s", "wlan: ICNSS_FW_READY detected via debugfs");
    else
        LOGI("radio", "%s", "wlan: FW_READY wait done (no debugfs)");
}


void configure_wlan_driver(void)
{
    const char *fwpath = pick_wlan_fwpath();
    char cur[16] = "";
    int fd;

    /* Un-shutdown WLAN chip: write 0 to remove hardware shutdown. */
    if (access("/sys/kernel/shutdown_wlan/shutdown", F_OK) == 0) {
        int rc = wf_checked("/sys/kernel/shutdown_wlan/shutdown", "0");
        LOGI("radio", "%s %s", "wlan: shutdown_wlan/shutdown=0", rc == 0 ? "ok" : "err(ok)");
    }

    /* fwpath sysfs accepts at most 18 chars on willow. */
    if (fwpath) {
        if (wf_checked("/sys/module/wlan/parameters/fwpath", fwpath) == 0)
            LOGI("radio", "%s %s", "wlan fwpath", fwpath);
        else
            LOGI("radio", "%s", "wlan fwpath write failed");
    } else {
        LOGI("radio", "%s", "wlan fwpath: no valid short path");
    }

    fd = open("/sys/module/wlan/parameters/con_mode", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, cur, sizeof(cur) - 1);
        close(fd);
        if (n > 0) {
            cur[n] = '\0';
            char *nl = strchr(cur, '\n');
            if (nl)
                *nl = '\0';
        }
    }
    if (cur[0] && strcmp(cur, "0"))
        wf_checked("/sys/module/wlan/parameters/con_mode", "0");
    usleep(200000);
    wf_checked("/sys/module/wlan/parameters/con_mode", "0");

    /* Create /dev/wlan and write "ON" to trigger hdd_driver_load() in the
     * QCA CLD3 driver.
     *
     * The wlan_hdd_state control device is registered via
     * alloc_chrdev_region() — a DYNAMIC major, not a fixed one. The old
     * hardcoded major=500 here (carried over from legacy prima/wcnss_wlan
     * drivers) was confirmed live to mismatch the real major (kernel logs
     * "wlan_hdd_state wlan major(501) initialized"), so every previous
     * open()+write("ON") on this node silently went to a bogus device —
     * mknod() itself doesn't fail, so this broke with zero error output.
     * Look the real major up in /proc/devices instead, and always
     * re-create the node if a stale one exists with the wrong major. */
    {
        /* Registered name in /proc/devices is "qcwlanstate", NOT "wlan" —
         * confirmed by reading the actual driver source
         * (kernel/drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_main.c,
         * wlan_hdd_state_ctrl_param_create(): alloc_chrdev_region(&device,
         * 0, dev_num, "qcwlanstate")). The kernel's own boot-time log line
         * ("wlan_hdd_state wlan major(%d) initialized") is misleadingly
         * named and does NOT reflect the /proc/devices key. Looking up
         * "wlan" here therefore never matched, always fell through to the
         * broken legacy major=500 fallback below, and every /dev/wlan
         * open() this project has ever done failed with ENXIO — silently,
         * since mknod() itself doesn't care whether the major is real.
         * Confirmed live this session via a new plog breadcrumb: "wlan:
         * /dev/wlan major=500 need_mknod=1" immediately followed by "wlan:
         * /dev/wlan open errno=6 attempt=0" (ENXIO), while the real major
         * was 501. This one-line fix may be the actual root cause behind
         * the whole project's "modem panics ~40s after reset" mystery —
         * hdd_driver_load()/wlan_hdd_register_driver() (qcacld's own WLAN
         * bring-up entry point) can never have run even once before this,
         * since the "ON" write that triggers it never reached the real
         * device. */
        int wlan_major = get_chrdev_major("qcwlanstate");
        struct stat st;
        int need_mknod = 1;

        if (wlan_major < 0) {
            LOGI("radio", "%s", "wlan: /proc/devices has no 'wlan' entry yet, falling back to major=500");
            wlan_major = 500;
        } else {
            char m[64];
            snprintf(m, sizeof(m), "wlan: /proc/devices wlan major=%d", wlan_major);
            LOGI("radio", "%s", m);
        }

        if (stat("/dev/wlan", &st) == 0) {
            if (S_ISCHR(st.st_mode) && (int)major(st.st_rdev) == wlan_major) {
                need_mknod = 0;
            } else {
                unlink("/dev/wlan");
            }
        }

        if (need_mknod) {
            if (mknod("/dev/wlan", S_IFCHR | 0666, makedev(wlan_major, 0)) == 0) {
                LOGI("radio", "%s", "wlan: /dev/wlan mknod ok");
            } else {
                char m[80];
                snprintf(m, sizeof(m), "wlan: mknod errno=%d", errno);
                LOGI("radio", "%s", m);
                plog_append(m);
            }
        }
        {
            char m[96];
            snprintf(m, sizeof(m), "wlan: /dev/wlan major=%d need_mknod=%d", wlan_major, need_mknod);
            plog_append(m);
        }
    }

    /* Retry ON up to 3 times; re-poll ICNSS_FW_READY before each attempt.
     * If FW_READY is set the write succeeds and wlan_start_comp fires within
     * 20s.  If still not ready, wait up to 20s more and retry. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            LOGI("radio", "%s", "wlan: /dev/wlan retry ON — waiting for FW_READY (20s)...");
            /* Re-poll FW_READY — may have just arrived */
            for (int w = 0; w < 40; w++) {
                if (icnss_fw_ready_check()) {
                    LOGI("radio", "%s", "wlan: FW_READY detected on retry");
                    break;
                }
                usleep(500000);
            }
        }
        fd = open("/dev/wlan", O_WRONLY);
        if (fd < 0) {
            char m[80];
            snprintf(m, sizeof(m), "wlan: /dev/wlan open errno=%d attempt=%d", errno, attempt);
            LOGI("radio", "%s", m);
            plog_append(m);
            continue;
        }
        /* wlan_hdd_state_ctrl_param_write() (kernel) unconditionally does
         * copy_from_user(buf, user_buf, 3) — a hardcoded 3, ignoring the
         * actual write() count — so it always tries to read a 3rd byte
         * past whatever we send. Sending only "ON" (2 bytes) makes that a
         * 1-byte user-space overread of our own buffer: harmless if the
         * adjacent byte happens to be mapped (usual case on a stack), but
         * copy_from_user() returning -EFAULT there would make the kernel
         * bail out with -EINVAL before ever reaching hdd_driver_load()/
         * wlan_hdd_register_driver() — a silent, non-obvious way for the
         * whole WiFi bring-up to never start. Pad to a real 3-byte buffer
         * so the kernel's read is always fully in-bounds. */
        ssize_t n = write(fd, "ON\0", 3);
        {
            int werrno = errno;
            close(fd);
            if (n >= 2) {
                LOGI("radio", "%s", "wlan: /dev/wlan=ON sent");
                plog_append("wlan: /dev/wlan=ON sent");
                break; /* write accepted — wait for wlan0 in caller */
            }
            {
                char m[96];
                snprintf(m, sizeof(m), "wlan: /dev/wlan write failed n=%d errno=%d attempt=%d",
                         (int)n, werrno, attempt);
                LOGI("radio", "%s", m);
                plog_append(m);
            }
        }
    }
}


/* Real QRTR service-lookup, replacing a check that could never have worked:
 * this kernel's net/qrtr/qrtr.c has no proc_create()/debugfs anywhere in it
 * (confirmed by reading the actual kernel source used for this build), so
 * /proc/net/qrtr never exists and the old qrtr_has_wlfw() (reading that
 * path) was guaranteed to return 0 regardless of whether wlfw had actually
 * registered — every prior "wlfw=0" observation was meaningless. This asks
 * the real local qrtr-ns (address {qrtr_local_nid=1, QRTR_PORT_CTRL},
 * confirmed against how the kernel itself addresses the local ns for its
 * own hello/bye control traffic) via a real NEW_LOOKUP control packet over
 * a raw AF_QIPCRTR socket and reads back its NEW_SERVER reply — validated
 * live via the standalone tools/qrtr_lookup.c before folding in here. */
#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_LOCAL_NID 1
#define QRTR_PORT_CTRL_ 0xfffffffeu

struct qrtr_sockaddr_ {
    unsigned short sq_family;
    unsigned short pad0;
    uint32_t sq_node;
    uint32_t sq_port;
};

struct qrtr_ctrl_pkt_ {
    uint32_t cmd;
    union {
        struct { uint32_t service; uint32_t instance; uint32_t node; uint32_t port; } server;
        struct { uint32_t node; uint32_t port; } client;
    };
};

/* One-shot check for the "icnss-state" diagnostic command — kept separate
 * from wait_for_wlfw()'s long-lived socket (see REAL BUG #4 below) since
 * this is called at most a couple of times per manual diagnostic command,
 * not up to 80 times in a tight loop, so the same-suspected socket-churn
 * issue shouldn't apply here. Bounded to ~600ms so it never noticeably
 * delays the command's response. */
int qrtr_has_wlfw(void)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return 0;
    struct qrtr_sockaddr_ dst;
    memset(&dst, 0, sizeof(dst));
    dst.sq_family = AF_QIPCRTR;
    dst.sq_node = QRTR_LOCAL_NID;
    dst.sq_port = QRTR_PORT_CTRL_;
    struct qrtr_ctrl_pkt_ req;
    memset(&req, 0, sizeof(req));
    req.cmd = 10; /* QRTR_TYPE_NEW_LOOKUP */
    req.server.service = 0x45;
    sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst));

    int found = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    for (int i = 0; i < 3 && poll(&pfd, 1, 200) > 0; i++) {
        unsigned char buf[256];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(struct qrtr_ctrl_pkt_))
            break;
        struct qrtr_ctrl_pkt_ *pkt = (struct qrtr_ctrl_pkt_ *)buf;
        if (pkt->cmd == 4 && pkt->server.service == 0x45) {
            found = 1;
            break;
        }
    }
    struct qrtr_ctrl_pkt_ del;
    memset(&del, 0, sizeof(del));
    del.cmd = 11; /* QRTR_TYPE_DEL_LOOKUP */
    del.server.service = 0x45;
    sendto(fd, &del, sizeof(del), 0, (struct sockaddr *)&dst, sizeof(dst));
    close(fd);
    return found;
}

/* REAL BUG #4 (found after #1/#2/#3 each reproduced the *identical*,
 * byte-for-byte hang at i=18/80 regardless of internal socket-handling
 * changes — proving the bug wasn't in any of those internals at all):
 * the old design opened a brand-new AF_QIPCRTR socket, subscribed, drained,
 * unsubscribed, and closed it on *every single call*, i.e. up to 80 times
 * across one wait_for_wlfw(). Consistently stalling around the same call
 * count points at kernel-side qrtr port/socket churn (rapid bind/close
 * cycles on this custom protocol family) rather than a userspace logic
 * bug — every rewrite of the read/timeout logic hit the identical wall.
 * Fix: create the socket and subscribe exactly ONCE per wait_for_wlfw()
 * call, then just keep polling/reading that SAME socket for the whole
 * wait window. This is also simply the correct way to use NEW_LOOKUP
 * (see the now-removed per-call comment: it's a subscription, meant to be
 * held open and drained, not repeatedly re-established). */
int wait_for_wlfw(int max_sec)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        plog_append("wlan: wlfw socket() failed");
        return 0;
    }

    struct qrtr_sockaddr_ dst;
    memset(&dst, 0, sizeof(dst));
    dst.sq_family = AF_QIPCRTR;
    dst.sq_node = QRTR_LOCAL_NID;
    dst.sq_port = QRTR_PORT_CTRL_;

    struct qrtr_ctrl_pkt_ req;
    memset(&req, 0, sizeof(req));
    req.cmd = 10; /* QRTR_TYPE_NEW_LOOKUP */
    req.server.service = 0x45; /* wlfw */
    sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst));

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += max_sec;

    int found = 0;
    int polls = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                         (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remain_ms <= 0)
            break;
        if (polls % 20 == 0) {
            char line[48];
            snprintf(line, sizeof(line), "wlan: wlfw poll remain_ms=%ld", remain_ms);
            plog_append(line);
            /* MEMORY.md §4.3.5: check whether hwservicemanager died at any
             * point during this wait, not just once before it started —
             * this is exactly the window pm-service's binder transactions
             * have been observed failing throughout. */
            check_hwservicemanager_late_exit();
        }
        polls++;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, remain_ms < 500 ? (int)remain_ms : 500);
        if (pr < 0)
            break;
        if (pr == 0)
            continue; /* just our own 500ms tick, not the real deadline yet */

        unsigned char buf[256];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(struct qrtr_ctrl_pkt_))
            continue;
        struct qrtr_ctrl_pkt_ *pkt = (struct qrtr_ctrl_pkt_ *)buf;
        if (pkt->cmd == 4 /* NEW_SERVER */ && pkt->server.service == 0x45) {
            found = 1;
            break;
        }
        /* not our service — keep draining on the same socket */
    }

    struct qrtr_ctrl_pkt_ del;
    memset(&del, 0, sizeof(del));
    del.cmd = 11; /* QRTR_TYPE_DEL_LOOKUP */
    del.server.service = 0x45;
    sendto(fd, &del, sizeof(del), 0, (struct sockaddr *)&dst, sizeof(dst));
    close(fd);

    if (found) {
        LOGI("radio", "%s", "wlan: wlfw (0x45) seen in QRTR");
        plog_append("wlan: wlfw (0x45) seen in QRTR");
    } else {
        LOGI("radio", "%s", "wlan: wlfw not in QRTR after wait");
        plog_append("wlan: wlfw not in QRTR after wait (loop completed)");
    }
    return found;
}


void trigger_wlan_power(void)
{
    const char *triggers[] = {
        "/sys/kernel/boot_wlan/boot_wlan",
        NULL
    };

    for (int i = 0; triggers[i]; i++) {
        if (access(triggers[i], W_OK) == 0)
            wf(triggers[i], "1");
    }

    walk_wcnss_in_dir("/sys/devices/platform/soc");
    walk_wcnss_in_dir("/sys/bus/platform/drivers/icnss");
    walk_wcnss_in_dir("/sys/bus/platform/drivers/icnss2");
    try_bind_icnss_driver("icnss");
    try_bind_icnss_driver("icnss2");
}


void iface_up(const char *ifname)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    if (fd < 0)
        return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP;
        ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    close(fd);
}


void wait_for_iface(const char *sys_path, int sec)
{
    for (int i = 0; i < sec * 2; i++) {
        if (path_exists(sys_path))
            return;
        usleep(500000);
    }
}


void try_wlan_enable(void)
{
    if (path_exists("/sys/class/net/wlan0")) {
        iface_up("wlan0");
        snprintf(wifi_st, sizeof(wifi_st), "WiFi: wlan0 already up");
        LOGI("radio", "%s", "wlan0 already up");
        return;
    }

    /* plog_append() writes straight to the SD-card log file (with fsync,
     * per plog.c) instead of going through /dev/kmsg -> the drain child ->
     * SD. It's the only channel confirmed to survive intact: kmsg-sourced
     * boot.log content has repeatedly stopped right around this point on
     * SD even when the live COM session clearly kept progressing further
     * (confirmed live: qrtr=1 pdmap=1 daemon=1 at t=30s while the recovered
     * on-disk log only showed qrtr=0 pdmap=0 daemon=0 and cut off at
     * "wlfw_start: Starting") — likely chkdsk rolling the FAT directory
     * entry's file-size metadata back to the last point it was updated,
     * even though fsync'd data landed on the card. These checkpoints are
     * cheap enough to persist this way without the same risk. */
    plog_append("wlan: start_cnss_stack()");
    radio_trace("wlan: start_cnss_stack");
    start_cnss_stack();
    radio_dump_qrtr("post start_cnss_stack");

    /* CRITICAL ORDERING FIX (see MEMORY.md #4.5au): the OLD order here was
     * cnss-daemon-wait -> wait_for_wlfw(40) -> wait_icnss_fw_ready(35) ->
     * trigger_wlan_power()/configure_wlan_driver(). That is backwards and
     * created a real deadlock, confirmed live via a captured pstore panic +
     * a byte-exact qrtr-snoop trace: icnss_probe() only registers the
     * platform-driver shell at kernel boot ("icnss: Platform driver probed
     * successfully" at t=0.99s in a captured dmesg) — it does NOT touch the
     * WCN3990 hardware or register the wlfw QMI service on its own. Nothing
     * else in the kernel does either: actually powering the chip (PCIe/
     * memory-mapped enumeration + wlfw registration) only happens as a
     * *consequence* of hdd_driver_load(), which only runs once userspace
     * writes "ON" to /dev/wlan via configure_wlan_driver(). So wlfw can
     * structurally never appear during wait_for_wlfw(40) — that whole 40s
     * is guaranteed wasted, every single boot (confirmed: wlfw=0 for the
     * full 40s in every "modem_watch" trace all night, no exceptions).
     * Worse: the modem itself panics ("Fatal error on modem!") almost
     * exactly 40.05s after "Brought out of reset" in the one boot where a
     * pstore panic dump was actually captured — its own last QMI activity
     * before that silent 40s gap was the tms/pdr_enabled + wlan_pd
     * REGISTER_LISTENER exchange, right at reset. The modem is very likely
     * running its own bailout timer waiting for the AP to bring up WLFW/
     * WCN3990 within that window — and our code doesn't even attempt to
     * power the chip until ~75s+ in (40s + 35s of guaranteed-empty waits
     * later), well past the modem's own deadline.
     *
     * Fix: power the chip and write ON to /dev/wlan immediately after
     * start_cnss_stack() returns (i.e. right after boot_modem() has
     * already triggered modem PIL) — BEFORE any of the wlfw/fw_ready
     * waits, not after. All the sysfs paths configure_wlan_driver() and
     * trigger_wlan_power() touch belong to the qcacld "wlan" module, which
     * is built directly into this kernel (minios.skip_ko=1) — its sysfs
     * parameter files and /proc/devices chrdev entry exist from kernel
     * boot, not from any userspace daemon, so calling this earlier is
     * safe and has no other new dependency. */
    /* UPDATE (this session, live usb_ctl.py history analysis): triggering
     * RF power at 0s right here (immediately after start_cnss_stack()
     * returns) was tried and made things WORSE, not better. Confirmed via
     * usb_ctl.py's passive Windows-side USB connect/disconnect history
     * (untouched by any COM traffic, so not an artifact of our own
     * polling): the SoC reboot cycle became a rock-solid, reproducible
     * ~121s from USB enumeration every single time (vs ~178s / "40s after
     * modem out-of-reset" before this change) -- i.e. the crash now
     * happens only ~16s after "Brought out of reset" (dmesg-find
     * consistently shows t~=105.3-105.5s boot time across many boots)
     * instead of ~40s after. "Brought out of reset" only means the PIL
     * hardware reset line was released -- the modem's own MPSS
     * firmware/software stack is not necessarily fully up yet at that
     * instant. Racing configure_wlan_driver()'s hardware bring-up (WCN3990
     * is memory-mapped on this SoC/DTB, not PCIe -- see trinket.dtsi
     * qcom,icnss@C800000) at that exact moment most likely collides with
     * the modem's own in-progress boot (shared clocks/rails/bus),
     * producing a NEW, faster crash instead of fixing the old one. pstore
     * came up completely empty both times (no ramoops files at all after
     * either crash), so we have no captured backtrace for either failure
     * mode -- only the precise, reproducible timing from usb_ctl.py.
     *
     * Fix (this iteration): give the modem a bounded ~20s head start after
     * boot_modem() before touching WLAN hardware -- long enough to be past
     * the immediate post-reset window that reproducibly crashed at ~16s,
     * nowhere near the old 75s+ (cnss-daemon-wait + wlfw 40s + fw_ready
     * 35s) that produced the original ~178s/40s-after-reset panic. Still
     * an empirical guess, not a confirmed root cause -- if it neither
     * prevents nor meaningfully delays the crash again, next step should
     * be getting pstore to actually capture a backtrace (console-ramoops,
     * bigger ramoops DTB region) instead of guessing another delay blind. */
    plog_append("wlan: post-boot_modem settle wait (~20s, bounded)");
    for (int i = 0; i < 40; i++) {
        wdt_pet();
        usleep(500000);
    }
    plog_append("wlan: settle wait done");

    LOGI("radio", "%s", "wlan: RF power on (post-settle, ~20s after boot_modem)");
    plog_append("wlan: RF power on (trigger_wlan_power, post-settle)");
    trigger_wlan_power();

    plog_append("wlan: configure_wlan_driver() (post-settle)");
    configure_wlan_driver();

    /* Wait for cnss-daemon to be alive (up to 90s).
     * Extended from 60s — vendor mount + qrtr-ns start can be slow. */
    for (int i = 0; i < 180 && !proc_running("cnss-daemon") &&
             !proc_cmdline_has("cnss-daemon"); i++) {
        wdt_pet();
        if (i == 0 || (i % 20) == 0)
            LOGI("radio", "%s %s", "cnss wait", proc_running("qrtr-ns") ? "qrtr=1" : "qrtr=0");
        usleep(500000);
    }
    {
        char line[128];
        snprintf(line, sizeof(line),
                 "wlan: cnss-daemon alive=%d qrtr=%d pdmap=%d",
                 proc_running("cnss-daemon") || proc_cmdline_has("cnss-daemon"),
                 proc_running("qrtr-ns"), proc_running("pd-mapper"));
        plog_append(line);
    }

    LOGI("radio", "%s", "wlan: waiting for wlfw in QRTR (max 40s)...");
    plog_append("wlan: waiting for wlfw in QRTR (max 40s)");
    int have_wlfw = wait_for_wlfw(40);
    radio_dump_qrtr("post wlfw wait");
    {
        char line[64];
        snprintf(line, sizeof(line), "wlan: wlfw_seen=%d", have_wlfw);
        plog_append(line);
    }

    LOGI("radio", "%s", "wlan: waiting for ICNSS_FW_READY (35s max)...");
    plog_append("wlan: waiting for ICNSS_FW_READY (35s max)");
    wait_icnss_fw_ready(35);
    {
        char line[64];
        snprintf(line, sizeof(line), "wlan: fw_ready=%d", icnss_fw_ready_check());
        plog_append(line);
    }

    plog_append("wlan: waiting for wlan0 iface (90s max)");
    wait_for_iface("/sys/class/net/wlan0", 90);
    {
        char line[64];
        snprintf(line, sizeof(line), "wlan: wlan0_present=%d",
                 path_exists("/sys/class/net/wlan0"));
        plog_append(line);
    }

    if (path_exists("/sys/class/net/wlan0")) {
        iface_up("wlan0");
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
        LOGI("radio", "%s", "wlan0 up");
        write_radio_log_append();
        /* Persist all runtime logs to SD now that WiFi is confirmed up */
        plog_save_tmp_logs();
        return;
    }

    if (has_wlan_firmware()) {
        if (path_exists("/sys/class/net/wlan0"))
            return;
        if (cnss_stack_running())
            snprintf(wifi_st, sizeof(wifi_st), "WiFi: cnss ok no iface");
        else if (vendor_bin("cnss-daemon"))
            snprintf(wifi_st, sizeof(wifi_st), "WiFi: cnss start fail");
        else
            snprintf(wifi_st, sizeof(wifi_st), vendor_mounted ?
                     "WiFi: fw ok no iface" : "WiFi: fw bundled no iface");
    } else if (vendor_mounted)
        snprintf(wifi_st, sizeof(wifi_st), "WiFi: vendor no wlan fw");
    else
        snprintf(wifi_st, sizeof(wifi_st), "WiFi: need vendor mount");
    write_radio_log_append();
}


