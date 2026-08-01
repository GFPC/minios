#define _GNU_SOURCE
#include "bt.h"
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

#define BT_CMD_PWR_CTRL 0xbfad
extern int bt_fw_mounted;
extern char bt_st[80];
void bt_power_on(void)
{
    int fd;

    rfkill_unblock_all();
    fd = open("/dev/rfkill", O_WRONLY);
    if (fd >= 0)
        close(fd);

    fd = open("/dev/btpower", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        int on = 1;
        if (ioctl(fd, BT_CMD_PWR_CTRL, on) == 0)
            LOGI("radio", "%s", "btpower on");
        close(fd);
    } else {
        wf("/sys/class/rfkill/rfkill1/state", "1");
        wf("/sys/class/rfkill/rfkill2/state", "1");
    }
}


void try_bt_enable(void)
{
    const char *hcia;
    pid_t hci_pid = 0;

    bt_power_on();

    /* Start hciattach to initialize the BT UART chip.
     * Without hciattach, /sys/class/bluetooth/hci0 will never appear
     * regardless of rfkill state. */
    hcia = access("/sbin/hciattach", X_OK) == 0 ? "/sbin/hciattach" : NULL;
    if (!hcia)
        hcia = access("/usr/sbin/hciattach", X_OK) == 0 ? "/usr/sbin/hciattach" : NULL;

    if (hcia) {
        /* Try QCA-specific speed first, fall back to generic */
        const char *uart_devs[] = {
            "/dev/ttyHS0", "/dev/ttyMSM0", "/dev/ttyHSL0", NULL
        };
        for (int i = 0; uart_devs[i] && !hci_pid; i++) {
            if (access(uart_devs[i], R_OK) != 0)
                continue;
            hci_pid = fork();
            if (hci_pid == 0) {
                int null = open("/dev/null", O_RDONLY);
                if (null >= 0) { dup2(null, 0); close(null); }
                int lfd = open("/tmp/hciattach.log",
                               O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (lfd >= 0) {
                    dprintf(lfd, "--- hciattach %s qca 3000000 ---\n", uart_devs[i]);
                    dup2(lfd, 1); dup2(lfd, 2); close(lfd);
                }
                /* Try QCA 3Mbit with hardware flow control */
                execl(hcia, "hciattach", "-n",
                      uart_devs[i], "qca", "3000000", "flow", NULL);
                /* Fallback: any driver at 115200 */
                execl(hcia, "hciattach", "-n",
                      uart_devs[i], "any", "115200", NULL);
                _exit(127);
            }
            if (hci_pid > 0) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "bt: hciattach pid=%d dev=%s", (int)hci_pid, uart_devs[i]);
                LOGI("radio", "%s", msg);
            }
        }
    } else {
        LOGI("radio", "%s", "bt: hciattach not found in /sbin");
    }

    /* Wait up to 20s for hci0 to appear */
    wait_for_iface("/sys/class/bluetooth/hci0", 20);

    if (path_exists("/sys/class/bluetooth/hci0")) {
        snprintf(bt_st, sizeof(bt_st), "BT: hci0 up");
        LOGI("radio", "%s", "hci0 up");
        return;
    }

    /* hci0 didn't appear — log the hciattach output */
    if (hci_pid > 0) {
        usleep(500000);
        int st = 0;
        if (waitpid(hci_pid, &st, WNOHANG) == hci_pid) {
            char msg[80];
            snprintf(msg, sizeof(msg), "bt: hciattach exit=%d",
                     WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            LOGI("radio", "%s", msg);
        }
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


