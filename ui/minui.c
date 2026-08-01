#include "minui.h"
#include <string.h>

static const uint8_t glyph5x7[][7] = {
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
    ['3'] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    ['6'] = {0x0E,0x10,0x1E,0x11,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    ['N'] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['Y'] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    ['-'] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x04,0x04},
    ['/'] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    ['%'] = {0x19,0x1A,0x04,0x08,0x10,0x13,0x13},
    [':'] = {0x00,0x04,0x00,0x00,0x04,0x00,0x00},
    ['>'] = {0x08,0x04,0x02,0x04,0x08,0x00,0x00},
};

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9')
        return glyph5x7[c];
    if (c >= 'A' && c <= 'Z')
        return glyph5x7[c];
    if (c == '-')
        return glyph5x7['-'];
    if (c == ':')
        return glyph5x7[':'];
    if (c == '.')
        return glyph5x7['.'];
    if (c == '/')
        return glyph5x7['/'];
    if (c == '%')
        return glyph5x7['%'];
    if (c == '>')
        return glyph5x7['>'];
    return NULL;
}

static void px_set(MinuiFb *fb, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || (uint32_t)x >= fb->w || (uint32_t)y >= fb->h)
        return;
    fb->px[y * fb->stride + x] = c;
}

void minui_fill(MinuiFb *fb, uint32_t color)
{
    minui_fill_rect(fb, 0, 0, (int)fb->w, (int)fb->h, color);
}

void minui_fill_rect(MinuiFb *fb, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->w)
        w = (int)fb->w - x;
    if (y + h > (int)fb->h)
        h = (int)fb->h - y;
    if (w <= 0 || h <= 0)
        return;

    for (int j = 0; j < h; j++) {
        uint32_t *row = fb->px + (y + j) * fb->stride + x;
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
}

void minui_rect(MinuiFb *fb, int x, int y, int w, int h, uint32_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px_set(fb, x + i, y + j, color);
}

void minui_rect_outline(MinuiFb *fb, int x, int y, int w, int h, uint32_t color)
{
    minui_rect(fb, x, y, w, 1, color);
    minui_rect(fb, x, y + h - 1, w, 1, color);
    minui_rect(fb, x, y, 1, h, color);
    minui_rect(fb, x + w - 1, y, 1, h, color);
}

void minui_bar(MinuiFb *fb, int x, int y, int w, int h, int pct,
               uint32_t fg, uint32_t bg)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    minui_fill_rect(fb, x, y, w, h, bg);
    int fw = (w * pct + 50) / 100;
    if (fw > 0)
        minui_fill_rect(fb, x, y, fw, h, fg);
}

static void fill_corner_q(MinuiFb *fb, int cx, int cy, int r, int qx, int qy,
                          uint32_t color)
{
    for (int dy = 0; dy <= r; dy++) {
        for (int dx = 0; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r)
                continue;
            px_set(fb, cx + qx * dx, cy + qy * dy, color);
        }
    }
}

void minui_fill_roundrect(MinuiFb *fb, int x, int y, int w, int h, int r,
                          uint32_t color)
{
    if (r < 0)
        r = 0;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r == 0) {
        minui_fill_rect(fb, x, y, w, h, color);
        return;
    }

    minui_fill_rect(fb, x + r, y, w - 2 * r, h, color);
    minui_fill_rect(fb, x, y + r, r, h - 2 * r, color);
    minui_fill_rect(fb, x + w - r, y + r, r, h - 2 * r, color);

    fill_corner_q(fb, x + r, y + r, r, -1, -1, color);
    fill_corner_q(fb, x + w - r - 1, y + r, r, 1, -1, color);
    fill_corner_q(fb, x + r, y + h - r - 1, r, -1, 1, color);
    fill_corner_q(fb, x + w - r - 1, y + h - r - 1, r, 1, 1, color);
}

void minui_roundrect_outline(MinuiFb *fb, int x, int y, int w, int h, int r,
                             uint32_t color)
{
    if (r < 1 || w < 4 || h < 4) {
        minui_rect_outline(fb, x, y, w, h, color);
        return;
    }
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;

    minui_rect(fb, x + r, y, w - 2 * r, 1, color);
    minui_rect(fb, x + r, y + h - 1, w - 2 * r, 1, color);
    minui_rect(fb, x, y + r, 1, h - 2 * r, color);
    minui_rect(fb, x + w - 1, y + r, 1, h - 2 * r, color);

    for (int dy = 0; dy <= r; dy++) {
        for (int dx = 0; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 < (r - 1) * (r - 1) || d2 > r * r)
                continue;
            px_set(fb, x + r - dx, y + r - dy, color);
            px_set(fb, x + w - r - 1 + dx, y + r - dy, color);
            px_set(fb, x + r - dx, y + h - r - 1 + dy, color);
            px_set(fb, x + w - r - 1 + dx, y + h - r - 1 + dy, color);
        }
    }
}

void minui_card(MinuiFb *fb, int x, int y, int w, int h,
                uint32_t fill, uint32_t border)
{
    minui_fill_roundrect(fb, x, y, w, h, 12, fill);
    minui_roundrect_outline(fb, x, y, w, h, 12, border);
}

void minui_bar_round(MinuiFb *fb, int x, int y, int w, int h, int pct, int r,
                     uint32_t fg, uint32_t bg)
{
    if (r > h / 2)
        r = h / 2;
    minui_fill_roundrect(fb, x, y, w, h, r, bg);
    if (pct <= 0)
        return;
    int fw = (w * pct + 50) / 100;
    if (fw < h)
        fw = h;
    if (fw > w)
        fw = w;
    minui_fill_roundrect(fb, x, y, fw, h, r, fg);
}

void minui_draw_corner_bezels(MinuiFb *fb, int inset_l, int inset_t,
                              int inset_r, int inset_b, int r, uint32_t bez)
{
    (void)fb;
    (void)inset_l;
    (void)inset_t;
    (void)inset_r;
    (void)inset_b;
    (void)r;
    (void)bez;
}

void minui_circle(MinuiFb *fb, int cx, int cy, int r, uint32_t color)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                px_set(fb, cx + dx, cy + dy, color);
}

void minui_text(MinuiFb *fb, int x, int y, const char *s, uint32_t color, int scale)
{
    if (scale < 1)
        scale = 1;
    int cx = x;
    for (int i = 0; s[i]; i++) {
        if (s[i] == ' ') {
            cx += 4 * scale;
            continue;
        }
        const uint8_t *g = glyph_for(s[i]);
        if (!g) {
            cx += 4 * scale;
            continue;
        }
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    minui_rect(fb, cx + col * scale, y + row * scale,
                               scale, scale, color);
        cx += 6 * scale;
    }
}

int minui_text_width(const char *s, int scale)
{
    if (scale < 1)
        scale = 1;
    int w = 0;
    for (int i = 0; s[i]; i++)
        w += (s[i] == ' ') ? (4 * scale) : (6 * scale);
    return w;
}

void minui_btn_draw(MinuiFb *fb, MinuiBtn *b)
{
    uint32_t c = b->pressed ? b->press_color : b->color;
    uint32_t fg = b->pressed ? 0xFFE6EDF3 : 0xFFF0F6FC;
    int r = 10;
    minui_fill_roundrect(fb, b->x, b->y, b->w, b->h, r, c);
    minui_roundrect_outline(fb, b->x, b->y, b->w, b->h, r,
                            b->pressed ? 0xFF58A6FF : 0xFF484F58);
    int lw = minui_text_width(b->label, 2);
    minui_text(fb, b->x + (b->w - lw) / 2, b->y + b->h / 2 - 7, b->label, fg, 2);
}

int minui_btn_hit(const MinuiBtn *b, int tx, int ty)
{
    return tx >= b->x && tx < b->x + b->w && ty >= b->y && ty < b->y + b->h;
}
