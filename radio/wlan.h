#ifndef MINIOS_WLAN_H
#define MINIOS_WLAN_H

#include <sys/types.h>
#include <stddef.h>

void rfkill_unblock_all(void);
void walk_wcnss_in_dir(const char *parent);
void try_bind_icnss_driver(const char *drv);
void modprobe_one(const char *mod);
int icnss_fw_ready_check(void);
void wait_icnss_fw_ready(int max_sec);
void configure_wlan_driver(void);
int qrtr_has_wlfw(void);
int wait_for_wlfw(int max_sec);
void trigger_wlan_power(void);
void iface_up(const char *ifname);
void wait_for_iface(const char *sys_path, int sec);
void try_wlan_enable(void);

#endif
