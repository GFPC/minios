#ifndef MINIOS_MODEM_H
#define MINIOS_MODEM_H

#include <sys/types.h>
#include <stddef.h>

int load_ko_file(const char *path);
void try_load_modem_ko(void);
void radio_load_modem_ko(void);
void try_load_qrtr_snoop(void);
int read_subsys_state(const char *subsys, char *buf, size_t bufsz);
void boot_remote_procs(void);
int wait_modem_leave_offlining(char *st, size_t stsz, int max_sec);
void boot_modem(void);
int wait_modem_online(int max_sec);
void radio_early_modem_boot(void);
void start_rmtfs_daemons_early(void);
void radio_modem_recover_stuck(void);
void start_diag_klog(void);
void start_diag_mdlog(void);
void start_diag_capture(void);

#endif
