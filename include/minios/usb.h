#ifndef MINIOS_USB_H
#define MINIOS_USB_H

#include <sys/types.h>

#define USB_G "/config/usb_gadget/g0"

extern char com_dev_path[64];
extern int usb_com_active;
extern int usb_ncm_active;
extern char usb_net_if[32];
extern char usb_udc_name[64];

int usb_setup(void);
void usb_adb_async(void);
void usb_restore_com_only(void);
void usb_start_com_shell(void);
void usb_tcp_adb_async(void);
void usb_net_setup(void);
int usb_open_com_tty(void);
void usb_udc_load(void);
void usb_com_maintain(void);
void usb_rebind_request(void);
int usb_enable_ncm(void);
int usb_add_diag(void);
int usb_setup_diag_only(void);
void usb_config_clear_links(void);

#endif
