#define _GNU_SOURCE
#include "radio.h"
#include "minios/log.h"
#include "minios/watchdog.h"
#include "minios/plog.h"
#include "radio_utils.h"
#include "modem.h"
#include "cnss.h"
#include "wlan.h"
#include "bt.h"
#include "firmware.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/capability.h>
#include <sys/syscall.h>

#ifndef SYS_capset
#define SYS_capset 90
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
#ifndef MODULE_INIT_IGNORE_VERMAGIC
#define MODULE_INIT_IGNORE_VERMAGIC 1
#endif
#ifndef MODULE_INIT_IGNORE_MODVERSIONS
#define MODULE_INIT_IGNORE_MODVERSIONS 2
#endif

#define CNSS_EXEC_LOG "/tmp/cnss.exec.log"
#define CNSS_BUILD_TAG "cnss-start v41-modem-partition"
#define WLAN_FWPATH_MAX 18

#define BT_CMD_PWR_CTRL 0xbfad

char wifi_st[80] = "WiFi: idle";
char bt_st[80] = "BT: idle";
int vendor_mounted;
int system_mounted;
int persist_mounted;
int modem_mounted;
int bt_fw_mounted;
pid_t radio_pid;
pid_t scan_pid;
pid_t cnss_qrtr_pid;
pid_t cnss_pdmap_pid;
pid_t cnss_daemon_pid;
int radio_job_bt;

static void write_radio_log_mode(int append);






/* Stock adsp-loader: sscanf "%du" → "1u"; patched kernel: "%d" → "1". Try both. */













const char *pick_wlan_fwpath(void)
{
    static const char *candidates[] = {
        "/vendor/etc/wifi",
        "/data/misc/wifi",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        char ini[256];

        if (strlen(candidates[i]) > WLAN_FWPATH_MAX)
            continue;
        if (strcmp(candidates[i], "/data/misc/wifi") == 0)
            ensure_wifi_config();
        snprintf(ini, sizeof(ini), "%s/WCNSS_qcom_cfg.ini", candidates[i]);
        if (path_exists(ini))
            return candidates[i];
    }
    return NULL;
}

/* Try to read ICNSS_FW_READY bit (bit 2 = 0x4) from debugfs state.
 * Returns 1 if confirmed ready, 0 if not readable / not set. */

/* Wait up to max_sec for ICNSS_FW_READY.  Falls through whether or not
 * the bit is detected (debugfs may not have the entry). */






















const char *vendor_bin(const char *name)
{
    static char paths[5][128];
    static int idx;
    static const char *prefixes[] = {
        "/mnt/vendor/bin/",
        "/vendor/bin/",
        "/vendor/bin/hw/",
        "/system/bin/",
        "/system/vendor/bin/",
        "/sbin/",
        NULL
    };

    for (int i = 0; prefixes[i]; i++) {
        idx = (idx + 1) % 5;
        snprintf(paths[idx], sizeof(paths[idx]), "%s%s", prefixes[i], name);
        if (path_exists(paths[idx]))
            return paths[idx];
    }
    return NULL;
}




const char *stage_cnss_daemon(const char *src)
{
    static char staged[128];
    char msg[256];
    int rc;

    if (!src || !path_exists(src))
        return src;
    snprintf(staged, sizeof(staged), "/tmp/cnss-daemon");
    unlink(staged);
    rc = copy_file_bin(src, staged);
    snprintf(msg, sizeof(msg), "stage cp %s -> %s rc=%d errno=%d exists=%d",
             src, staged, rc, rc < 0 ? -rc : 0, path_exists(staged));
    cnss_log_line(msg);
    LOGI("radio", "%s", msg);
    return path_exists(staged) ? staged : src;
}












static char staged_bin_path[128];

const char *stage_vendor_bin(const char *name)
{
    const char *src = vendor_bin(name);
    int rc;

    if (!src)
        return NULL;
    if (!strncmp(src, "/vendor/", 8) || !strncmp(src, "/system/", 8)) {
        snprintf(staged_bin_path, sizeof(staged_bin_path), "/tmp/%s", name);
        unlink(staged_bin_path);
        rc = copy_file_bin(src, staged_bin_path);
        if (rc == 0)
            chmod(staged_bin_path, 0755);
        if (path_exists(staged_bin_path))
            return staged_bin_path;
    }
    return src;
}

























/* PIL request_firmware() looks in /lib/firmware/modem.{mdt,bNN}. */










static void write_radio_log(void)
{
    write_radio_log_mode(0);
}

void write_radio_log_append(void)
{
    write_radio_log_mode(1);
}

static void write_radio_log_mode(int append)
{
    int fd = open("/tmp/radio.log", O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0)
        return;
    dprintf(fd, "vendor=%d system=%d persist=%d modem=%d bt_fw=%d wlan_fw=%d\n",
            vendor_mounted, system_mounted, persist_mounted, modem_mounted, bt_fw_mounted,
            has_wlan_firmware());
    dprintf(fd, "cnss qrtr=%d pdmap=%d daemon=%d vndksupport=%d\n",
            proc_running("qrtr-ns"), proc_running("pd-mapper"),
            proc_running("cnss-daemon"), path_exists("/lib64/libvndksupport.so"));
    {
        char diag[2048];
        blockdev_format_diag(diag, sizeof(diag));
        write(fd, diag, strlen(diag));
    }
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

static void radio_prepare(void)
{
    ensure_block_layout();
    rfkill_unblock_all();
    /* Mount debugfs so icnss/state is readable for FW_READY polling. */
    ensure_debugfs();
    mount_radio_partitions();
    link_firmware_tree();
    ensure_rmtfs_firmware_paths();
    write_radio_log();
}






static void radio_work(void)
{
    LOGI("radio", "%s", "bringup start");
    radio_trace("radio_work: begin");
    try_load_qrtr_snoop();
    radio_prepare();
    radio_trace("radio_prepare: mounts+rmtfs paths done");
    /* Start RMTFS serving (rmt_storage/tftp_server) right after mounts are
     * ready, before the cnss stack / modem PIL trigger — real ROM has these
     * listening from very early boot, well before modem PIL; the modem's
     * own software starts polling RMTFS immediately on leaving reset. See
     * start_rmtfs_daemons_early()'s own comment in modem.c. */
    start_rmtfs_daemons_early();
    radio_trace("rmtfs daemons started (pre-cnss)");
    /* Let USB/COM settle before RF power (reduces dwc3 drop). */
    usleep(800000);
    try_wlan_enable();
    radio_dump_qrtr("after try_wlan_enable");
    write_radio_status();

    if (radio_job_bt) {
        usleep(1500000);
        try_bt_enable();
        write_radio_status();
    } else {
        snprintf(bt_st, sizeof(bt_st), "BT: skipped (wifi job)");
    }

    /* Always persist all runtime logs to SD at the end of radio bringup */
    plog_save_tmp_logs();

    radio_trace("radio_work: done");
    LOGI("radio", "%s", "bringup done");
}

static void scan_work(void)
{
    int fd = open("/tmp/wifi-scan.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);

    LOGI("radio", "%s", "scan start");
    if (!path_exists("/sys/class/net/wlan0")) {
        radio_prepare();
        usleep(800000);
        try_wlan_enable();
    }

    if (!path_exists("/sys/class/net/wlan0")) {
        int fd = open("/tmp/wifi-scan.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dprintf(fd, "wlan0 down — check: status (vendor mount + fw)\n");
            close(fd);
        }
        LOGI("radio", "%s", "scan: no wlan0");
        return;
    }

    {
        pid_t p = fork();
        if (p == 0) {
            int out = open("/tmp/wifi-scan.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            radio_child_setup();
            if (out >= 0) {
                dup2(out, 1);
                dup2(out, 2);
                close(out);
            }
            execl("/sbin/wlan_scan", "wlan_scan", NULL);
            _exit(127);
        }
        if (p > 0)
            waitpid(p, NULL, 0);
    }
    LOGI("radio", "%s", "scan done");
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
    radio_child_setup();
    fn();
    _exit(0);
}

static void start_radio_job(int with_bt)
{
    if (pid_alive(radio_pid))
        return;
    if (pid_alive(scan_pid))
        return;
    radio_job_bt = with_bt;
    start_job(&radio_pid, radio_work);
}

void radio_request_wifi_async(void)
{
    start_radio_job(0);
}

void radio_request_async(void)
{
    start_radio_job(1);
}

void radio_init_async(void)
{
    radio_request_wifi_async();
}

void modem_qmi_services_start(void)
{
    start_modem_qmi_services();
}

void radio_probe_now(void)
{
    radio_request_wifi_async();
}

void radio_scan_request_async(void)
{
    if (pid_alive(radio_pid))
        return;
    start_job(&scan_pid, scan_work);
}

static void log_job_exit(const char *name, int st)
{
    char msg[96];

    if (WIFSIGNALED(st))
        snprintf(msg, sizeof(msg), "%s job killed by signal %d", name, WTERMSIG(st));
    else if (WIFEXITED(st))
        snprintf(msg, sizeof(msg), "%s job exited status %d", name, WEXITSTATUS(st));
    else
        snprintf(msg, sizeof(msg), "%s job ended st=0x%x", name, st);
    LOGI("radio", "%s", msg);
}

void radio_poll(void)
{
    int st;
    pid_t p;

    if (radio_pid > 0) {
        p = waitpid(radio_pid, &st, WNOHANG);
        if (p == radio_pid) {
            log_job_exit("radio", st);
            radio_pid = 0;
        }
    }
    if (scan_pid > 0) {
        p = waitpid(scan_pid, &st, WNOHANG);
        if (p == scan_pid) {
            log_job_exit("scan", st);
            scan_pid = 0;
        }
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

int radio_format_fw_list(char *buf, size_t bufsz)
{
    int n = 0;
    DIR *d = opendir("/lib/firmware/wlan/qca_cld");

    n += snprintf(buf + n, bufsz - (size_t)n, "wifi-fw /lib/firmware/wlan/qca_cld\r\n");
    if (!d) {
        n += snprintf(buf + n, bufsz - (size_t)n, "  (missing)\r\n");
        return n;
    }
    struct dirent *e;
    while ((e = readdir(d)) && n < (int)bufsz - 80) {
        char target[256];
        char linkpath[384];
        ssize_t tl;

        if (e->d_name[0] == '.')
            continue;
        snprintf(linkpath, sizeof(linkpath), "/lib/firmware/wlan/qca_cld/%s", e->d_name);
        tl = readlink(linkpath, target, sizeof(target) - 1);
        if (tl > 0) {
            target[tl] = '\0';
            n += snprintf(buf + n, bufsz - (size_t)n, "  %s -> %s\r\n", e->d_name, target);
        } else {
            n += snprintf(buf + n, bufsz - (size_t)n, "  %s\r\n", e->d_name);
        }
    }
    closedir(d);
    return n;
}

static int format_dir_entries(const char *dir, char *buf, size_t bufsz, int n)
{
    DIR *d = opendir(dir);

    n += snprintf(buf + n, bufsz - (size_t)n, "%s:\r\n", dir);
    if (!d) {
        n += snprintf(buf + n, bufsz - (size_t)n, "  (missing)\r\n");
        return n;
    }
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) && n < (int)bufsz - 64 && count < 32) {
        if (e->d_name[0] == '.')
            continue;
        n += snprintf(buf + n, bufsz - (size_t)n, "  %s\r\n", e->d_name);
        count++;
    }
    closedir(d);
    if (count >= 32)
        n += snprintf(buf + n, bufsz - (size_t)n, "  ...\r\n");
    return n;
}

static int find_in_dir(const char *dir, const char *needle, char *buf, size_t bufsz, int n)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return n;
    while ((e = readdir(d)) && n < (int)bufsz - 64) {
        if (e->d_name[0] == '.')
            continue;
        if (strstr(e->d_name, needle))
            n += snprintf(buf + n, bufsz - (size_t)n, "  %s/%s\r\n", dir, e->d_name);
    }
    closedir(d);
    return n;
}

int radio_format_pidinfo(char *buf, size_t bufsz)
{
    int n = snprintf(buf, bufsz, "radio_pid=%d alive=%d\r\n", (int)radio_pid, pid_alive(radio_pid));

    if (radio_pid <= 0)
        return n;

    {
        char path[64];
        int fd;
        char rb[512];
        ssize_t rn;

        snprintf(path, sizeof(path), "/proc/%d/status", (int)radio_pid);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            rn = read(fd, rb, sizeof(rb) - 1);
            close(fd);
            if (rn > 0) {
                rb[rn] = '\0';
                char *line = rb;
                while (line && *line && n < (int)bufsz - 96) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';
                    if (!strncmp(line, "State:", 6) || !strncmp(line, "Name:", 5) ||
                        !strncmp(line, "PPid:", 5) || !strncmp(line, "Threads:", 8))
                        n += snprintf(buf + n, bufsz - (size_t)n, "%s\r\n", line);
                    line = nl ? nl + 1 : NULL;
                }
            }
        } else {
            n += snprintf(buf + n, bufsz - (size_t)n, "no /proc/%d/status (errno=%d)\r\n",
                          (int)radio_pid, errno);
        }

        snprintf(path, sizeof(path), "/proc/%d/wchan", (int)radio_pid);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            rn = read(fd, rb, sizeof(rb) - 1);
            close(fd);
            if (rn > 0) {
                rb[rn] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "wchan=%s\r\n", rb);
            } else {
                n += snprintf(buf + n, bufsz - (size_t)n, "wchan=(empty, running)\r\n");
            }
        }

        snprintf(path, sizeof(path), "/proc/%d/syscall", (int)radio_pid);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            rn = read(fd, rb, sizeof(rb) - 1);
            close(fd);
            if (rn > 0) {
                rb[rn] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "syscall=%s\r\n", rb);
            }
        }
    }
    return n;
}

/* Same State/wchan/syscall dump as radio_format_pidinfo(), but for
 * cnss_qrtr_pid — qrtr-ns runs as its own forked+dropped-privilege child,
 * separate from the top-level radio_work() job, so radio-pid's own PID
 * doesn't cover it. Added to answer directly: after drop_privileges()
 * succeeds (uid=2906/gid=2906, confirmed via cnss-log), does qrtr-ns's
 * process actually block/sleep somewhere (bind stuck, State=S/D) or is it
 * already gone (no /proc/<pid> at all — silent crash, not shown by
 * proc_running()'s own snapshot-in-time check)? */
int radio_format_qrtr_pidinfo(char *buf, size_t bufsz)
{
    /* cnss_qrtr_pid, read here, lives in THIS (top-level init) process's
     * memory — but the pid that actually matters (the one from the latest
     * `radio` run) was written by start_cnss_stack() inside radio_work()'s
     * forked child, which never propagates back to this process's own copy
     * of the global. /tmp/qrtr_ns.pid is written by that same child and is
     * visible across the fork boundary; prefer it whenever present. */
    pid_t pid = cnss_qrtr_pid;
    int from_file = 0;
    {
        int fd = open("/tmp/qrtr_ns.pid", O_RDONLY);
        if (fd >= 0) {
            char pb[16];
            ssize_t pn = read(fd, pb, sizeof(pb) - 1);
            close(fd);
            if (pn > 0) {
                pb[pn] = '\0';
                int fpid = atoi(pb);
                if (fpid > 0) {
                    pid = (pid_t)fpid;
                    from_file = 1;
                }
            }
        }
    }

    int n = snprintf(buf, bufsz, "qrtr_ns_pid=%d alive=%d source=%s\r\n",
                      (int)pid, pid_alive(pid), from_file ? "file" : "global(stale across fork)");

    if (pid <= 0)
        return n;

    {
        char path[64];
        int fd;
        char rb[512];
        ssize_t rn;

        snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            rn = read(fd, rb, sizeof(rb) - 1);
            close(fd);
            if (rn > 0) {
                rb[rn] = '\0';
                char *line = rb;
                while (line && *line && n < (int)bufsz - 96) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';
                    if (!strncmp(line, "State:", 6) || !strncmp(line, "Name:", 5) ||
                        !strncmp(line, "PPid:", 5) || !strncmp(line, "Threads:", 8))
                        n += snprintf(buf + n, bufsz - (size_t)n, "%s\r\n", line);
                    line = nl ? nl + 1 : NULL;
                }
            }
        } else {
            n += snprintf(buf + n, bufsz - (size_t)n, "no /proc/%d/status (errno=%d)\r\n",
                          (int)pid, errno);
        }

        snprintf(path, sizeof(path), "/proc/%d/wchan", (int)pid);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            rn = read(fd, rb, sizeof(rb) - 1);
            close(fd);
            if (rn > 0) {
                rb[rn] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "wchan=%s\r\n", rb);
            } else {
                n += snprintf(buf + n, bufsz - (size_t)n, "wchan=(empty, running)\r\n");
            }
        }

        snprintf(path, sizeof(path), "/proc/%d/syscall", (int)pid);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            rn = read(fd, rb, sizeof(rb) - 1);
            close(fd);
            if (rn > 0) {
                rb[rn] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "syscall=%s\r\n", rb);
            }
        }

        snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
        {
            DIR *d = opendir(path);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    char lpath[96], target[128];
                    ssize_t ln;
                    if (e->d_name[0] == '.')
                        continue;
                    snprintf(lpath, sizeof(lpath), "%s/%s", path, e->d_name);
                    ln = readlink(lpath, target, sizeof(target) - 1);
                    if (ln > 0) {
                        target[ln] = '\0';
                        n += snprintf(buf + n, bufsz - (size_t)n, "fd/%s -> %s\r\n",
                                      e->d_name, target);
                    }
                }
                closedir(d);
            }
        }
    }
    return n;
}

int radio_format_find(const char *needle, char *buf, size_t bufsz)
{
    static const char *dirs[] = {
        "/vendor/firmware_mnt/image",
        "/vendor/firmware/wlan/qca_cld",
        "/vendor/firmware/wlan",
        "/lib/firmware/wlan/qca_cld",
        NULL
    };
    int n = snprintf(buf, bufsz, "radio-find %s\r\n", needle ? needle : "");

    if (!needle || !needle[0])
        return n + snprintf(buf + n, bufsz - (size_t)n, "  (no pattern)\r\n");
    for (int i = 0; dirs[i] && n < (int)bufsz - 64; i++)
        n = find_in_dir(dirs[i], needle, buf, bufsz, n);
    return n;
}

int radio_format_src_list(char *buf, size_t bufsz)
{
    static const char *dirs[] = {
        "/sbin",
        "/vendor/bin",
        "/system/bin",
        "/vendor/firmware_mnt/image",
        "/vendor/firmware/wlan/qca_cld",
        "/vendor/etc/wifi",
        "/mnt/vendor/persist",
        "/persist",
        NULL
    };
    int n = snprintf(buf, bufsz, "radio-ls\r\n");

    for (int i = 0; dirs[i] && n < (int)bufsz - 64; i++)
        n = format_dir_entries(dirs[i], buf, bufsz, n);
    if (n < (int)bufsz)
        n += radio_format_fw_list(buf + n, bufsz - (size_t)n);
    return n;
}

/* Targeted existence check for the binder context-manager daemons that
 * cnss-daemon needs on /dev/hwbinder. radio-ls's 4KB buffer truncates
 * /vendor/bin and /system/bin alphabetically before reaching entries
 * starting with 'h'/'s', so it can't answer "does hwservicemanager
 * exist" — this checks the exact candidate paths directly instead. */
int radio_format_binder(char *buf, size_t bufsz)
{
    static const char *prefixes[] = {
        "/mnt/vendor/bin/", "/vendor/bin/", "/vendor/bin/hw/",
        "/system/bin/", "/system/vendor/bin/", "/sbin/", NULL
    };
    static const char *svcs[] = {
        "hwservicemanager", "servicemanager", "vndservicemanager", NULL
    };
    int n = snprintf(buf, bufsz, "binder-state\r\n");

    n += snprintf(buf + n, bufsz - (size_t)n,
                  "nodes: binder=%d hwbinder=%d vndbinder=%d ctl=%d\r\n",
                  path_exists("/dev/binder"), path_exists("/dev/hwbinder"),
                  path_exists("/dev/vndbinder"),
                  path_exists("/dev/binderfs/binder-control"));
    n += snprintf(buf + n, bufsz - (size_t)n,
                  "binderfs raw: binder=%d hwbinder=%d vndbinder=%d\r\n",
                  path_exists("/dev/binderfs/binder"),
                  path_exists("/dev/binderfs/hwbinder"),
                  path_exists("/dev/binderfs/vndbinder"));

    for (int s = 0; svcs[s] && n < (int)bufsz - 96; s++) {
        n += snprintf(buf + n, bufsz - (size_t)n, "%s: running=%d",
                      svcs[s], proc_running(svcs[s]));
        int found = 0;
        for (int p = 0; prefixes[p] && n < (int)bufsz - 96; p++) {
            char path[192];
            snprintf(path, sizeof(path), "%s%s", prefixes[p], svcs[s]);
            if (path_exists(path)) {
                n += snprintf(buf + n, bufsz - (size_t)n, " at=%s", path);
                found = 1;
                break;
            }
        }
        if (!found)
            n += snprintf(buf + n, bufsz - (size_t)n, " (not found anywhere)");
        n += snprintf(buf + n, bufsz - (size_t)n, "\r\n");
    }
    n += snprintf(buf + n, bufsz - (size_t)n, "%s", get_binder_exit_report());
    return n;
}

int radio_format_icnss(char *buf, size_t bufsz)
{
    int n = 0;
    static const char *dbg[] = {
        "/sys/kernel/debug/icnss/state",
        "/sys/kernel/debug/icnss/stats",
        NULL
    };

    n += snprintf(buf + n, bufsz - (size_t)n, "icnss-state\r\n");
    n += snprintf(buf + n, bufsz - (size_t)n, "build=%s\r\n", CNSS_BUILD_TAG);
    n += snprintf(buf + n, bufsz - (size_t)n, "qrtr=%d pdmap=%d daemon=%d\r\n",
                  proc_running("qrtr-ns"), proc_running("pd-mapper"),
                  proc_running("cnss-daemon"));
    n += snprintf(buf + n, bufsz - (size_t)n, "hwserv=%d servicemgr=%d\r\n",
                  proc_running("hwservicemanager"), proc_running("servicemanager"));
    {
        char mst[32] = "?";
        if (read_subsys_state("subsys0", mst, sizeof(mst)) == 0)
            n += snprintf(buf + n, bufsz - (size_t)n, "modem=%s wlfw=%d\r\n",
                          mst, qrtr_has_wlfw());
        else
            n += snprintf(buf + n, bufsz - (size_t)n, "modem=? wlfw=%d\r\n",
                          qrtr_has_wlfw());
    }
    /* Show ICNSS_FW_READY from debugfs if available */
    {
        int fw_rdy = icnss_fw_ready_check();
        char st_hex[32] = "n/a";
        int sfd = open("/sys/kernel/debug/icnss/state", O_RDONLY);
        if (sfd >= 0) {
            ssize_t r = read(sfd, st_hex, sizeof(st_hex) - 1);
            close(sfd);
            if (r > 0) { st_hex[r] = '\0'; char *nl = strchr(st_hex, '\n'); if (nl) *nl = '\0'; }
        }
        n += snprintf(buf + n, bufsz - (size_t)n,
                      "fw_ready=%d icnss/state=%s\r\n", fw_rdy, st_hex);
    }
    {
        /* /dev/wlan existence and shutdown_wlan state */
        char sdval[8] = "?";
        int sfd = open("/sys/kernel/shutdown_wlan/shutdown", O_RDONLY);
        if (sfd >= 0) {
            ssize_t r = read(sfd, sdval, sizeof(sdval) - 1);
            close(sfd);
            if (r > 0) {
                sdval[r] = '\0';
                char *nl = strchr(sdval, '\n');
                if (nl) *nl = '\0';
            }
        }
        n += snprintf(buf + n, bufsz - (size_t)n,
                      "devwlan=%s shutdown_wlan=%s\r\n",
                      path_exists("/dev/wlan") ? "yes" : "no", sdval);
    }
    {
        int fd = open("/sys/module/wlan/parameters/fwpath", O_RDONLY);
        if (fd >= 0) {
            char p[32];
            ssize_t r = read(fd, p, sizeof(p) - 1);
            close(fd);
            if (r > 0) {
                p[r] = '\0';
                char *nl = strchr(p, '\n');
                if (nl)
                    *nl = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "wlan fwpath=%s\r\n", p);
            }
        }
    }
    if (path_exists("/proc/net/qrtr")) {
        int fd = open("/proc/net/qrtr", O_RDONLY);
        char chunk[512];
        if (fd >= 0) {
            n += snprintf(buf + n, bufsz - (size_t)n, "qrtr-nodes:\r\n");
            while (n < (int)bufsz - 64) {
                ssize_t r = read(fd, chunk, sizeof(chunk) - 1);
                if (r <= 0)
                    break;
                chunk[r] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "%s", chunk);
            }
            close(fd);
        }
    } else {
        n += snprintf(buf + n, bufsz - (size_t)n, "qrtr-nodes: none\r\n");
    }
    for (int i = 0; dbg[i] && n < (int)bufsz - 128; i++) {
        int fd = open(dbg[i], O_RDONLY);
        char chunk[256];
        if (fd < 0)
            continue;
        n += snprintf(buf + n, bufsz - (size_t)n, "%s:\r\n", dbg[i]);
        while (n < (int)bufsz - 64) {
            ssize_t r = read(fd, chunk, sizeof(chunk) - 1);
            if (r <= 0)
                break;
            chunk[r] = '\0';
            n += snprintf(buf + n, bufsz - (size_t)n, "%s", chunk);
        }
        n += snprintf(buf + n, bufsz - (size_t)n, "\r\n");
        close(fd);
    }
    return n;
}

int radio_format_diag(char *buf, size_t bufsz)
{
    int n = 0;

    sync_mount_flags();
    n += snprintf(buf + n, bufsz - (size_t)n,
                  "radio-diag\r\n"
                  "vendor=%d system=%d persist=%d modem=%d bt_fw=%d wlan_fw=%d\r\n"
                  "cnss qrtr=%d pdmap=%d daemon=%d vndksupport=%d\r\n"
                  "wlan0=%s hci0=%s\r\n",
                  vendor_mounted, system_mounted, persist_mounted, modem_mounted, bt_fw_mounted,
                  has_wlan_firmware(),
                  proc_running("qrtr-ns"), proc_running("pd-mapper"),
                  proc_running("cnss-daemon"), path_exists("/lib64/libvndksupport.so"),
                  path_exists("/sys/class/net/wlan0") ? "yes" : "no",
                  path_exists("/sys/class/bluetooth/hci0") ? "yes" : "no");
    if (n < (int)bufsz) {
        int fd = open("/proc/mounts", O_RDONLY);
        if (fd >= 0) {
            char m[1024];
            ssize_t r = read(fd, m, sizeof(m) - 1);
            close(fd);
            if (r > 0) {
                m[r] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "mounts:\r\n");
                for (char *line = m; line && *line && n < (int)bufsz - 48; ) {
                    char *nl = strchr(line, '\n');
                    if (nl)
                        *nl++ = '\0';
                    if (strstr(line, "vendor") || strstr(line, "persist") ||
                        strstr(line, "firmware") || strstr(line, "modem"))
                        n += snprintf(buf + n, bufsz - (size_t)n, "  %s\r\n", line);
                    line = nl;
                }
            }
        }
    }
    if (n < (int)bufsz) {
        int fd = open("/sys/module/wlan/parameters/con_mode", O_RDONLY);
        if (fd >= 0) {
            char p[32];
            ssize_t r = read(fd, p, sizeof(p) - 1);
            close(fd);
            if (r > 0) {
                p[r] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "wlan con_mode=%s\r\n", p);
            }
        }
    }
    if (n < (int)bufsz) {
        static const char *dbg[] = {
            "/sys/kernel/debug/icnss/stats",
            "/sys/kernel/debug/icnss/state",
            NULL
        };
        for (int i = 0; dbg[i] && n < (int)bufsz - 128; i++) {
            int fd = open(dbg[i], O_RDONLY);
            char chunk[256];
            if (fd < 0)
                continue;
            n += snprintf(buf + n, bufsz - (size_t)n, "%s:\r\n", dbg[i]);
            while (n < (int)bufsz - 64) {
                ssize_t r = read(fd, chunk, sizeof(chunk) - 1);
                if (r <= 0)
                    break;
                chunk[r] = '\0';
                n += snprintf(buf + n, bufsz - (size_t)n, "%s", chunk);
            }
            n += snprintf(buf + n, bufsz - (size_t)n, "\r\n");
            close(fd);
        }
    }
    if (n < (int)bufsz)
        n += radio_format_fw_list(buf + n, bufsz - (size_t)n);
    return n;
}
