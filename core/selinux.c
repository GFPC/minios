#define _GNU_SOURCE
#include <sys/mount.h>
#include <unistd.h>
#include "minios/log.h"
#include "minios/selinux.h"
#include "minios/sysfs.h"

void selinux_prepare(void)
{
    sysfs_mkdir("/sys/fs/selinux");
    if (mount("selinuxfs", "/sys/fs/selinux", "selinuxfs", 0, NULL) == 0)
        klog("selinuxfs OK");
    if (access("/sys/fs/selinux/enforce", W_OK) == 0)
        sysfs_write("/sys/fs/selinux/enforce", "0");
}
