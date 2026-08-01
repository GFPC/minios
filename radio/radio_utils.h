#ifndef MINIOS_RADIO_UTILS_H
#define MINIOS_RADIO_UTILS_H

#include <sys/types.h>
#include <stddef.h>

void wf(const char *path, const char *val);
int wf_checked(const char *path, const char *val);
void wf_boot(const char *path);
void md(const char *p);
int path_exists(const char *p);
int pid_alive(pid_t p);
void write_file(const char *path, const char *data);
void ensure_etc_group(void);
void run_sh(const char *cmd);
void radio_child_setup(void);
void wf_path_join(const char *dir, const char *leaf);
int copy_file_bin(const char *src, const char *dst);
int proc_cmdline_has(const char *needle);
int proc_running(const char *comm);

#endif
