/*
 * qrtr_lookup — minimal standalone QRTR service-lookup client.
 *
 * Talks raw AF_QIPCRTR sockets directly (kernel wire protocol from
 * kernel/include/uapi/linux/qrtr.h) instead of dlopen()'ing the vendor
 * libqrtr.so — that library is a bionic/Android build, and this tool is
 * cross-compiled against glibc (same toolchain/flags as the rest of
 * minios's own tools, -static); mixing the two ABIs at runtime via dlopen
 * was judged too likely to silently misbehave. Socket syscalls themselves
 * don't care which libc issues them, so this sidesteps the ABI question
 * entirely.
 *
 * Sends a NEW_LOOKUP control packet for a given service id to the local
 * qrtr-ns (address {qrtr_local_nid, QRTR_PORT_CTRL} — confirmed against
 * kernel/net/qrtr/qrtr.c, which uses exactly this address for its own
 * internal hello/bye control traffic) and prints any NEW_SERVER responses.
 *
 * Usage: qrtr_lookup <service_hex> [timeout_sec]
 *   e.g. qrtr_lookup 45      (wlfw is service 0x45)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QRTR_PORT_CTRL 0xfffffffeu

struct sockaddr_qrtr_local {
    unsigned short sq_family;
    unsigned short pad0;
    uint32_t sq_node;
    uint32_t sq_port;
};

enum {
    QRTR_TYPE_DATA       = 1,
    QRTR_TYPE_HELLO      = 2,
    QRTR_TYPE_BYE        = 3,
    QRTR_TYPE_NEW_SERVER = 4,
    QRTR_TYPE_DEL_SERVER = 5,
    QRTR_TYPE_DEL_CLIENT = 6,
    QRTR_TYPE_NEW_LOOKUP = 10,
    QRTR_TYPE_DEL_LOOKUP = 11,
};

struct qrtr_ctrl_pkt {
    uint32_t cmd;
    union {
        struct { uint32_t service; uint32_t instance; uint32_t node; uint32_t port; } server;
        struct { uint32_t node; uint32_t port; } client;
        struct { uint32_t rsvd; uint32_t node; } proc;
    };
};

/* CONFIG_QRTR_NODE_ID for this build — see kernel/include/config/qrtr.h /
 * project MEMORY.md. Confirmed via kernel source: qrtr_local_nid is used
 * as the address for the kernel's own control traffic to the local ns. */
#define QRTR_LOCAL_NID 1

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: qrtr_lookup <service_hex> [timeout_sec]\n");
        return 1;
    }
    unsigned int service = (unsigned int)strtoul(argv[1], NULL, 16);
    int timeout_sec = argc > 2 ? atoi(argv[2]) : 8;

    int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("socket(AF_QIPCRTR) failed errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    struct sockaddr_qrtr_local dst;
    memset(&dst, 0, sizeof(dst));
    dst.sq_family = AF_QIPCRTR;
    dst.sq_node = QRTR_LOCAL_NID;
    dst.sq_port = QRTR_PORT_CTRL;

    struct qrtr_ctrl_pkt req;
    memset(&req, 0, sizeof(req));
    req.cmd = QRTR_TYPE_NEW_LOOKUP;
    req.server.service = service;
    req.server.instance = 0;
    req.server.node = 0;
    req.server.port = 0;

    printf("qrtr_lookup: socket fd=%d, sending NEW_LOOKUP for service=0x%x to node=%u port=CTRL\n",
           fd, service, QRTR_LOCAL_NID);

    ssize_t sr = sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst));
    if (sr < 0) {
        printf("sendto failed errno=%d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("sendto ok (%zd bytes)\n", sr);

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int found = 0;
    time_t deadline = time(NULL) + timeout_sec;
    while (time(NULL) < deadline) {
        unsigned char buf[512];
        struct sockaddr_qrtr_local from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            printf("recvfrom errno=%d (%s)\n", errno, strerror(errno));
            break;
        }
        if ((size_t)n < sizeof(uint32_t)) continue;
        struct qrtr_ctrl_pkt *pkt = (struct qrtr_ctrl_pkt *)buf;
        printf("recv %zd bytes from node=%u port=%u: cmd=%u", n, from.sq_node, from.sq_port, pkt->cmd);
        if (pkt->cmd == QRTR_TYPE_NEW_SERVER || pkt->cmd == QRTR_TYPE_DEL_SERVER) {
            printf(" service=0x%x instance=%u node=%u port=%u",
                   pkt->server.service, pkt->server.instance, pkt->server.node, pkt->server.port);
            if (pkt->cmd == QRTR_TYPE_NEW_SERVER && pkt->server.service == service) {
                printf(" *** MATCH for requested service 0x%x ***", service);
                found = 1;
            }
        }
        printf("\n");
    }

    if (found)
        printf("RESULT: service 0x%x IS registered in QRTR\n", service);
    else
        printf("RESULT: service 0x%x NOT seen within %ds (either not registered, or ns didn't reply — see notes)\n",
               service, timeout_sec);

    close(fd);
    return found ? 0 : 2;
}
