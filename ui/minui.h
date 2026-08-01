#ifndef MINUI_H
#define MINUI_H

#include <stdint.h>

typedef struct {
    uint32_t *px;
    uint32_t  w, h, stride;
} MinuiFb;

typedef struct {
    int x, y, w, h;
    uint32_t color;
    uint32_t press_color;
    const char *label;
    int id;
    int pressed;
} MinuiBtn;

void minui_fill(MinuiFb *fb, uint32_t color);
void minui_fill_rect(MinuiFb *fb, int x, int y, int w, int h, uint32_t color);
void minui_rect(MinuiFb *fb, int x, int y, int w, int h, uint32_t color);
void minui_rect_outline(MinuiFb *fb, int x, int y, int w, int h, uint32_t color);
void minui_bar(MinuiFb *fb, int x, int y, int w, int h, int pct,
               uint32_t fg, uint32_t bg);
void minui_card(MinuiFb *fb, int x, int y, int w, int h,
                uint32_t fill, uint32_t border);
void minui_fill_roundrect(MinuiFb *fb, int x, int y, int w, int h, int r,
                          uint32_t color);
void minui_roundrect_outline(MinuiFb *fb, int x, int y, int w, int h, int r,
                             uint32_t color);
void minui_bar_round(MinuiFb *fb, int x, int y, int w, int h, int pct, int r,
                     uint32_t fg, uint32_t bg);
void minui_draw_corner_bezels(MinuiFb *fb, int inset_l, int inset_t,
                              int inset_r, int inset_b, int r, uint32_t bez);
void minui_circle(MinuiFb *fb, int cx, int cy, int r, uint32_t color);
void minui_text(MinuiFb *fb, int x, int y, const char *s, uint32_t color, int scale);
int  minui_text_width(const char *s, int scale);
void minui_btn_draw(MinuiFb *fb, MinuiBtn *b);
int  minui_btn_hit(const MinuiBtn *b, int tx, int ty);

#endif
