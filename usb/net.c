#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "minios/adb.h"
#include "minios/log.h"
#include "minios/usb.h"
#include "minios/watchdog.h"

int if_set_up4(const char *ifname, const char *addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    if (fd < 0)
        return -1;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_flags = IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) {
        close(fd);
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ((struct sockaddr_in *)&ifr.ifr_addr)->sin_family = AF_INET;
    inet_pton(AF_INET, addr, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        close(fd);
        return -1;
    }
    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0",
              &((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr);
    ioctl(fd, SIOCSIFNETMASK, &ifr);
    close(fd);
    return 0;
}

int find_net_if(char *out, size_t outsz)
{
    const char *guess[] = { "usb0", "ncm0", "rndis0", "eth0", NULL };
    DIR *d;

    for (int i = 0; guess[i]; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/net/%s", guess[i]);
        if (access(path, F_OK) == 0) {
            snprintf(out, outsz, "%s", guess[i]);
            return 0;
        }
    }
    d = opendir("/sys/class/net");
    if (!d)
        return -1;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo"))
            continue;
        snprintf(out, outsz, "%s", e->d_name);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}
void usb_net_setup(void)
{
    char ifn[32];

    if (!usb_ncm_active)
        return;
    for (int i = 0; i < 60; i++) {
        wdt_pet();
        if (find_net_if(ifn, sizeof(ifn)) == 0)
            break;
        usleep(200000);
    }
    if (find_net_if(ifn, sizeof(ifn)) != 0) {
        klog("net: iface missing");
        return;
    }
    snprintf(usb_net_if, sizeof(usb_net_if), "%s", ifn);
    if (if_set_up4(ifn, "192.168.42.129") == 0)
        klogf("net: %s up 192.168.42.129", ifn);
    else
        klogf("net: %s ifconfig failed", ifn);
}

void usb_tcp_adb_async(void)
{
    pid_t p = fork();
    if (p != 0)
        return;
    usb_net_setup();
    usleep(500000);
    adb_start_tcp();
    _exit(0);
}
