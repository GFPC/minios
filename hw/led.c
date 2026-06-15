#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "minios/led.h"
#include "minios/log.h"
#include "minios/sysfs.h"
#include "minios/watchdog.h"

char led_white_path[128];
char led_torch_path[128];

void led_trigger_none(const char *base)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/trigger", base);
    sysfs_write(p, "none");
}

int led_path_ok(const char *base)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/brightness", base);
    return access(p, W_OK) == 0;
}

void led_discover(void)
{
    DIR *d = opendir("/sys/class/leds");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[160];
        snprintf(p, sizeof(p), "/sys/class/leds/%s", e->d_name);
        if (!led_path_ok(p)) continue;
        if (!led_white_path[0] &&
            (!strcmp(e->d_name, "white") || !strcmp(e->d_name, "red")))
            snprintf(led_white_path, sizeof(led_white_path), "%s", p);
        if (!led_torch_path[0] &&
            (!strcmp(e->d_name, "led:torch_0") || !strcmp(e->d_name, "torch")))
            snprintf(led_torch_path, sizeof(led_torch_path), "%s", p);
        klogf("LED: found %s", e->d_name);
    }
    closedir(d);
}

/* wait up to 45 s for LED drivers */
void led_prepare(void)
{
    for (int i = 0; i < 450; i++) {
        led_discover();
        if (led_white_path[0] || led_torch_path[0]) break;
        if (i % 50 == 49) klog("LED: waiting for driver...");
        usleep(100000);
        wdt_pet();
    }
    if (led_white_path[0]) {
        led_trigger_none(led_white_path);
        klogf("LED: using %s", led_white_path);
    }
    if (led_torch_path[0]) {
        led_trigger_none(led_torch_path);
        klogf("LED: torch %s", led_torch_path);
    }
    if (!led_white_path[0] && !led_torch_path[0])
        klog("LED: none found");
}

void led_set_path(const char *base, int brightness)
{
    if (!base || !base[0]) return;
    char p[160], buf[16];
    snprintf(p, sizeof(p), "%s/brightness", base);
    snprintf(buf, sizeof(buf), "%d", brightness);
    sysfs_write(p, buf);
}

void led_set(int brightness)
{
    led_set_path(led_white_path, brightness);
}

void torch_set(int brightness)
{
    led_set_path(led_torch_path, brightness);
}

/* ── vibrator (works when LED drivers are still deferred) ── */
void vib_pulse(int ms)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", ms);
    sysfs_write("/sys/class/leds/vibrator/activate", "1");
    sysfs_write("/sys/class/leds/vibrator/brightness", "255");
    sysfs_write("/sys/devices/virtual/timed_output/vibrator/enable", buf);
    usleep((useconds_t)ms * 1000);
    sysfs_write("/sys/class/leds/vibrator/activate", "0");
    sysfs_write("/sys/class/leds/vibrator/brightness", "0");
    sysfs_write("/sys/devices/virtual/timed_output/vibrator/enable", "0");
}

void led_blink(int times)
{
    const char *const k_led[] = {
        "/sys/class/leds/white",
        "/sys/class/leds/red",
        "/sys/class/leds/green",
        "/sys/class/leds/blue",
        NULL
    };
    const char *const k_torch[] = {
        "/sys/class/leds/led:torch_0",
        "/sys/class/leds/led:torch_1",
        "/sys/class/leds/led:flash_0",
        NULL
    };
    for (int t = 0; t < times; t++) {
        for (int i = 0; k_led[i]; i++) {
            led_trigger_none(k_led[i]);
            led_set_path(k_led[i], 255);
        }
        for (int i = 0; k_torch[i]; i++) {
            led_trigger_none(k_torch[i]);
            led_set_path(k_torch[i], 255);
        }
        vib_pulse(200);
        usleep(500000);
        for (int i = 0; k_led[i]; i++) led_set_path(k_led[i], 0);
        for (int i = 0; k_torch[i]; i++) led_set_path(k_torch[i], 0);
        usleep(500000);
        wdt_pet();
    }
}
