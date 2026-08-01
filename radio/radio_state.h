#ifndef MINIOS_RADIO_STATE_H
#define MINIOS_RADIO_STATE_H

#include <sys/types.h>

extern char wifi_st[80];
extern char bt_st[80];
extern int vendor_mounted;
extern int system_mounted;
extern int persist_mounted;
extern int modem_mounted;
extern int bt_fw_mounted;
extern pid_t radio_pid;
extern pid_t scan_pid;
extern pid_t cnss_qrtr_pid;
extern pid_t cnss_pdmap_pid;
extern pid_t cnss_daemon_pid;
extern int radio_job_bt;

#endif
