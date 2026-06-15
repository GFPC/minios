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
