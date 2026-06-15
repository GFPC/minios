#ifndef MINIOS_RADIO_H
#define MINIOS_RADIO_H

#include <stddef.h>

/* Non-blocking: start WiFi/BT bring-up in a child (safe for COM/USB). */
void radio_request_async(void);

/* Legacy alias — never blocks, same as radio_request_async(). */
void radio_init_async(void);
void radio_probe_now(void);

/* Non-blocking WiFi scan → /tmp/wifi-scan.txt */
void radio_scan_request_async(void);

/* Call from main loop to reap finished jobs. */
void radio_poll(void);

int radio_job_running(void);
int radio_scan_running(void);

const char *radio_wifi_status(void);
const char *radio_bt_status(void);
int radio_format_status(char *buf, size_t bufsz);

#endif
