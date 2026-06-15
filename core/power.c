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
