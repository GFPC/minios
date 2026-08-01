#define _GNU_SOURCE
#include <unistd.h>
#include "minios/power.h"
#include "minios/radio.h"
#include "minios/triggers.h"

void triggers_poll(void)
{
    if (access("/tmp/power.off", F_OK) == 0) {
        unlink("/tmp/power.off");
        power_off();
    }
    if (access("/tmp/reboot.warm", F_OK) == 0) {
        unlink("/tmp/reboot.warm");
        reboot_mode(NULL);
    }
    if (access("/tmp/reboot.bootloader", F_OK) == 0) {
        unlink("/tmp/reboot.bootloader");
        reboot_mode("bootloader");
    }
    if (access("/tmp/reboot.recovery", F_OK) == 0) {
        unlink("/tmp/reboot.recovery");
        reboot_mode("recovery");
    }
    if (access("/tmp/radio.probe", F_OK) == 0) {
        unlink("/tmp/radio.probe");
        radio_request_wifi_async();
    }
    if (access("/tmp/radio.start", F_OK) == 0) {
        unlink("/tmp/radio.start");
        radio_request_wifi_async();
    }
    if (access("/tmp/radio.scan", F_OK) == 0) {
        unlink("/tmp/radio.scan");
        radio_scan_request_async();
    }
}
