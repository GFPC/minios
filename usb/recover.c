#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/radio.h"
#include "minios/sysfs.h"
#include "minios/usb.h"

static int udc_miss_streak;
static int maintain_tick;
static int rebind_cooldown;

static int udc_sysfs_bound(void)
{
    char buf[64];
    int fd = open(USB_G "/UDC", O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    for (char *p = buf; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
            *p = '\0';
    }
    return buf[0] != '\0';
}

static int udc_controller_present(void)
{
    char path[128];

    if (!usb_udc_name[0])
        return 0;
    snprintf(path, sizeof(path), "/sys/class/udc/%s", usb_udc_name);
    return access(path, F_OK) == 0;
}

static void usb_rebind_udc(const char *why)
{
    if (!usb_udc_name[0])
        return;

    klogf("USB: rebind %s (%s)", usb_udc_name, why);
    sysfs_write(USB_G "/UDC", "");
    usleep(500000);
    sysfs_write(USB_G "/UDC", usb_udc_name);
    usleep(400000);
    if (usb_com_active)
        devnodes_ensure_cser();

    if (udc_sysfs_bound())
        klog("USB: rebind ok");
    else
        klog("USB: rebind failed");
    rebind_cooldown = 60;
}

/* Fast recovery (miss_streak >= 3, ~a couple seconds) stays gated to RF
 * jobs only — idle auto-rebind at that speed caused host/usbipd-visible
 * cyclic re-enumeration in an earlier session, and that finding still
 * holds.
 *
 * IDLE_MISS_LIMIT is a separate, much longer safety net: this project hit
 * a real, repeated symptom all through one whole session where the UDC
 * went missing with *no* RF job running at all (e.g. right after a COM<->ADB
 * switch, or just sitting idle) and *never* recovered on its own — the
 * existing RF-job-gated path could never fire for it, and the only
 * confirmed recovery was a full physical power-cycle (a real replug never
 * worked either, suggesting the host-side VBUS/extcon disconnect signal
 * itself isn't reliably reaching dwc3 on this board — a separate, deeper
 * kernel question not resolved this session, see MEMORY.md §4.5at). A
 * one-shot rebind after a genuinely long, continuous absence (tens of
 * seconds, not a couple) is a much rarer trigger than the fast RF-job path
 * and should not reproduce that earlier cyclic-reenumeration problem, while
 * still eventually self-healing instead of requiring physical access. */
#define IDLE_MISS_LIMIT 40 /* ticks; tick period ~5*loop-interval, see below */

void usb_com_maintain(void)
{
    maintain_tick++;
    if (maintain_tick < 5)
        return;
    maintain_tick = 0;

    if (rebind_cooldown > 0)
        rebind_cooldown--;

    usb_udc_load();
    if (!usb_udc_name[0])
        return;

    if (udc_sysfs_bound() || udc_controller_present()) {
        udc_miss_streak = 0;
        return;
    }

    udc_miss_streak++;

    if (radio_job_running() || radio_scan_running()) {
        if (udc_miss_streak >= 3 && rebind_cooldown == 0) {
            udc_miss_streak = 0;
            usb_rebind_udc("rf job UDC drop");
        }
        return;
    }

    /* Idle safety net — see IDLE_MISS_LIMIT comment above. */
    if (udc_miss_streak >= IDLE_MISS_LIMIT && rebind_cooldown == 0) {
        udc_miss_streak = 0;
        usb_rebind_udc("idle UDC drop (long absence)");
    }
}

void usb_rebind_request(void)
{
    usb_udc_load();
    if (!usb_udc_name[0]) {
        klog("USB: rebind skipped (no UDC name)");
        return;
    }
    usb_rebind_udc("manual");
}
