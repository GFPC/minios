#ifndef MINIOS_PLOG_H
#define MINIOS_PLOG_H

#include <stddef.h>

/* Initialize persistent logging: mount SD/persist, open log file, start kmsg drain. */
void plog_init(void);

/* Periodic heartbeat poll — call from main loop (~15s interval). */
void plog_poll(void);

/* Return last N bytes of the boot log for COM display. */
int plog_format_tail(char *buf, size_t bufsz, size_t max_bytes);

/* Return absolute path to the current log file, or "" if no storage found. */
const char *plog_path(void);

/* Append a plain text string to the persistent log (no-op if no storage). */
void plog_append(const char *s);

/* Copy /tmp/*.log radio/cnss/qrtr/pd log files to the persistent log directory. */
void plog_save_tmp_logs(void);

/* Copy /sys/fs/pstore/* into log_dir/pstore/ if mounted. */
void plog_save_pstore(void);

#endif /* MINIOS_PLOG_H */
