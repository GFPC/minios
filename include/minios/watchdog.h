#ifndef MINIOS_WATCHDOG_H
#define MINIOS_WATCHDOG_H

extern int wdt_fd;
extern int wdt_msm_disabled;
void wdt_open(void);
void wdt_pet(void);

#endif
