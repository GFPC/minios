#ifndef MINIOS_FIRMWARE_H
#define MINIOS_FIRMWARE_H

#include <sys/types.h>
#include <stddef.h>

void symlink_force(const char *target, const char *linkpath);
void ensure_linker64_real(void);
void ensure_linkerconfig(void);
void ensure_system_bin_tools(void);
void ensure_android_roots(void);
void ensure_wifi_config(void);
void copy_dir_if_present(const char *src, const char *dst);
int try_mount_ro(const char *src, const char *dst, const char *fstype);
int try_mount_ro_any(const char *src, const char *dst);
int try_mount_part(const char *part, const char *dst);
int try_mount_rw(const char *src, const char *dst, const char *fstype);
int try_mount_rw_any(const char *src, const char *dst);
int try_mount_part_rw(const char *part, const char *dst);
int vendor_tree_visible(void);
void stage_cnss_daemon_from_vendor(void);
int mount_vendor_partition(void);
void ensure_block_layout(void);
int mount_point_active(const char *dst);
void sync_mount_flags(void);
void mount_radio_partitions(void);
void symlink_firmware(const char *target, const char *linkpath);
void symlink_if_missing(const char *target, const char *linkpath);
void link_fw_bin(const char *srcdir, const char *name);
void link_modem_pil_firmware(void);
int link_modem_pil_firmware_count(void);
void harvest_fw_bins(const char *srcdir, int depth);
void link_board_data_variants(const char *srcdir);
void link_fw_version_aliases(void);
void link_known_firmware_bins(void);
void link_firmware_tree(void);
void ensure_rmtfs_readonly_layout(void);
void ensure_rmtfs_boot_paths(void);
int ensure_modem_firmware_mounted(void);
void ensure_rmtfs_firmware_paths(void);
void set_firmware_class_path(void);
int ensure_modem_pil_firmware(void);
int dir_has_files(const char *path);
int path_has_bin_files(const char *path);
int has_wlan_firmware(void);
void radio_stage_early_bins(void);
void ensure_debugfs(void);

#endif
