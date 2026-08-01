#ifndef MINIOS_RADIO_H
#define MINIOS_RADIO_H

#include <stddef.h>

void radio_request_wifi_async(void);
void radio_request_async(void);
void radio_init_async(void);
void radio_probe_now(void);
void radio_scan_request_async(void);
void radio_poll(void);

int radio_job_running(void);
int radio_scan_running(void);

const char *radio_wifi_status(void);
const char *radio_bt_status(void);
int radio_format_status(char *buf, size_t bufsz);

const char *stage_vendor_bin(const char *name);

#endif
