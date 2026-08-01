#ifndef MINIOS_CNSS_H
#define MINIOS_CNSS_H

#include <sys/types.h>
#include <stddef.h>

void ensure_cnss_devnodes(void);
void cnss_log_exit(pid_t pid);
void cnss_log_line(const char *msg);
void cnss_drop_to_system(void);
void cnss_log_argv(const char *run, char *const argv[]);
int exec_via_linker64(const char *run, char *const argv[]);
int cnss_try_exec(const char *run, char *const argv[]);
void set_daemon_preload(void);
void cnss_child_setup(const char *run);
pid_t cnss_spawn_variant(const char *run, char *const argv[], const char *label);
pid_t start_cnss_daemon(const char *path);
void daemon_child_setup(const char *path, const char *logfile);
pid_t start_vendor_daemon(const char *path, char *const argv[]);
void run_vendor_oneshot(const char *path, char *const argv[]);
void stage_cnss_libs(void);
void ensure_binder_nodes(void);
void start_binder_services(void);
const char *get_binder_exit_report(void);
void check_hwservicemanager_late_exit(void);
void ensure_cnss_sockets(void);
void start_modem_qmi_services(void);
void start_cnss_stack(void);
int cnss_stack_running(void);

#endif
