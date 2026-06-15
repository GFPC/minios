#define _GNU_SOURCE
#include "ui.h"
#include "minui.h"
#include "touch.h"
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* GitHub-dark inspired palette */
#define COL_BG      0xFF0D1117
#define COL_CARD    0xFF161B22
#define COL_BORDER  0xFF30363D
#define COL_HEAD    0xFF0B1929
#define COL_ACCENT  0xFF58A6FF
#define COL_GREEN   0xFF3FB950
#define COL_ORANGE  0xFFD29922
#define COL_RED     0xFFF85149
#define COL_PURPLE  0xFFBC8CFF
#define COL_WHITE   0xFFF0F6FC
#define COL_MUTED   0xFF8B949E
#define COL_TAB_ON  0xFF1F6FEB
#define COL_TAB_OFF 0xFF21262D
#define COL_BEZEL   0xFF000000
#define COL_GLOW    0xFF388BFD33
#define CUR_R       18
#define CUR_PAD     (CUR_R + 4)

enum { TAB_DASH = 0, TAB_POWER, TAB_SYS, TAB_RADIO, TAB_COUNT };
enum { BTN_MAX = 8 };

typedef struct {
    int cpu_pct;
    int ram_pct;
    unsigned long ram_used_mb;
    unsigned long ram_total_mb;
    unsigned long uptime_sec;
    int adb_on;
    int touch_ok;
    int wifi_seen;
    int bt_seen;
} UiMetrics;

static MinuiFb screen;
static MinuiBtn tabs[TAB_COUNT];
static MinuiBtn buttons[BTN_MAX];
static int n_buttons;
static int active_tab = TAB_DASH;
static int touch_fd = -1;
static int touch_ok;
static TouchCal touch_cal;
static int touch_x, touch_y, touch_down;
static int cur_x = -1, cur_y = -1;
static int cur_vis;
static int head_h, tab_h, content_y;
static char status_line[96] = "MiniOS ready";
static char status_prev[96] = "";
static int frame;
static int btn_dirty[BTN_MAX];
static int tab_dirty[TAB_COUNT];
static int content_dirty = 1;
static UiMetrics metrics;
static unsigned long prev_idle, prev_total;
static unsigned long last_metrics_ms;
static int safe_l, safe_t, safe_r, safe_b, corner_r;
static char wifi_line[56] = "WIFI OFF";
static char bt_line[56] = "BT OFF";

static void layout_safe(void)
{
    corner_r = (int)screen.w / 26;
    if (corner_r < 34)
        corner_r = 34;
    if (corner_r > 46)
        corner_r = 46;
    safe_l = safe_r = (int)screen.w / 27;
    if (safe_l < 36)
        safe_l = 36;
    safe_t = (int)screen.h / 27;
    if (safe_t < 80)
        safe_t = 80;
    safe_b = (int)screen.h / 40;
    if (safe_b < 50)
        safe_b = 50;
}

static int pad_x(void)
{
    return safe_l + 20;
}

static int content_w(void)
{
    return (int)screen.w - safe_l - safe_r - 40;
}

static void read_radio_status(void)
{
    char buf[128];
    int fd = open("/tmp/radio.status", O_RDONLY);
    wifi_line[0] = bt_line[0] = '\0';
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = '\0';
        snprintf(wifi_line, sizeof(wifi_line), "%s", buf);
        if (nl[1])
            snprintf(bt_line, sizeof(bt_line), "%s", nl + 1);
    } else {
        snprintf(wifi_line, sizeof(wifi_line), "%s", buf);
    }
}

static void draw_bezel_overlay(void)
{
    minui_draw_corner_bezels(&screen, safe_l, safe_t, safe_r, safe_b,
                             corner_r, COL_BEZEL);
}

static void kmsg(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[128];
        int n = snprintf(b, sizeof(b), "<6>ui: %s\n", s);
        if (n > 0)
            (void)write(fd, b, n);
        close(fd);
    }
}

static void ui_trigger(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

static void vib_short(void)
{
    DIR *d = opendir("/sys/class/leds");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strstr(e->d_name, "vibrator") || strstr(e->d_name, "vib")) {
            char p[128];
            snprintf(p, sizeof(p), "/sys/class/leds/%s/brightness", e->d_name);
            int fd = open(p, O_WRONLY);
            if (fd >= 0) {
                write(fd, "255", 3);
                close(fd);
                usleep(60000);
                fd = open(p, O_WRONLY);
                if (fd >= 0) {
                    write(fd, "0", 1);
                    close(fd);
                }
            }
            break;
        }
    }
    closedir(d);
}

static int sysfs_dir_exists(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return 0;
    closedir(d);
    return 1;
}

static unsigned long read_uptime_sec(void)
{
    char buf[64];
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return (unsigned long)atof(buf);
}

static void read_meminfo(unsigned long *total_kb, unsigned long *avail_kb)
{
    char line[128];
    int fd = open("/proc/meminfo", O_RDONLY);
    *total_kb = 0;
    *avail_kb = 0;
    if (fd < 0)
        return;
    while (read(fd, line, sizeof(line) - 1) > 0) {
        unsigned long v = 0;
        if (!strncmp(line, "MemTotal:", 9))
            sscanf(line + 9, "%lu", &v), *total_kb = v;
        else if (!strncmp(line, "MemAvailable:", 13))
            sscanf(line + 13, "%lu", &v), *avail_kb = v;
        if (*total_kb && *avail_kb)
            break;
    }
    close(fd);
}

static int read_cpu_usage_pct(void)
{
    char buf[256];
    int fd = open("/proc/stat", O_RDONLY);
    unsigned long user, nice, sys, idle, iow, irq, sirq, steal;
    unsigned long total, idle_all;

    if (fd < 0)
        return metrics.cpu_pct;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return metrics.cpu_pct;
    buf[n] = '\0';
    if (sscanf(buf, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
               &user, &nice, &sys, &idle, &iow, &irq, &sirq, &steal) < 4)
        return metrics.cpu_pct;

    idle_all = idle + iow;
    total = user + nice + sys + idle_all + irq + sirq + steal;
    if (prev_total == 0) {
        prev_idle = idle_all;
        prev_total = total;
        return 0;
    }
    unsigned long d_total = total - prev_total;
    unsigned long d_idle = idle_all - prev_idle;
    prev_idle = idle_all;
    prev_total = total;
    if (d_total == 0)
        return metrics.cpu_pct;
    int pct = (int)(100 - (100 * d_idle / d_total));
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}

static void metrics_update(void)
{
    unsigned long total_kb = 0, avail_kb = 0;

    metrics.cpu_pct = read_cpu_usage_pct();
    read_meminfo(&total_kb, &avail_kb);
    if (total_kb > 0) {
        metrics.ram_total_mb = total_kb / 1024;
        metrics.ram_used_mb = (total_kb - avail_kb) / 1024;
        metrics.ram_pct = (int)((metrics.ram_used_mb * 100) / metrics.ram_total_mb);
    }
    metrics.uptime_sec = read_uptime_sec();
    metrics.adb_on = access("/tmp/adb.active", F_OK) == 0 ||
                     access("/tmp/adbd.pid", F_OK) == 0 ||
                     access("/sbin/adbd", X_OK) == 0;
    metrics.touch_ok = touch_ok;
    read_radio_status();
    metrics.wifi_seen = sysfs_dir_exists("/sys/class/net/wlan0") ||
                        (wifi_line[0] && strstr(wifi_line, "wlan"));
    metrics.bt_seen = sysfs_dir_exists("/sys/class/bluetooth/hci0") ||
                      (bt_line[0] && strstr(bt_line, "hci"));
    content_dirty = 1;
}

static void layout_tabs(void)
{
    int pad = 10;
    int x0 = pad_x();
    int tw = (content_w() - pad * (TAB_COUNT - 1)) / TAB_COUNT;
    if (tw < 72)
        tw = 72;
    tab_h = 50;
    head_h = safe_t + 92;
    const char *labels[TAB_COUNT] = { "DASH", "PWR", "SYS", "RAD" };
    for (int i = 0; i < TAB_COUNT; i++) {
        int x = x0 + i * (tw + pad);
        tabs[i] = (MinuiBtn){
            x, head_h, tw, tab_h,
            i == active_tab ? COL_TAB_ON : COL_TAB_OFF,
            COL_BORDER, labels[i], i, 0
        };
    }
    content_y = head_h + tab_h + 14;
}

static void layout_power_buttons(void)
{
    int x0 = pad_x();
    int gap = 14;
    int bw = (content_w() - gap) / 2;
    int bh = 80;
    int y0 = content_y + 16;
    n_buttons = 4;
    buttons[0] = (MinuiBtn){ x0, y0, bw, bh, COL_RED, COL_BORDER, "OFF", 0, 0 };
    buttons[1] = (MinuiBtn){ x0 + bw + gap, y0, bw, bh, COL_ORANGE, COL_BORDER, "REBOOT", 1, 0 };
    buttons[2] = (MinuiBtn){ x0, y0 + bh + gap, bw, bh, COL_ACCENT, COL_BORDER, "FASTBOOT", 2, 0 };
    buttons[3] = (MinuiBtn){ x0 + bw + gap, y0 + bh + gap, bw, bh, COL_PURPLE, COL_BORDER, "RECOVERY", 3, 0 };
}

static void layout_sys_buttons(void)
{
    int x0 = pad_x();
    int cw = content_w();
    n_buttons = 2;
    buttons[0] = (MinuiBtn){ x0, content_y + 250, cw, 68,
                              COL_GREEN, COL_BORDER, "COM MODE", 10, 0 };
    buttons[1] = (MinuiBtn){ x0, content_y + 330, cw, 68,
                              COL_ACCENT, COL_BORDER, "VIB TEST", 11, 0 };
}

static void layout_radio_buttons(void)
{
    int x0 = pad_x();
    n_buttons = 2;
    buttons[0] = (MinuiBtn){ x0, content_y + 200, content_w(), 68,
                              COL_TAB_ON, COL_BORDER, "START RADIO", 20, 0 };
    buttons[1] = (MinuiBtn){ x0, content_y + 280, content_w(), 68,
                              COL_TAB_OFF, COL_BORDER, "PROBE", 21, 0 };
}

static void layout_content_buttons(void)
{
    switch (active_tab) {
    case TAB_POWER:
        layout_power_buttons();
        break;
    case TAB_SYS:
        layout_sys_buttons();
        break;
    case TAB_RADIO:
        layout_radio_buttons();
        break;
    default:
        n_buttons = 0;
        break;
    }
}

static void fmt_uptime(char *out, size_t outsz, unsigned long sec)
{
    unsigned long h = sec / 3600;
    unsigned long m = (sec % 3600) / 60;
    unsigned long s = sec % 60;
    snprintf(out, outsz, "%lu:%02lu:%02lu", h, m, s);
}

static void draw_header(void)
{
    int y0 = safe_t;
    minui_fill_rect(&screen, safe_l, y0, (int)screen.w - safe_l - safe_r, 92,
                    COL_HEAD);
    minui_fill_rect(&screen, safe_l, y0, (int)screen.w - safe_l - safe_r, 4,
                    COL_ACCENT);
    minui_text(&screen, pad_x(), y0 + 14, "MINIOS", COL_WHITE, 3);
    minui_text(&screen, pad_x(), y0 + 52, "CONTROL CENTER", COL_MUTED, 2);
    minui_text(&screen, pad_x(), y0 + 72, status_line, COL_ACCENT, 2);
    strncpy(status_prev, status_line, sizeof(status_prev) - 1);
    status_prev[sizeof(status_prev) - 1] = '\0';
}

static void draw_tabs(void)
{
    for (int i = 0; i < TAB_COUNT; i++) {
        tabs[i].color = (i == active_tab) ? COL_TAB_ON : COL_TAB_OFF;
        minui_btn_draw(&screen, &tabs[i]);
    }
}

static void draw_dash_content(void)
{
    int x0 = pad_x();
    int cw = content_w();
    int y = content_y;
    char line[64];

    minui_card(&screen, x0, y, cw, 108, COL_CARD, COL_BORDER);
    snprintf(line, sizeof(line), "%d%%", metrics.cpu_pct);
    minui_text(&screen, x0 + 16, y + 14, "CPU", COL_MUTED, 2);
    minui_text(&screen, x0 + cw - 80, y + 14, line, COL_WHITE, 2);
    minui_bar_round(&screen, x0 + 16, y + 52, cw - 32, 18, metrics.cpu_pct, 9,
                    COL_ACCENT, 0xFF0D1117);

    y += 120;
    minui_card(&screen, x0, y, cw, 108, COL_CARD, COL_BORDER);
    snprintf(line, sizeof(line), "%lu/%lu MB",
             metrics.ram_used_mb, metrics.ram_total_mb);
    minui_text(&screen, x0 + 16, y + 14, "RAM", COL_MUTED, 2);
    minui_text(&screen, x0 + 16, y + 44, line, COL_WHITE, 2);
    minui_bar_round(&screen, x0 + 16, y + 72, cw - 32, 18, metrics.ram_pct, 9,
                    COL_GREEN, 0xFF0D1117);

    y += 120;
    minui_card(&screen, x0, y, cw, 84, COL_CARD, COL_BORDER);
    char up[32];
    fmt_uptime(up, sizeof(up), metrics.uptime_sec);
    minui_text(&screen, x0 + 16, y + 14, "UPTIME", COL_MUTED, 2);
    minui_text(&screen, x0 + 16, y + 44, up, COL_WHITE, 2);

    y += 96;
    minui_card(&screen, x0, y, cw, 84, COL_CARD, COL_BORDER);
    minui_text(&screen, x0 + 16, y + 14, "USB ADB", COL_MUTED, 2);
    minui_text(&screen, x0 + 16, y + 44,
               metrics.adb_on ? "4EE7 ONLINE" : "OFFLINE", COL_WHITE, 2);
}

static void draw_sys_content(void)
{
    int x0 = pad_x();
    int cw = content_w();
    int y = content_y;
    char line[80];

    minui_card(&screen, x0, y, cw, 220, COL_CARD, COL_BORDER);
    minui_text(&screen, x0 + 16, y + 14, "SYSTEM", COL_MUTED, 2);
    snprintf(line, sizeof(line), "TOUCH %s", metrics.touch_ok ? "OK" : "NO");
    minui_text(&screen, x0 + 16, y + 48, line, COL_WHITE, 2);
    snprintf(line, sizeof(line), "CPU %d%% RAM %d%%",
             metrics.cpu_pct, metrics.ram_pct);
    minui_text(&screen, x0 + 16, y + 84, line, COL_WHITE, 2);
    minui_text(&screen, x0 + 16, y + 120, "ADB 4EE7 ONLINE", COL_WHITE, 2);
    minui_text(&screen, x0 + 16, y + 156, "COM VIA BUTTON", COL_MUTED, 2);
}

static void draw_radio_content(void)
{
    int x0 = pad_x();
    int cw = content_w();
    int y = content_y;
    uint32_t wcol = metrics.wifi_seen ? COL_GREEN : COL_MUTED;
    uint32_t bcol = metrics.bt_seen ? COL_GREEN : COL_MUTED;

    minui_card(&screen, x0, y, cw, 170, COL_CARD, COL_BORDER);
    minui_text(&screen, x0 + 16, y + 14, "WIRELESS", COL_MUTED, 2);
    minui_fill_roundrect(&screen, x0 + 16, y + 48, 10, 10, 5,
                         metrics.wifi_seen ? COL_GREEN : COL_RED);
    minui_text(&screen, x0 + 34, y + 44, wifi_line[0] ? wifi_line : "WIFI OFF",
               wcol, 2);
    minui_fill_roundrect(&screen, x0 + 16, y + 84, 10, 10, 5,
                         metrics.bt_seen ? COL_GREEN : COL_RED);
    minui_text(&screen, x0 + 34, y + 80, bt_line[0] ? bt_line : "BT OFF",
               bcol, 2);
    minui_text(&screen, x0 + 16, y + 120, "TAP START RADIO", COL_MUTED, 2);
}

static void draw_content(void)
{
    int y0 = content_y - 8;
    int h = (int)screen.h - y0;
    minui_fill_rect(&screen, 0, y0, (int)screen.w, h, COL_BG);

    switch (active_tab) {
    case TAB_DASH:
        draw_dash_content();
        break;
    case TAB_SYS:
        draw_sys_content();
        break;
    case TAB_RADIO:
        draw_radio_content();
        break;
    default:
        break;
    }

    for (int i = 0; i < n_buttons; i++)
        minui_btn_draw(&screen, &buttons[i]);
}

static void draw_static_scene(void)
{
    minui_fill(&screen, COL_BEZEL);
    minui_fill_rect(&screen, safe_l, safe_t,
                    (int)screen.w - safe_l - safe_r,
                    (int)screen.h - safe_t - safe_b, COL_BG);
    draw_header();
    draw_tabs();
    layout_content_buttons();
    draw_content();
    draw_bezel_overlay();
    content_dirty = 0;
}

static int rects_overlap(int ax, int ay, int aw, int ah,
                         int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static void restore_rect(int x, int y, int w, int h)
{
    if (content_dirty)
        return;
    minui_fill_rect(&screen, x, y, w, h, COL_BG);

    if (rects_overlap(x, y, w, h, 0, 0, (int)screen.w, head_h)) {
        draw_header();
    }
    if (rects_overlap(x, y, w, h, 0, head_h, (int)screen.w, tab_h + 16)) {
        draw_tabs();
    }
    if (y + h > content_y) {
        switch (active_tab) {
        case TAB_DASH:
            draw_dash_content();
            break;
        case TAB_SYS:
            draw_sys_content();
            break;
        case TAB_RADIO:
            draw_radio_content();
            break;
        default:
            break;
        }
    }
    for (int i = 0; i < n_buttons; i++) {
        MinuiBtn *b = &buttons[i];
        if (rects_overlap(x, y, w, h, b->x, b->y, b->w, b->h))
            minui_btn_draw(&screen, b);
    }
}

static void erase_cursor(void)
{
    if (!cur_vis || cur_x < 0)
        return;
    restore_rect(cur_x - CUR_PAD, cur_y - CUR_PAD, CUR_PAD * 2, CUR_PAD * 2);
    cur_vis = 0;
}

static void draw_cursor(int x, int y)
{
    minui_circle(&screen, x, y, CUR_R, 0xAA58A6FF);
    minui_fill_rect(&screen, x - 2, y - 2, 5, 5, COL_WHITE);
    cur_x = x;
    cur_y = y;
    cur_vis = 1;
}

static void on_tab(int id)
{
    if (id < 0 || id >= TAB_COUNT || id == active_tab)
        return;
    vib_short();
    active_tab = id;
    layout_tabs();
    layout_content_buttons();
    content_dirty = 1;
    draw_static_scene();
    kmsg("tab switch");
}

static void on_button(int id)
{
    vib_short();
    switch (id) {
    case 0:
        snprintf(status_line, sizeof(status_line), "Power off...");
        ui_trigger("/tmp/power.off");
        kmsg("power off");
        break;
    case 1:
        snprintf(status_line, sizeof(status_line), "Rebooting...");
        ui_trigger("/tmp/reboot.warm");
        kmsg("reboot");
        break;
    case 2:
        snprintf(status_line, sizeof(status_line), "Fastboot...");
        ui_trigger("/tmp/reboot.bootloader");
        kmsg("fastboot");
        break;
    case 3:
        snprintf(status_line, sizeof(status_line), "Recovery...");
        ui_trigger("/tmp/reboot.recovery");
        kmsg("recovery");
        break;
    case 10:
        ui_trigger("/tmp/com.on");
        snprintf(status_line, sizeof(status_line), "COM mode soon");
        kmsg("com-on");
        break;
    case 11:
        vib_short();
        snprintf(status_line, sizeof(status_line), "VIB OK");
        kmsg("vib");
        break;
    case 20:
        ui_trigger("/tmp/radio.start");
        snprintf(status_line, sizeof(status_line), "Radio starting...");
        kmsg("radio start");
        break;
    case 21:
        ui_trigger("/tmp/radio.probe");
        metrics_update();
        snprintf(status_line, sizeof(status_line), "Radio probe");
        kmsg("radio probe");
        break;
    default:
        break;
    }
}

int ui_touch_fd(void)
{
    return touch_fd;
}

void ui_init(UiDrm *ctx)
{
    screen = (MinuiFb){
        .px = ctx->fb_mem,
        .w = ctx->w,
        .h = ctx->h,
        .stride = ctx->pitch / 4,
    };
    head_h = 110;
    layout_safe();
    layout_tabs();
    touch_fd = touch_open();
    touch_ok = touch_fd >= 0;
    metrics.touch_ok = touch_ok;
    if (touch_ok) {
        touch_get_cal(touch_fd, &touch_cal);
        touch_cal.screen_w = (int)ctx->w;
        touch_cal.screen_h = (int)ctx->h;
    }
    metrics_update();
    last_metrics_ms = 0;
    draw_static_scene();
    ctx->dirty = 0;
    kmsg(touch_ok ? "dashboard init ok" : "dashboard no touch");
}

int ui_tick(UiDrm *ctx)
{
    (void)ctx;
    int timeout_ms = 1000;
    int moved = 0;
    unsigned long now_ms = (unsigned long)(frame * 33);

    if (touch_fd < 0 && (frame % 120) == 0) {
        touch_fd = touch_open();
        touch_ok = touch_fd >= 0;
        metrics.touch_ok = touch_ok;
        if (touch_ok) {
            touch_get_cal(touch_fd, &touch_cal);
            touch_cal.screen_w = (int)screen.w;
            touch_cal.screen_h = (int)screen.h;
            content_dirty = 1;
        }
    }

    if (now_ms - last_metrics_ms >= 1000) {
        metrics_update();
        last_metrics_ms = now_ms;
        if (active_tab == TAB_DASH || active_tab == TAB_SYS)
            draw_content();
        moved = 1;
    }

    if (touch_fd >= 0) {
        struct pollfd pfd = { .fd = touch_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            timeout_ms = 33;
            int x, y, down;
            while (touch_read_timeout(touch_fd, &touch_cal, &x, &y, &down, 0)) {
                int pos_new = down || (x != touch_x || y != touch_y);
                touch_x = x;
                touch_y = y;
                if (down != touch_down && !down) {
                    for (int i = 0; i < TAB_COUNT; i++) {
                        if (tabs[i].pressed) {
                            on_tab(tabs[i].id);
                            tabs[i].pressed = 0;
                            tab_dirty[i] = 1;
                        }
                    }
                    for (int i = 0; i < n_buttons; i++) {
                        if (buttons[i].pressed) {
                            on_button(buttons[i].id);
                            buttons[i].pressed = 0;
                            btn_dirty[i] = 1;
                        }
                    }
                }
                touch_down = down;
                if (down) {
                    for (int i = 0; i < TAB_COUNT; i++) {
                        int hit = minui_btn_hit(&tabs[i], touch_x, touch_y);
                        if (hit != tabs[i].pressed) {
                            tabs[i].pressed = hit;
                            tab_dirty[i] = 1;
                        }
                    }
                    for (int i = 0; i < n_buttons; i++) {
                        int hit = minui_btn_hit(&buttons[i], touch_x, touch_y);
                        if (hit != buttons[i].pressed) {
                            buttons[i].pressed = hit;
                            btn_dirty[i] = 1;
                        }
                    }
                }
                if (pos_new) {
                    erase_cursor();
                    draw_cursor(touch_x, touch_y);
                    moved = 1;
                }
            }
        }
    }

    for (int i = 0; i < TAB_COUNT; i++) {
        if (tab_dirty[i]) {
            minui_btn_draw(&screen, &tabs[i]);
            tab_dirty[i] = 0;
            moved = 1;
        }
    }
    for (int i = 0; i < n_buttons; i++) {
        if (btn_dirty[i]) {
            minui_btn_draw(&screen, &buttons[i]);
            btn_dirty[i] = 0;
            moved = 1;
        }
    }

    if (strcmp(status_line, status_prev) != 0) {
        minui_fill_rect(&screen, 0, head_h - 40, (int)screen.w, 32, COL_HEAD);
        minui_text(&screen, 28, head_h - 36, status_line, COL_ACCENT, 2);
        strncpy(status_prev, status_line, sizeof(status_prev) - 1);
        status_prev[sizeof(status_prev) - 1] = '\0';
        moved = 1;
    }

    frame++;
    return moved ? 33 : timeout_ms;
}
