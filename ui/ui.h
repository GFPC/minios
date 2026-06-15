#ifndef MINUI_UI_H
#define MINUI_UI_H

#include <stdint.h>

typedef struct {
    uint32_t *fb_mem;
    uint32_t  fb_id;
    uint32_t  w, h, pitch;
    uint32_t  plane_id, crtc_id, conn_id;
    int       atomic_ok;
    int       dirty;
} UiDrm;

void ui_init(UiDrm *ctx);
/* Poll timeout ms for next frame (0 = block until input). */
int  ui_tick(UiDrm *ctx);
int  ui_touch_fd(void);

#endif
