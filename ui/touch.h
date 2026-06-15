#ifndef MINUI_TOUCH_H
#define MINUI_TOUCH_H

#include <stdint.h>

typedef struct {
    int min_x, max_x, min_y, max_y;
    int inv_x, inv_y, swap_xy;
    int screen_w, screen_h;
} TouchCal;

void touch_ensure_nodes(void);
int  touch_open(void);
void touch_close(int fd);
void touch_get_cal(int fd, TouchCal *cal);
void touch_map(const TouchCal *cal, int raw_x, int raw_y, int *out_x, int *out_y);
/* Returns 1 if event read. *down=1 pressed, 0 released. */
int  touch_read(int fd, int *x, int *y, int *down);
/* Block up to ms for one mapped event; returns 1 on event. */
int  touch_read_timeout(int fd, TouchCal *cal, int *x, int *y, int *down, int ms);
const char *touch_name(void);

#endif
