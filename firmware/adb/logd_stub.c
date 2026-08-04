/*
 * logd_stub.c -- minimal /dev/socket/logdw listener, diagnostic only.
 *
 * Real Android liblog (staged into /lib64 by stage_cnss_libs() for every
 * vendor CNSS daemon) calls __android_log_print()/__android_log_buf_write()
 * for its logging, which connects to /dev/socket/logdw (an AF_UNIX
 * SOCK_DGRAM path) and fire-and-forgets a datagram per log line. Without a
 * real logd, and without anything else bound to that socket path, every
 * single one of those calls silently no-ops -- liblog swallows send()
 * failures. That's why pd-mapper/cnss-daemon (both link liblog.so) produced
 * zero log output for their entire runtime while qrtr-ns (links only
 * libc/libqrtr, no liblog) stayed silent for the unrelated, expected reason
 * of just not using this logging path at all. See the structure-audit
 * findings in KERNEL_CHANGES.md / MEMORY.md.
 *
 * This does not implement logd's real ring-buffer or binary wire protocol
 * (android_log_header_t + priority/tag/message). It's diagnostic-only:
 * every datagram's printable-ASCII runs get extracted and appended to a
 * file, which is enough to read the tag+message text that real logd would
 * otherwise store, without needing a byte-perfect protocol implementation.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/dev/socket/logdw"
#define LOG_PATH  "/mnt/sdcard/minios/logd_trace.log"

static void extract_text(const unsigned char *buf, int n, char *out, int outlen)
{
    int oi = 0;
    int last_was_space = 1; /* avoid leading space */

    for (int i = 0; i < n && oi < outlen - 1; i++) {
        unsigned char c = buf[i];
        if (c >= 32 && c < 127) {
            out[oi++] = (char)c;
            last_was_space = 0;
        } else if (!last_was_space) {
            out[oi++] = ' ';
            last_was_space = 1;
        }
    }
    while (oi > 0 && out[oi - 1] == ' ')
        oi--;
    out[oi] = '\0';
}

int main(void)
{
    struct sockaddr_un addr;
    int fd, lf;
    unsigned char buf[4096];
    char text[4096];
    ssize_t n;

    mkdir("/dev/socket", 0755);
    unlink(SOCK_PATH);

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return 1;
    chmod(SOCK_PATH, 0666);

    for (;;) {
        n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            continue;
        extract_text(buf, (int)n, text, sizeof(text));
        lf = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lf >= 0) {
            dprintf(lf, "[%ld bytes] %s\n", (long)n, text);
            close(lf);
        }
    }
}
