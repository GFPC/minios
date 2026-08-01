#ifndef MINIOS_RADIO_H
#define MINIOS_RADIO_H

#include <stddef.h>

/* WiFi-only bring-up (skips BT — keeps USB/COM stable). */
void radio_request_wifi_async(void);

/* WiFi + BT bring-up in a child process. */
void radio_request_async(void);

void radio_init_async(void);
void modem_qmi_services_start(void);
void radio_load_modem_ko(void);
void try_load_qrtr_snoop(void);
void radio_early_modem_boot(void);
void radio_stage_early_bins(void);
void radio_probe_now(void);

void radio_scan_request_async(void);
void radio_poll(void);

int radio_job_running(void);
int radio_scan_running(void);

const char *radio_wifi_status(void);
const char *radio_bt_status(void);
int radio_format_status(char *buf, size_t bufsz);
int radio_format_diag(char *buf, size_t bufsz);
int radio_format_fw_list(char *buf, size_t bufsz);
int radio_format_src_list(char *buf, size_t bufsz);
int radio_format_find(const char *needle, char *buf, size_t bufsz);
int radio_format_icnss(char *buf, size_t bufsz);
int radio_format_binder(char *buf, size_t bufsz);
int radio_format_pidinfo(char *buf, size_t bufsz);

#endif
