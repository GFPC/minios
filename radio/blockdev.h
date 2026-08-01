#ifndef MINIOS_BLOCKDEV_H
#define MINIOS_BLOCKDEV_H

#include <stddef.h>

/* Create /dev/block/by-name/* from mmc PARTNAME. Returns count linked. */
int blockdev_ensure_by_name(void);

/* Ensure /dev/mmcblk* nodes exist from sysfs. Returns count created. */
int blockdev_ensure_devnodes(void);

/* Wait up to sec for any mmc block device in sysfs. */
int blockdev_wait_mmc(int sec);

/* Resolve partition path, e.g. "vendor" -> /dev/block/by-name/vendor. */
const char *blockdev_by_name(const char *part);

/* Human-readable partition/mount diagnostics for COM. */
int blockdev_format_diag(char *buf, size_t bufsz);

#endif
