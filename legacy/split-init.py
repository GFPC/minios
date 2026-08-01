#!/usr/bin/env python3
"""Split legacy monolithic init.c into modular MiniOS sources."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "legacy" / "init.c.bak"
if not SRC.exists():
    SRC = ROOT / "init.c"

lines = SRC.read_text().splitlines(keepends=True)


def chunk(a, b):
    return "".join(lines[a - 1 : b])


def xform(body):
    body = re.sub(r"\bstatic\s+", "", body)
    body = body.replace("wf(", "sysfs_write(")
    body = body.replace("md(", "sysfs_mkdir(")
    body = re.sub(r"\bG\"", 'USB_G"', body)
    body = re.sub(r"\bG ", "USB_G ", body)
    body = body.replace("(G ", "(USB_G ")
    body = body.replace(", G)", ", USB_G)")
    body = body.replace("do_power_off(", "power_off(")
    body = body.replace("do_reboot_mode(", "reboot_mode(")
    body = body.replace("ensure_cser_nodes(", "devnodes_ensure_cser(")
    body = body.replace("ensure_drm_nodes(", "devnodes_ensure_drm(")
    body = body.replace("run_kms_paint(", "boot_display_run_kms(")
    body = body.replace("display_try_once(", "boot_display_try_once(")
    body = body.replace("start_display_async(", "boot_display_start_async(")
    body = body.replace("check_ui_triggers(", "triggers_poll(")
    body = body.replace("start_tcp_adbd(", "adb_start_tcp(")
    body = body.replace("start_adbd(", "adb_start_daemon(")
    body = body.replace("open_com_tty(", "usb_open_com_tty(")
    body = body.replace("run_cmd(", "process_run(")
    return body


def w(rel, text):
    p = ROOT / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    print(f"  {rel}")


# ── headers ──
w("include/minios/log.h", """\
#ifndef MINIOS_LOG_H
#define MINIOS_LOG_H

void klog(const char *msg);
void klogf(const char *fmt, ...);

#endif
""")

w("include/minios/sysfs.h", """\
#ifndef MINIOS_SYSFS_H
#define MINIOS_SYSFS_H

void sysfs_write(const char *path, const char *val);
void sysfs_mkdir(const char *path);

#endif
""")

w("include/minios/watchdog.h", """\
#ifndef MINIOS_WATCHDOG_H
#define MINIOS_WATCHDOG_H

void wdt_open(void);
void wdt_pet(void);

#endif
""")

w("include/minios/power.h", """\
#ifndef MINIOS_POWER_H
#define MINIOS_POWER_H

void power_off(void);
void reboot_warm(void);
void reboot_mode(const char *mode);

#endif
""")

w("include/minios/process.h", """\
#ifndef MINIOS_PROCESS_H
#define MINIOS_PROCESS_H

void process_run(const char *cmd);

#endif
""")

w("include/minios/selinux.h", """\
#ifndef MINIOS_SELINUX_H
#define MINIOS_SELINUX_H

void selinux_prepare(void);

#endif
""")

w("include/minios/devnodes.h", """\
#ifndef MINIOS_DEVNODES_H
#define MINIOS_DEVNODES_H

void devnodes_ensure_cser(void);
void devnodes_ensure_drm(void);

#endif
""")

w("include/minios/led.h", """\
#ifndef MINIOS_LED_H
#define MINIOS_LED_H

void led_prepare(void);
void led_blink(int times);
void vib_pulse(int ms);

#endif
""")

w("include/minios/display.h", """\
#ifndef MINIOS_DISPLAY_H
#define MINIOS_DISPLAY_H

void display_status(unsigned int top, unsigned int bg);

#endif
""")

w("include/minios/boot_display.h", """\
#ifndef MINIOS_BOOT_DISPLAY_H
#define MINIOS_BOOT_DISPLAY_H

int boot_display_try_once(void);
int boot_display_run_kms(void);
void boot_display_start_async(void);

#endif
""")

w("include/minios/usb.h", """\
#ifndef MINIOS_USB_H
#define MINIOS_USB_H

#include <sys/types.h>

#define USB_G "/config/usb_gadget/g0"

extern char com_dev_path[64];
extern int usb_com_active;
extern int usb_ncm_active;
extern char usb_net_if[32];
extern char usb_udc_name[64];

int usb_setup(void);
void usb_adb_async(void);
void usb_restore_com_only(void);
void usb_start_com_shell(void);
void usb_tcp_adb_async(void);
void usb_net_setup(void);
int usb_open_com_tty(void);

#endif
""")

w("include/minios/adb.h", """\
#ifndef MINIOS_ADB_H
#define MINIOS_ADB_H

#include <sys/types.h>

extern pid_t adbd_pid;
extern pid_t ffs_adb_pid;

pid_t adb_pid_alive(void);
void adb_env_prepare(void);
void adb_start_tcp(void);
pid_t adb_start_daemon(void);

#endif
""")

w("include/minios/com.h", """\
#ifndef MINIOS_COM_H
#define MINIOS_COM_H

void com_shell(int fd);
int com_handle(int out, const char *line);

#endif
""")

w("include/minios/triggers.h", """\
#ifndef MINIOS_TRIGGERS_H
#define MINIOS_TRIGGERS_H

void triggers_poll(void);

#endif
""")

w("include/minios/fwload.h", chunk(1, 7).replace("fwload.h", "minios/fwload.h") if False else """\
#ifndef MINIOS_FWLOAD_H
#define MINIOS_FWLOAD_H

void fwload_prepare_paths(void);
void fwload_helper_start(void);

#endif
""")

# copy fwload header content from original
orig_fwload = (ROOT / "fwload.h")
if orig_fwload.exists():
    t = orig_fwload.read_text()
    t = t.replace("MINIOS_FWLOAD_H", "MINIOS_FWLOAD_H").replace("#ifndef MINIOS_FWLOAD_H", "#ifndef MINIOS_FWLOAD_H")
    w("include/minios/fwload.h", """\
#ifndef MINIOS_FWLOAD_H
#define MINIOS_FWLOAD_H

void fwload_prepare_paths(void);
void fwload_helper_start(void);

#endif
""")

print("headers")

# ── core ──
w("core/log.c", """\
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include "minios/log.h"

""" + xform(chunk(86, 104)))

w("core/sysfs.c", """\
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "minios/sysfs.h"

void sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { write(fd, val, strlen(val)); close(fd); }
}

void sysfs_mkdir(const char *path) { mkdir(path, 0755); }
""")

w("core/watchdog.c", """\
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "minios/watchdog.h"

""" + xform(chunk(72, 83)))

w("core/power.c", """\
#define _GNU_SOURCE
#include <linux/reboot.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "minios/log.h"
#include "minios/power.h"

void power_off(void)
{
    klog("power off");
    sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_POWER_OFF, NULL);
    reboot(RB_POWER_OFF);
}

void reboot_mode(const char *mode)
{
    klogf("reboot mode=%s", mode ? mode : "warm");
    sync();
    if (mode)
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, (char *)mode);
    reboot(RB_AUTOBOOT);
}

void reboot_warm(void)
{
    reboot_mode(NULL);
}
""")

w("core/process.c", """\
#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/process.h"

""" + xform(chunk(1214, 1224)))

w("core/selinux.c", """\
#define _GNU_SOURCE
#include <sys/mount.h>
#include <unistd.h>
#include "minios/log.h"
#include "minios/selinux.h"
#include "minios/sysfs.h"

""" + xform(chunk(1558, 1565)))

print("core")

# ── hw ──
w("hw/devnodes.c", """\
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/watchdog.h"

""" + xform(chunk(146, 208)))

w("hw/led.c", """\
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "minios/led.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/watchdog.h"

""" + xform(chunk(211, 335)))

w("hw/display.c", """\
#define _GNU_SOURCE
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include "minios/display.h"
#include "minios/log.h"
#include "minios/sysfs.h"

""" + xform(chunk(338, 588)))

w("hw/boot_display.c", """\
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include "minios/boot_display.h"
#include "minios/log.h"

""" + xform(chunk(1041, 1087)).replace(
    "if (drm_fd >= 0)", "if (0 /* drm owned by display.c */ && drm_fd >= 0)"
))

# boot_display references drm_fd from display.c - need to fix
boot_disp = xform(chunk(1041, 1087))
boot_disp = re.sub(
    r"if \(drm_fd >= 0\) \{[^}]+\}",
    "/* kms_paint owns DRM */",
    boot_disp,
    count=1,
)
w("hw/boot_display.c", """\
#define _GNU_SOURCE
#include <unistd.h>
#include "minios/boot_display.h"
#include "minios/log.h"

""" + boot_disp)

print("hw")

# ── usb state ──
w("usb/state.c", """\
#include "minios/adb.h"
#include "minios/usb.h"

char com_dev_path[64] = "/dev/ttyGS0";
int usb_com_active = 0;
int usb_ncm_active = 0;
char usb_net_if[32] = "usb0";
char usb_udc_name[64];
pid_t adbd_pid = -1;
pid_t ffs_adb_pid = -1;
""")

# ── usb/adb.c ──
adb_body = (
    xform(chunk(114, 144))
    + xform(chunk(1339, 1352))
    + xform(chunk(1388, 1613))
    + xform(chunk(1893, 1928))
)
w("usb/adb.c", """\
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

""" + adb_body)

# ── usb/net.c ──
w("usb/net.c", """\
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "minios/log.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

""" + xform(chunk(1258, 1337)))

# net chunk overlaps with adb - use only 1258-1314 and 1316-1337
w("usb/net.c", """\
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "minios/log.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

""" + xform(chunk(1258, 1314)) + xform(chunk(1316, 1337)))

# ── usb/com.c ──
w("usb/com.c", """\
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/boot_display.h"
#include "minios/com.h"
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/radio.h"
#include "minios/usb.h"
#include "minios/watchdog.h"
#include "minios/touch.h"

""" + xform(chunk(591, 1038)))

# ── usb/gadget.c ──
gadget_parts = (
    chunk(1089, 1134)
    + chunk(1164, 1212)
    + chunk(1354, 1363)
    + chunk(1365, 1386)
    + chunk(1658, 1681)
    + chunk(1683, 2076)
)
w("usb/gadget.c", """\
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/com.h"
#include "minios/devnodes.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

""" + xform(gadget_parts))

print("usb")

# ── init ──
w("init/triggers.c", """\
#define _GNU_SOURCE
#include <unistd.h>
#include "minios/power.h"
#include "minios/radio.h"
#include "minios/triggers.h"

""" + xform(chunk(1226, 1256)))

w("init/main.c", """\
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

""" + xform(chunk(2079, 2168)))

print("init")
print("done")
