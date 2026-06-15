#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/process.h"

void run_csysfs_mkdir(const char *cmd)
{
    pid_t p = fork();
    if (p == 0) {
        setenv("PATH", "/bin:/sbin:/system/bin", 1);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}
