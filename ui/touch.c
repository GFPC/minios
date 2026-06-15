#define _GNU_SOURCE
#include "touch.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef test_bit
#define test_bit(bit, array) ((((const char *)(array))[(bit) / 8] >> ((bit) % 8)) & 1)
#endif

static char touch_dev_name[64];

static void kmsg(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[128];
        int n = snprintf(b, sizeof(b), "<6>touch: %s\n", s);
        if (n > 0)
            (void)write(fd, b, n);
        close(fd);
    }
}

static void mknod_input(const char *sys_dev, const char *dev_path)
{
    char buf[32];
    if (access(dev_path, F_OK) == 0)
        return;
    int fd = open(sys_dev, O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    unsigned maj = 0, min = 0;
    if (sscanf(buf, "%u:%u", &maj, &min) != 2)
        return;
    mknod(dev_path, S_IFCHR | 0666, makedev(maj, min));
}

void touch_ensure_nodes(void)
{
    mkdir("/dev/input", 0755);
    DIR *d = opendir("/sys/class/input");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char sp[128], dp[128];
        snprintf(sp, sizeof(sp), "/sys/class/input/%s/dev", e->d_name);
        snprintf(dp, sizeof(dp), "/dev/input/%s", e->d_name);
        mknod_input(sp, dp);
    }
    closedir(d);
}

static int name_is_touch(const char *name)
{
    if (!name || !name[0])
        return 0;
    if (strstr(name, "NVTCapacitive") || strstr(name, "NVT-ts") ||
        strstr(name, "touch") || strstr(name, "Touch") ||
        strstr(name, "touchscreen") || strstr(name, "fts"))
        return 1;
    return 0;
}

static int try_open_event(const char *ev, const char *name_path)
{
    char name[256];
    int fd = open(name_path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, name, sizeof(name) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    name[n] = '\0';
    for (char *p = name; *p; p++)
        if (*p == '\n') *p = '\0';
    if (!name_is_touch(name))
        return -1;
    snprintf(touch_dev_name, sizeof(touch_dev_name), "%s", name);
    return open(ev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

int touch_open(void)
{
    touch_ensure_nodes();
    touch_dev_name[0] = '\0';

    for (int i = 0; i < 16; i++) {
        char ev[64], np[128];
        snprintf(ev, sizeof(ev), "/dev/input/event%d", i);
        snprintf(np, sizeof(np), "/sys/class/input/event%d/device/name", i);
        if (access(ev, F_OK) != 0)
            continue;
        int fd = try_open_event(ev, np);
        if (fd >= 0) {
            kmsg(touch_dev_name);
            return fd;
        }
    }

    for (int i = 0; i < 16; i++) {
        char ev[64];
        unsigned long absbit[(ABS_MAX + 1) / (8 * sizeof(long)) + 1] = {0};
        snprintf(ev, sizeof(ev), "/dev/input/event%d", i);
        int fd = open(ev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) == 0 &&
            (test_bit(ABS_MT_POSITION_X, absbit) ||
             test_bit(ABS_X, absbit))) {
            snprintf(touch_dev_name, sizeof(touch_dev_name), "event%d", i);
            kmsg("fallback abs");
            return fd;
        }
        close(fd);
    }
    kmsg("not found");
    return -1;
}

void touch_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

const char *touch_name(void)
{
    return touch_dev_name;
}

static int abs_range(int fd, int code, int *minv, int *maxv)
{
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(code), &ai) < 0)
        return -1;
    *minv = ai.minimum;
    *maxv = ai.maximum;
    return 0;
}

void touch_get_cal(int fd, TouchCal *cal)
{
    memset(cal, 0, sizeof(*cal));
    if (fd < 0)
        return;
    if (abs_range(fd, ABS_MT_POSITION_X, &cal->min_x, &cal->max_x) < 0)
        abs_range(fd, ABS_X, &cal->min_x, &cal->max_x);
    if (abs_range(fd, ABS_MT_POSITION_Y, &cal->min_y, &cal->max_y) < 0)
        abs_range(fd, ABS_Y, &cal->min_y, &cal->max_y);
}

void touch_map(const TouchCal *cal, int raw_x, int raw_y, int *out_x, int *out_y)
{
    int x = raw_x, y = raw_y;
    int minx = cal->min_x, maxx = cal->max_x;
    int miny = cal->min_y, maxy = cal->max_y;

    if (cal->swap_xy) {
        int t = x;
        x = y;
        y = t;
        minx = cal->min_y;
        maxx = cal->max_y;
        miny = cal->min_x;
        maxy = cal->max_x;
    }
    if (cal->inv_x)
        x = minx + maxx - x;
    if (cal->inv_y)
        y = miny + maxy - y;

    if (cal->screen_w > 0 && maxx > minx)
        x = (x - minx) * (cal->screen_w - 1) / (maxx - minx);
    if (cal->screen_h > 0 && maxy > miny)
        y = (y - miny) * (cal->screen_h - 1) / (maxy - miny);

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (cal->screen_w > 0 && x >= cal->screen_w)
        x = cal->screen_w - 1;
    if (cal->screen_h > 0 && y >= cal->screen_h)
        y = cal->screen_h - 1;
    *out_x = x;
    *out_y = y;
}

static int parse_event(struct input_event *ev, int *lx, int *ly, int *ld, int *got)
{
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_MT_POSITION_X || ev->code == ABS_X)
            *lx = ev->value;
        else if (ev->code == ABS_MT_POSITION_Y || ev->code == ABS_Y)
            *ly = ev->value;
        else if (ev->code == ABS_MT_TRACKING_ID)
            *ld = (ev->value >= 0);
    } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        *ld = ev->value;
        *got = 1;
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        *got = 1;
    }
    return 0;
}

int touch_read(int fd, int *x, int *y, int *down)
{
    struct input_event ev;
    static int lx, ly, ld;
    int got = 0;

    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
        parse_event(&ev, &lx, &ly, &ld, &got);

    if (!got)
        return 0;
    *x = lx;
    *y = ly;
    *down = ld;
    return 1;
}

int touch_read_timeout(int fd, TouchCal *cal, int *x, int *y, int *down, int ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct input_event ev;
    static int lx, ly, ld;
    int got = 0;
    int deadline = ms;
    int raw_x = 0, raw_y = 0;

    for (;;) {
        int pr = poll(&pfd, 1, deadline > 0 ? deadline : 0);
        if (pr <= 0)
            break;
        while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
            parse_event(&ev, &lx, &ly, &ld, &got);
        if (got)
            break;
        deadline = 50;
    }
    if (!got)
        return 0;
    raw_x = lx;
    raw_y = ly;
    if (cal && cal->screen_w > 0 && cal->screen_h > 0)
        touch_map(cal, raw_x, raw_y, x, y);
    else {
        *x = raw_x;
        *y = raw_y;
    }
    *down = ld;
    return 1;
}
