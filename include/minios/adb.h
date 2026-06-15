#ifndef MINIOS_ADB_H
#define MINIOS_ADB_H

#include <sys/types.h>

extern pid_t adbd_pid;
extern pid_t ffs_adb_pid;

pid_t adb_pid_alive(void);
void adb_env_prepare(void);
void adb_start_tcp(void);
pid_t adb_start_daemon(void);

/* Used by usb/gadget.c */
int usb_mount_ffs_adb(void);
int usb_link_ffs_adb(void);
void wait_ffs_ep1(int sec);
int wait_adbd_handoff(int sec);

#endif
