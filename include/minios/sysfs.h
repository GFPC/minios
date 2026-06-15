#ifndef MINIOS_SYSFS_H
#define MINIOS_SYSFS_H

void sysfs_write(const char *path, const char *val);
void sysfs_mkdir(const char *path);

#endif
