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
#include <time.h>
#include <unistd.h>

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
#define COL_LOG_BG  0xFF010409
#define COL_LOG_HDR 0xFF161B22
#define CUR_R       14
#define CUR_PAD     (CUR_R + 4)
#define LOG_LINES   6
#define LOG_COLS    56
#define PAD         16
#define METRICS_MS  3000

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
static int head_h, tab_h, content_y, content_h, log_h, log_y, bottom_safe;
static char status_line[96] = "MINIOS READY";
static char status_prev[96] = "";
static int frame;
static int btn_dirty[BTN_MAX];
static int tab_dirty[TAB_COUNT];
static int content_dirty = 1;
static int log_dirty = 1;
static UiMetrics metrics;
static unsigned long prev_idle, prev_total;
static int cpu_warmed;
static unsigned long last_metrics_ms;
static char log_ring[LOG_LINES][LOG_COLS];
static int log_head;
static int log_kmsg_fd = -1;
static unsigned long last_kmsg_poll_ms;
static char wifi_line[56] = "WIFI OFF";
static char bt_line[56] = "BT OFF";
static UiMetrics metrics_prev;
static int dash_layout_done;
static int tile_cpu[4], tile_ram[4], tile_up[4], tile_adb[4], tile_rf[4];

static int pad_x(void)
{
    return PAD;
}

static int content_w(void)
{
    return (int)screen.w - PAD * 2;
}

static unsigned long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return (unsigned long)(frame * 33);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)ts.tv_nsec / 1000000UL;
}

static void layout_metrics(void)
{
    bottom_safe = (int)screen.h / 14;
    if (bottom_safe < 48)
        bottom_safe = 48;
    head_h = 68;
    tab_h = 40;
    log_h = 24 + LOG_LINES * 18;
    log_y = head_h + tab_h + 6;
    content_y = log_y + log_h + 10;
    content_h = (int)screen.h - content_y - bottom_safe;
    if (content_h < 100)
        content_h = 100;
}

static void log_push(const char *msg)
{
    char line[LOG_COLS];
    int i, j;

    if (!msg || !msg[0])
        return;
    for (i = 0; msg[i] == ' '; i++)
        ;
    if (msg[i] == '<') {
        while (msg[i] && msg[i] != '>')
            i++;
        if (msg[i] == '>')
            i++;
        while (msg[i] == ' ')
            i++;
    }
    for (j = 0; j < LOG_COLS - 1 && msg[i]; i++) {
        char c = msg[i];
        if (c == '\n' || c == '\r')
            break;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        line[j++] = c;
    }
    line[j] = '\0';
    if (!line[0])
        return;

    log_head = (log_head + 1) % LOG_LINES;
    if (strcmp(log_ring[log_head], line) != 0) {
        snprintf(log_ring[log_head], LOG_COLS, "%s", line);
        log_dirty = 1;
    }
}

static void log_poll_kmsg(void)
{
    char buf[512];
    ssize_t n;
    unsigned long now = monotonic_ms();

    if (now - last_kmsg_poll_ms < 400UL)
        return;
    last_kmsg_poll_ms = now;

    if (log_kmsg_fd < 0) {
        log_kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (log_kmsg_fd < 0)
            return;
    }
    while ((n = read(log_kmsg_fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        char *p = buf, *nl;
        while (p && *p) {
            nl = strchr(p, '\n');
            if (nl)
                *nl = '\0';
            log_push(p);
            p = nl ? nl + 1 : NULL;
        }
    }
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

static void kmsg_ui(const char *s)
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
    double up;
    int fd = open("/proc/uptime", O_RDONLY);

    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    if (sscanf(buf, "%lf", &up) != 1)
        return 0;
    return (unsigned long)up;
}

static void read_meminfo(unsigned long *total_kb, unsigned long *avail_kb,
                         unsigned long *free_kb)
{
    char buf[2048];
    int fd = open("/proc/meminfo", O_RDONLY);

    *total_kb = *avail_kb = *free_kb = 0;
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        unsigned long v = 0;
        if (!strncmp(p, "MemTotal:", 9))
            sscanf(p + 9, "%lu", &v), *total_kb = v;
        else if (!strncmp(p, "MemAvailable:", 13))
            sscanf(p + 13, "%lu", &v), *avail_kb = v;
        else if (!strncmp(p, "MemFree:", 8))
            sscanf(p + 8, "%lu", &v), *free_kb = v;
        p = nl ? nl + 1 : NULL;
    }
    if (*avail_kb == 0 && *free_kb > 0)
        *avail_kb = *free_kb;
}

static int read_cpu_usage_pct(void)
{
    char buf[256];
    int fd = open("/proc/stat", O_RDONLY);
    unsigned long user, nice, sys, idle, iow, irq, sirq, steal;
    unsigned long total, idle_all, d_total, d_idle;

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
        cpu_warmed = 0;
        return 0;
    }

    d_total = total - prev_total;
    d_idle = idle_all - prev_idle;
    prev_idle = idle_all;
    prev_total = total;
    if (d_total == 0)
        return metrics.cpu_pct;

    cpu_warmed = 1;
    int pct = (int)((100UL * (d_total - d_idle) + d_total / 2) / d_total);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}

static void metrics_update(void)
{
    unsigned long total_kb = 0, avail_kb = 0, free_kb = 0;

    metrics.cpu_pct = read_cpu_usage_pct();
    read_meminfo(&total_kb, &avail_kb, &free_kb);
    if (total_kb > 0) {
        unsigned long used_kb = total_kb - avail_kb;
        metrics.ram_total_mb = (total_kb + 512) / 1024;
        metrics.ram_used_mb = (used_kb + 512) / 1024;
        metrics.ram_pct = (int)((used_kb * 100 + total_kb / 2) / total_kb);
    }
    metrics.uptime_sec = read_uptime_sec();
    metrics.adb_on = access("/tmp/adbd.pid", F_OK) == 0 ||
                     access("/tmp/adb.active", F_OK) == 0;
    metrics.touch_ok = touch_ok;
    read_radio_status();
    metrics.wifi_seen = sysfs_dir_exists("/sys/class/net/wlan0") ||
                        (wifi_line[0] && strstr(wifi_line, "WLAN"));
    metrics.bt_seen = sysfs_dir_exists("/sys/class/bluetooth/hci0") ||
                      (bt_line[0] && strstr(bt_line, "HCI"));
}

static void layout_tabs(void)
{
    int x0 = pad_x();
    int gap = 8;
    int tw = (content_w() - gap * (TAB_COUNT - 1)) / TAB_COUNT;
    if (tw < 64)
        tw = 64;
    const char *labels[TAB_COUNT] = { "DASH", "PWR", "SYS", "RAD" };
    for (int i = 0; i < TAB_COUNT; i++) {
        tabs[i] = (MinuiBtn){
            x0 + i * (tw + gap), head_h, tw, tab_h,
            i == active_tab ? COL_TAB_ON : COL_TAB_OFF,
            COL_BORDER, labels[i], i, 0
        };
    }
}

static void layout_power_buttons(void)
{
    int x0 = pad_x();
    int gap = 12;
    int bw = (content_w() - gap) / 2;
    int bh = 68;
    int y0 = content_y + 12;
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
    buttons[0] = (MinuiBtn){ x0, content_y + content_h - 140, cw, 56,
                              COL_GREEN, COL_BORDER, "COM MODE", 10, 0 };
    buttons[1] = (MinuiBtn){ x0, content_y + content_h - 72, cw, 56,
                              COL_ACCENT, COL_BORDER, "VIB TEST", 11, 0 };
}

static void layout_radio_buttons(void)
{
    int x0 = pad_x();
    n_buttons = 2;
    buttons[0] = (MinuiBtn){ x0, content_y + content_h - 140, content_w(), 56,
                              COL_TAB_ON, COL_BORDER, "START RADIO", 20, 0 };
    buttons[1] = (MinuiBtn){ x0, content_y + content_h - 72, content_w(), 56,
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
    unsigned long d = sec / 86400;
    unsigned long h = (sec % 86400) / 3600;
    unsigned long m = (sec % 3600) / 60;
    unsigned long s = sec % 60;
    if (d > 0)
        snprintf(out, outsz, "%lud %02lu:%02lu:%02lu", d, h, m, s);
    else
        snprintf(out, outsz, "%02lu:%02lu:%02lu", h, m, s);
}

static void draw_header(void)
{
    minui_fill_rect(&screen, 0, 0, (int)screen.w, head_h, COL_HEAD);
    minui_fill_rect(&screen, 0, head_h - 3, (int)screen.w, 3, COL_ACCENT);
    minui_text(&screen, pad_x(), 12, "MINIOS", COL_WHITE, 3);
    minui_text(&screen, pad_x() + 120, 18, "CONTROL", COL_MUTED, 2);
    minui_text(&screen, pad_x(), 46, status_line, COL_ACCENT, 2);
    strncpy(status_prev, status_line, sizeof(status_prev) - 1);
    status_prev[sizeof(status_prev) - 1] = '\0';
}

static void draw_tabs(void)
{
    minui_fill_rect(&screen, 0, head_h, (int)screen.w, tab_h + 8, COL_BG);
    for (int i = 0; i < TAB_COUNT; i++) {
        tabs[i].color = (i == active_tab) ? COL_TAB_ON : COL_TAB_OFF;
        minui_btn_draw(&screen, &tabs[i]);
    }
}

static int metrics_changed(void)
{
    return memcmp(&metrics, &metrics_prev, sizeof(metrics)) != 0;
}

static void metrics_snapshot(void)
{
    metrics_prev = metrics;
}

static void erase_tile(int x, int y, int w, int h)
{
    minui_fill_rect(&screen, x, y, w, h, COL_BG);
}

static void draw_stat_tile(int x, int y, int w, int h, const char *title,
                           const char *value, int pct, uint32_t bar_col)
{
    minui_card(&screen, x, y, w, h, COL_CARD, COL_BORDER);
    minui_text(&screen, x + 12, y + 8, title, COL_MUTED, 2);
    minui_text(&screen, x + 12, y + 28, value, COL_WHITE, 2);
    if (pct >= 0)
        minui_bar_round(&screen, x + 12, y + h - 20, w - 24, 8, pct, 4,
                        bar_col, 0xFF21262D);
}

static void dash_layout_tiles(void)
{
    int x0 = pad_x();
    int cw = content_w();
    int gap = 8;
    int half = (cw - gap) / 2;
    int tile_h = 64;
    int y = content_y + 2;

    tile_cpu[0] = x0; tile_cpu[1] = y; tile_cpu[2] = half; tile_cpu[3] = tile_h;
    tile_ram[0] = x0 + half + gap; tile_ram[1] = y; tile_ram[2] = half; tile_ram[3] = tile_h;
    y += tile_h + gap;
    tile_up[0] = x0; tile_up[1] = y; tile_up[2] = half; tile_up[3] = tile_h;
    tile_adb[0] = x0 + half + gap; tile_adb[1] = y; tile_adb[2] = half; tile_adb[3] = tile_h;
    y += tile_h + gap;
    tile_rf[0] = x0; tile_rf[1] = y; tile_rf[2] = cw; tile_rf[3] = 48;
    dash_layout_done = 1;
}

static void draw_dash_content(void)
{
    int x0 = pad_x();
    int cw = content_w();
    char val[48];

    if (!dash_layout_done)
        dash_layout_tiles();

    snprintf(val, sizeof(val), cpu_warmed ? "%d%%" : "--%%", metrics.cpu_pct);
    draw_stat_tile(tile_cpu[0], tile_cpu[1], tile_cpu[2], tile_cpu[3],
                   "CPU", val, metrics.cpu_pct, COL_ACCENT);

    snprintf(val, sizeof(val), "%lu/%lu MB",
             metrics.ram_used_mb, metrics.ram_total_mb);
    draw_stat_tile(tile_ram[0], tile_ram[1], tile_ram[2], tile_ram[3],
                   "RAM", val, metrics.ram_pct, COL_GREEN);

    fmt_uptime(val, sizeof(val), metrics.uptime_sec);
    draw_stat_tile(tile_up[0], tile_up[1], tile_up[2], tile_up[3],
                   "UPTIME", val, -1, COL_ACCENT);

    snprintf(val, sizeof(val), metrics.adb_on ? "TCP ON" : "ADB OFF");
    draw_stat_tile(tile_adb[0], tile_adb[1], tile_adb[2], tile_adb[3],
                   "ADB", val, -1, metrics.adb_on ? COL_GREEN : COL_MUTED);

    erase_tile(tile_rf[0], tile_rf[1], tile_rf[2], tile_rf[3]);
    minui_card(&screen, tile_rf[0], tile_rf[1], tile_rf[2], tile_rf[3],
               COL_CARD, COL_BORDER);
    minui_fill_roundrect(&screen, x0 + 14, tile_rf[1] + 16, 8, 8, 4,
                         metrics.wifi_seen ? COL_GREEN : COL_RED);
    minui_text(&screen, x0 + 28, tile_rf[1] + 10,
               wifi_line[0] ? wifi_line : "WIFI IDLE", COL_WHITE, 2);
    minui_fill_roundrect(&screen, x0 + cw / 2 + 8, tile_rf[1] + 16, 8, 8, 4,
                         metrics.bt_seen ? COL_GREEN : COL_RED);
    minui_text(&screen, x0 + cw / 2 + 22, tile_rf[1] + 10,
               bt_line[0] ? bt_line : "BT IDLE", COL_WHITE, 2);
}

static void draw_dash_delta(void)
{
    char val[48];

    if (!dash_layout_done)
        dash_layout_tiles();

    if (metrics.cpu_pct != metrics_prev.cpu_pct ||
        metrics.ram_pct != metrics_prev.ram_pct) {
        snprintf(val, sizeof(val), cpu_warmed ? "%d%%" : "--%%", metrics.cpu_pct);
        erase_tile(tile_cpu[0], tile_cpu[1], tile_cpu[2], tile_cpu[3]);
        draw_stat_tile(tile_cpu[0], tile_cpu[1], tile_cpu[2], tile_cpu[3],
                       "CPU", val, metrics.cpu_pct, COL_ACCENT);
    }
    if (metrics.ram_used_mb != metrics_prev.ram_used_mb ||
        metrics.ram_total_mb != metrics_prev.ram_total_mb ||
        metrics.ram_pct != metrics_prev.ram_pct) {
        snprintf(val, sizeof(val), "%lu/%lu MB",
                 metrics.ram_used_mb, metrics.ram_total_mb);
        erase_tile(tile_ram[0], tile_ram[1], tile_ram[2], tile_ram[3]);
        draw_stat_tile(tile_ram[0], tile_ram[1], tile_ram[2], tile_ram[3],
                       "RAM", val, metrics.ram_pct, COL_GREEN);
    }
    if (metrics.uptime_sec != metrics_prev.uptime_sec) {
        fmt_uptime(val, sizeof(val), metrics.uptime_sec);
        erase_tile(tile_up[0], tile_up[1], tile_up[2], tile_up[3]);
        draw_stat_tile(tile_up[0], tile_up[1], tile_up[2], tile_up[3],
                       "UPTIME", val, -1, COL_ACCENT);
    }
    if (metrics.adb_on != metrics_prev.adb_on) {
        snprintf(val, sizeof(val), metrics.adb_on ? "TCP ON" : "ADB OFF");
        erase_tile(tile_adb[0], tile_adb[1], tile_adb[2], tile_adb[3]);
        draw_stat_tile(tile_adb[0], tile_adb[1], tile_adb[2], tile_adb[3],
                       "ADB", val, -1, metrics.adb_on ? COL_GREEN : COL_MUTED);
    }
    if (metrics.wifi_seen != metrics_prev.wifi_seen ||
        metrics.bt_seen != metrics_prev.bt_seen) {
        erase_tile(tile_rf[0], tile_rf[1], tile_rf[2], tile_rf[3]);
        int x0 = pad_x(), cw = content_w();
        minui_card(&screen, tile_rf[0], tile_rf[1], tile_rf[2], tile_rf[3],
                   COL_CARD, COL_BORDER);
        minui_fill_roundrect(&screen, x0 + 14, tile_rf[1] + 16, 8, 8, 4,
                             metrics.wifi_seen ? COL_GREEN : COL_RED);
        minui_text(&screen, x0 + 28, tile_rf[1] + 10,
                   wifi_line[0] ? wifi_line : "WIFI IDLE", COL_WHITE, 2);
        minui_fill_roundrect(&screen, x0 + cw / 2 + 8, tile_rf[1] + 16, 8, 8, 4,
                             metrics.bt_seen ? COL_GREEN : COL_RED);
        minui_text(&screen, x0 + cw / 2 + 22, tile_rf[1] + 10,
                   bt_line[0] ? bt_line : "BT IDLE", COL_WHITE, 2);
    }
}

static void draw_sys_content(void)
{
    int x0 = pad_x();
    int cw = content_w();
    int y = content_y + 8;
    char line[80];

    minui_card(&screen, x0, y, cw, 110, COL_CARD, COL_BORDER);
    minui_text(&screen, x0 + 12, y + 10, "SYSTEM INFO", COL_MUTED, 2);
    snprintf(line, sizeof(line), "TOUCH %s", metrics.touch_ok ? "OK" : "NO");
    minui_text(&screen, x0 + 12, y + 36, line, COL_WHITE, 2);
    fmt_uptime(line, sizeof(line), metrics.uptime_sec);
    minui_text(&screen, x0 + 12, y + 60, line, COL_WHITE, 2);
    snprintf(line, sizeof(line), "CPU %d%%  RAM %d%%",
             metrics.cpu_pct, metrics.ram_pct);
    minui_text(&screen, x0 + 12, y + 84, line, COL_WHITE, 2);
    snprintf(line, sizeof(line), "ADB %s", metrics.adb_on ? "ACTIVE" : "IDLE");
    minui_text(&screen, x0 + 12, y + 108, line, COL_WHITE, 2);
}

static void draw_radio_content(void)
{
    int x0 = pad_x();
    int cw = content_w();
    int y = content_y + 8;

    minui_card(&screen, x0, y, cw, 120, COL_CARD, COL_BORDER);
    minui_text(&screen, x0 + 12, y + 10, "WIRELESS", COL_MUTED, 2);
    minui_fill_roundrect(&screen, x0 + 14, y + 40, 8, 8, 4,
                         metrics.wifi_seen ? COL_GREEN : COL_RED);
    minui_text(&screen, x0 + 28, y + 34,
               wifi_line[0] ? wifi_line : "WIFI OFF", COL_WHITE, 2);
    minui_fill_roundrect(&screen, x0 + 14, y + 72, 8, 8, 4,
                         metrics.bt_seen ? COL_GREEN : COL_RED);
    minui_text(&screen, x0 + 28, y + 66,
               bt_line[0] ? bt_line : "BT OFF", COL_WHITE, 2);
}

static void draw_content(void)
{
    minui_fill_rect(&screen, 0, content_y, (int)screen.w, content_h, COL_BG);

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

static void draw_log_panel(void)
{
    int x0 = pad_x();
    int cw = content_w();

    minui_fill_rect(&screen, 0, log_y - 2, (int)screen.w, log_h + 4, COL_BG);
    minui_fill_roundrect(&screen, x0, log_y, cw, log_h, 10, COL_LOG_BG);
    minui_roundrect_outline(&screen, x0, log_y, cw, log_h, 10, COL_ACCENT);
    minui_fill_rect(&screen, x0 + 1, log_y + 1, cw - 2, 20, COL_LOG_HDR);
    minui_text(&screen, x0 + 10, log_y + 4, "KERNEL LOG", COL_ACCENT, 2);

    int ly = log_y + 24;
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = (log_head + 1 + i) % LOG_LINES;
        const char *line = log_ring[idx];
        if (!line[0])
            continue;
        minui_text(&screen, x0 + 8, ly, ">", COL_GREEN, 2);
        minui_text(&screen, x0 + 20, ly, line, COL_WHITE, 2);
        ly += 18;
    }
    log_dirty = 0;
}

static void draw_log_delta(void)
{
    int x0 = pad_x();
    int cw = content_w();
    int ly = log_y + 24;

    minui_fill_rect(&screen, x0 + 1, log_y + 22, cw - 2, log_h - 24, COL_LOG_BG);
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = (log_head + 1 + i) % LOG_LINES;
        const char *line = log_ring[idx];
        minui_fill_rect(&screen, x0 + 8, ly, cw - 16, 16, COL_LOG_BG);
        if (line[0]) {
            minui_text(&screen, x0 + 8, ly, ">", COL_GREEN, 2);
            minui_text(&screen, x0 + 20, ly, line, COL_WHITE, 2);
        }
        ly += 18;
    }
    log_dirty = 0;
}

static void draw_static_scene(void)
{
    minui_fill(&screen, COL_BG);
    draw_header();
    draw_tabs();
    draw_log_panel();
    layout_content_buttons();
    dash_layout_done = 0;
    draw_content();
    if (bottom_safe > 0)
        minui_fill_rect(&screen, 0, (int)screen.h - bottom_safe,
                        (int)screen.w, bottom_safe, COL_BG);
    content_dirty = 0;
    metrics_snapshot();
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

    if (rects_overlap(x, y, w, h, 0, log_y, (int)screen.w, log_h + 4)) {
        draw_log_delta();
        return;
    }

    if (rects_overlap(x, y, w, h, 0, content_y, (int)screen.w, content_h)) {
        if (active_tab == TAB_DASH)
            draw_dash_content();
        else if (active_tab == TAB_SYS)
            draw_sys_content();
        else if (active_tab == TAB_RADIO)
            draw_radio_content();
        for (int i = 0; i < n_buttons; i++)
            minui_btn_draw(&screen, &buttons[i]);
    }
    if (rects_overlap(x, y, w, h, 0, 0, (int)screen.w, head_h))
        draw_header();
    if (rects_overlap(x, y, w, h, 0, head_h, (int)screen.w, tab_h + 8))
        draw_tabs();
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
    if (y >= content_y + content_h - CUR_PAD)
        return;
    minui_circle(&screen, x, y, CUR_R, 0x8858A6FF);
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
    dash_layout_done = 0;
    content_dirty = 1;
    draw_static_scene();
    kmsg_ui("tab switch");
}

static void on_button(int id)
{
    vib_short();
    switch (id) {
    case 0:
        snprintf(status_line, sizeof(status_line), "POWER OFF...");
        ui_trigger("/tmp/power.off");
        log_push("UI: POWER OFF");
        break;
    case 1:
        snprintf(status_line, sizeof(status_line), "REBOOT...");
        ui_trigger("/tmp/reboot.warm");
        log_push("UI: REBOOT");
        break;
    case 2:
        snprintf(status_line, sizeof(status_line), "FASTBOOT...");
        ui_trigger("/tmp/reboot.bootloader");
        log_push("UI: FASTBOOT");
        break;
    case 3:
        snprintf(status_line, sizeof(status_line), "RECOVERY...");
        ui_trigger("/tmp/reboot.recovery");
        log_push("UI: RECOVERY");
        break;
    case 10:
        ui_trigger("/tmp/com.on");
        snprintf(status_line, sizeof(status_line), "COM MODE");
        log_push("UI: COM MODE");
        break;
    case 11:
        vib_short();
        snprintf(status_line, sizeof(status_line), "VIB OK");
        log_push("UI: VIB TEST");
        break;
    case 20:
        ui_trigger("/tmp/radio.start");
        snprintf(status_line, sizeof(status_line), "RADIO START...");
        log_push("UI: RADIO START");
        break;
    case 21:
        ui_trigger("/tmp/radio.probe");
        metrics_update();
        snprintf(status_line, sizeof(status_line), "RADIO PROBE");
        log_push("UI: RADIO PROBE");
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
    layout_metrics();
    layout_tabs();
    touch_fd = touch_open();
    touch_ok = touch_fd >= 0;
    metrics.touch_ok = touch_ok;
    if (touch_ok) {
        touch_get_cal(touch_fd, &touch_cal);
        touch_cal.screen_w = (int)ctx->w;
        touch_cal.screen_h = (int)ctx->h;
    }
    log_push("UI: DASHBOARD READY");
    log_poll_kmsg();
    metrics_update();
    metrics_snapshot();
    last_metrics_ms = monotonic_ms();
    draw_static_scene();
    ctx->dirty = 0;
    kmsg_ui(touch_ok ? "dashboard init ok" : "dashboard no touch");
}

int ui_tick(UiDrm *ctx)
{
    (void)ctx;
    int timeout_ms = 1000;
    int moved = 0;
    unsigned long now_ms = monotonic_ms();

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

    log_poll_kmsg();

    if (now_ms - last_metrics_ms >= METRICS_MS) {
        metrics_update();
        last_metrics_ms = now_ms;
        if (metrics_changed()) {
            if (active_tab == TAB_DASH)
                draw_dash_delta();
            else if (active_tab == TAB_SYS)
                draw_sys_content();
            metrics_snapshot();
            moved = 1;
        }
    }

    if (log_dirty) {
        draw_log_delta();
        moved = 1;
    }

    if (touch_fd >= 0) {
        struct pollfd pfd = { .fd = touch_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            timeout_ms = 50;
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
        minui_fill_rect(&screen, pad_x(), 44, content_w(), 20, COL_HEAD);
        minui_text(&screen, pad_x(), 46, status_line, COL_ACCENT, 2);
        strncpy(status_prev, status_line, sizeof(status_prev) - 1);
        status_prev[sizeof(status_prev) - 1] = '\0';
        moved = 1;
    }

    frame++;
    return moved ? 200 : 2000;
}
