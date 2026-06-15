#include "minios/adb.h"
#include "minios/usb.h"

char com_dev_path[64] = "/dev/ttyGS0";
int usb_com_active = 0;
int usb_ncm_active = 0;
char usb_net_if[32] = "usb0";
char usb_udc_name[64];
pid_t adbd_pid = -1;
pid_t ffs_adb_pid = -1;
