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

/* Real, meaningful liveness signal: /sys/class/udc/<name>/state reflects
 * gadget->state (the USB core's own enumeration state machine -- "not
 * attached", "attached", "powered", "default", "address", "configured",
 * "suspended"), updated by real SetAddress/SetConfiguration/disconnect
 * events. This is what the two checks below were trying and failing to
 * approximate: udc_controller_present() only checks that the dwc3 driver
 * has *probed* (/sys/class/udc/<name> itself), which is created once at
 * boot and never goes away for the rest of uptime regardless of any
 * later cable/link state -- and udc_sysfs_bound() only checks that
 * ConfigFS still has a UDC name *written*, which also doesn't get
 * cleared just because the link died. Confirmed live: a real, reproduced
 * full-USB-dropout event (SD-card boot.log, MEMORY.md §4.5c9) never
 * self-recovered even after 4+ minutes -- well past this file's own
 * IDLE_MISS_LIMIT threshold -- which is only explainable if udc_miss_streak
 * never actually incremented at all, i.e. the old OR-gated check below
 * was passing throughout. */
static int udc_link_configured(void)
{
    char path[128], buf[32];
    int fd, n;

    if (!usb_udc_name[0])
        return 0;
    snprintf(path, sizeof(path), "/sys/class/udc/%s/state", usb_udc_name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return strncmp(buf, "configured", 10) == 0;
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

    /* Real fix (MEMORY.md §4.5c9): this used to be
     * "udc_sysfs_bound() || udc_controller_present()" -- both sides of
     * that OR stay true essentially forever after boot regardless of
     * actual link state (see udc_link_configured()'s comment above), so
     * udc_miss_streak could never increment and neither recovery path
     * below could ever fire. udc_link_configured() reads the real
     * gadget->state enumeration state instead. */
    if (udc_link_configured()) {
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
