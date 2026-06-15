#ifndef MINIOS_BLOCKDEV_H
#define MINIOS_BLOCKDEV_H

/* Create /dev/block/by-name/* from mmc PARTNAME. Returns count linked. */
int blockdev_ensure_by_name(void);

/* Resolve partition path, e.g. "vendor" -> /dev/block/by-name/vendor. */
const char *blockdev_by_name(const char *part);

#endif
