#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/boot_display.h"
#include "minios/com.h"
#include "minios/devnodes.h"
#include "minios/fwload.h"
#include "minios/led.h"
#include "minios/log.h"
#include "minios/radio.h"
#include "minios/selinux.h"
#include "minios/sysfs.h"
#include "minios/touch.h"
#include "minios/triggers.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

int main(void)
{
    sysfs_mkdir("/dev"); sysfs_mkdir("/proc"); sysfs_mkdir("/sys"); sysfs_mkdir("/config"); sysfs_mkdir("/tmp");

    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mknod("/dev/console", S_IFCHR|0600, makedev(5,1));
    mknod("/dev/null",    S_IFCHR|0666, makedev(1,3));
    mknod("/dev/kmsg",    S_IFCHR|0600, makedev(1,11));
    mknod("/dev/urandom", S_IFCHR|0666, makedev(1,9));

    klog("=== MINIOS START ===");
    setenv("PATH", "/bin:/sbin", 1);

    mount("proc",     "/proc",   "proc",    0, NULL);
    mount("sysfs",    "/sys",    "sysfs",   0, NULL);
    mount("debugfs",  "/sys/kernel/debug", "debugfs", 0, NULL);
    klog("proc/sysfs OK");

    fwload_prepare_paths();
    fwload_helper_start();

    wdt_open();
    wdt_pet();

    /* immediate haptic ping — often works before LED drivers */
    vib_pulse(300);
    klog("vib pulse");

    mount("configfs", "/config", "configfs", 0, NULL);
    selinux_prepare();

    /* USB first — display DRM must not block or panic before gadget is up */
    led_prepare();
    vib_pulse(100);

    int usb_ok = (usb_setup() == 0);
    wdt_pet();
    klog(usb_ok ? "USB up" : "USB fail");
    if (usb_ok)
        led_blink(2);

    /* Display after USB — async, must not block COM */
    devnodes_ensure_drm();
    touch_ensure_nodes();
    boot_display_start_async();
    klog("display: kms_paint async");

    /* Radio only on user request — wlan power-on can glitch USB/COM. */

    if (usb_com_active) {
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
        radio_poll();
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
