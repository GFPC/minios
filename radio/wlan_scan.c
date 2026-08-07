#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Rewritten version. The original blindly sent TRIGGER_SCAN (as a passive
 * scan, no SSID list, never checking the ACK/error) then slept a fixed 4s
 * and dumped whatever GET_SCAN had at that point -- any rejected trigger
 * (EBUSY, interface not fully up yet, etc.) or a scan that simply took
 * longer than 4s (very common on first bring-up) silently looked like
 * "no networks found" with zero indication of what actually happened.
 * This version: sends a wildcard-SSID *active* scan (matches what every
 * real supplicant does -- passive-only misses/delays APs that don't
 * beacon quickly), reads the real ACK/error for the trigger itself, joins
 * nl80211's "scan" multicast group and waits for the real
 * NL80211_CMD_NEW_SCAN_RESULTS/SCAN_ABORTED notification instead of a
 * fixed sleep, and drains the results dump properly across however many
 * netlink datagrams the kernel splits it into instead of a single
 * best-effort MSG_DONTWAIT burst. */

#define RECV_BUF_SIZE   16384
#define SCAN_WAIT_MS    25000
#define MAX_ENTRIES     128

#define NLMSG_TAIL(nmsg) \
    ((struct nlattr *)(((char *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

static int nl_sock = -1;
static int nl80211_id = -1;
static int scan_mcast_id = -1;
static int ifindex;

static int nla_put(struct nlmsghdr *n, size_t max, int attrtype, int attrlen, const void *data)
{
    int len = NLA_HDRLEN + attrlen;
    struct nlattr *nla;

    if (NLMSG_ALIGN(n->nlmsg_len) + NLMSG_ALIGN((unsigned)len) > max)
        return -1;
    nla = NLMSG_TAIL(n);
    nla->nla_type = (uint16_t)attrtype;
    nla->nla_len = (uint16_t)len;
    if (attrlen)
        memcpy((char *)nla + NLA_HDRLEN, data, (size_t)attrlen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + NLMSG_ALIGN((unsigned)len);
    return 0;
}

static struct nlattr *nla_nest_start(struct nlmsghdr *n, size_t max, int attrtype)
{
    struct nlattr *start = NLMSG_TAIL(n);
    if (nla_put(n, max, attrtype, 0, NULL) < 0)
        return NULL;
    return start;
}

static void nla_nest_end(struct nlmsghdr *n, struct nlattr *start)
{
    start->nla_len = (uint16_t)((char *)NLMSG_TAIL(n) - (char *)start);
}

static int nl_send(struct nlmsghdr *n)
{
    struct sockaddr_nl nladdr = { .nl_family = AF_NETLINK };
    struct iovec iov = { n, n->nlmsg_len };
    struct msghdr msg = {
        .msg_name = &nladdr,
        .msg_namelen = sizeof(nladdr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    return sendmsg(nl_sock, &msg, 0) >= 0 ? 0 : -1;
}

/* Blocks up to timeout_ms waiting for readable data, then does one
 * MSG_DONTWAIT recv(). Returns the recv() length, 0 on timeout, -1 on
 * error. */
static ssize_t nl_recv_wait(char *buf, size_t buflen, int timeout_ms)
{
    struct pollfd pfd = { .fd = nl_sock, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);

    if (pr <= 0)
        return 0;
    return recv(nl_sock, buf, buflen, MSG_DONTWAIT);
}

static int nl80211_init(void)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char buf[256];
    } req;
    char buf[8192];
    int seq = (int)time(NULL);

    nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (nl_sock < 0)
        return -1;

    struct sockaddr_nl local = { .nl_family = AF_NETLINK };
    if (bind(nl_sock, (struct sockaddr *)&local, sizeof(local)) < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req.n.nlmsg_type = GENL_ID_CTRL;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.n.nlmsg_seq = (uint32_t)seq;
    req.n.nlmsg_pid = 0;
    req.g.cmd = CTRL_CMD_GETFAMILY;
    nla_put(&req.n, sizeof(req), CTRL_ATTR_FAMILY_NAME, 8, "nl80211");
    if (nl_send(&req.n) < 0)
        return -1;

    while (1) {
        ssize_t len = recv(nl_sock, buf, sizeof(buf), 0);
        struct nlmsghdr *n;
        if (len <= 0)
            return -1;
        for (n = (struct nlmsghdr *)buf; NLMSG_OK(n, (unsigned int)len);
             n = NLMSG_NEXT(n, len)) {
            struct genlmsghdr *g = NLMSG_DATA(n);
            struct nlattr *tb[CTRL_ATTR_MAX + 1];
            if (n->nlmsg_type == NLMSG_ERROR)
                return -1;
            if (n->nlmsg_type != GENL_ID_CTRL || g->cmd != CTRL_CMD_NEWFAMILY)
                continue;
            memset(tb, 0, sizeof(tb));
            struct nlattr *attrs = (struct nlattr *)((char *)g + GENL_HDRLEN);
            int alen = (int)n->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
            while (alen > 0) {
                struct nlattr *a = attrs;
                if (a->nla_len < NLA_HDRLEN)
                    break;
                if (a->nla_type <= CTRL_ATTR_MAX)
                    tb[a->nla_type] = a;
                int step = NLA_ALIGN(a->nla_len);
                attrs = (struct nlattr *)((char *)attrs + step);
                alen -= step;
            }
            if (tb[CTRL_ATTR_FAMILY_ID])
                nl80211_id = *(int16_t *)((char *)tb[CTRL_ATTR_FAMILY_ID] + NLA_HDRLEN);

            /* Walk CTRL_ATTR_MCAST_GROUPS (nested list of nested {NAME,ID}
             * pairs) looking for the "scan" group, so we can subscribe and
             * wait for the real scan-done notification instead of guessing
             * with a fixed sleep. */
            if (tb[CTRL_ATTR_MCAST_GROUPS]) {
                struct nlattr *grp;
                int glen = tb[CTRL_ATTR_MCAST_GROUPS]->nla_len - NLA_HDRLEN;
                grp = (struct nlattr *)((char *)tb[CTRL_ATTR_MCAST_GROUPS] + NLA_HDRLEN);
                while (glen > 0) {
                    struct nlattr *gb[CTRL_ATTR_MCAST_GRP_ID + 1];
                    struct nlattr *ga;
                    int alen2;

                    if (grp->nla_len < NLA_HDRLEN)
                        break;
                    memset(gb, 0, sizeof(gb));
                    ga = (struct nlattr *)((char *)grp + NLA_HDRLEN);
                    alen2 = grp->nla_len - NLA_HDRLEN;
                    while (alen2 > 0) {
                        if (ga->nla_len < NLA_HDRLEN)
                            break;
                        if (ga->nla_type <= CTRL_ATTR_MCAST_GRP_ID)
                            gb[ga->nla_type] = ga;
                        int step2 = NLA_ALIGN(ga->nla_len);
                        ga = (struct nlattr *)((char *)ga + step2);
                        alen2 -= step2;
                    }
                    if (gb[CTRL_ATTR_MCAST_GRP_NAME] && gb[CTRL_ATTR_MCAST_GRP_ID]) {
                        const char *name = (char *)gb[CTRL_ATTR_MCAST_GRP_NAME] + NLA_HDRLEN;
                        if (!strcmp(name, "scan"))
                            scan_mcast_id = (int)*(uint32_t *)((char *)gb[CTRL_ATTR_MCAST_GRP_ID] + NLA_HDRLEN);
                    }
                    int step = NLA_ALIGN(grp->nla_len);
                    grp = (struct nlattr *)((char *)grp + step);
                    glen -= step;
                }
            }

            if (nl80211_id >= 0)
                return 0;
        }
    }
}

static int join_scan_mcast(void)
{
    if (scan_mcast_id < 0)
        return -1;
    return setsockopt(nl_sock, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                       &scan_mcast_id, sizeof(scan_mcast_id));
}

/* Sends TRIGGER_SCAN with a single wildcard (zero-length) SSID -- an
 * active probe-request scan, same as any real supplicant does, instead of
 * the passive-only scan the old code silently did by omitting
 * NL80211_ATTR_SCAN_SSIDS entirely. Reads the real ACK/error afterward
 * instead of assuming success. Returns 0 on success, negative errno (or
 * -1) on failure. */
static int trigger_scan(void)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char buf[256];
    } req;
    struct nlattr *ssids;
    char rbuf[RECV_BUF_SIZE];
    int seq = (int)time(NULL) + 1;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req.n.nlmsg_type = (uint16_t)nl80211_id;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.n.nlmsg_seq = (uint32_t)seq;
    req.g.cmd = NL80211_CMD_TRIGGER_SCAN;
    nla_put(&req.n, sizeof(req), NL80211_ATTR_IFINDEX, 4, &ifindex);

    ssids = nla_nest_start(&req.n, sizeof(req), NL80211_ATTR_SCAN_SSIDS);
    nla_put(&req.n, sizeof(req), 0, 0, NULL); /* one empty == wildcard SSID */
    if (ssids)
        nla_nest_end(&req.n, ssids);

    if (nl_send(&req.n) < 0)
        return -1;

    for (;;) {
        ssize_t len = nl_recv_wait(rbuf, sizeof(rbuf), 5000);
        struct nlmsghdr *n;
        if (len <= 0)
            return -1;
        for (n = (struct nlmsghdr *)rbuf; NLMSG_OK(n, (unsigned int)len);
             n = NLMSG_NEXT(n, len)) {
            if (n->nlmsg_seq != (uint32_t)seq)
                continue;
            if (n->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(n);
                return e->error;
            }
        }
    }
}

/* Waits for the real scan-completion notification on the "scan" multicast
 * group. Returns 1 on NEW_SCAN_RESULTS, 0 on SCAN_ABORTED, -1 on timeout
 * (caller should still attempt a best-effort dump afterward -- partial
 * results are better than none). */
static int wait_scan_done(void)
{
    char rbuf[RECV_BUF_SIZE];
    struct timespec start, now;
    long elapsed_ms;

    if (scan_mcast_id < 0) {
        /* No multicast group found (unexpected on a real nl80211 driver,
         * but fall back to the old fixed-delay behavior rather than
         * failing outright). */
        sleep(4);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                     (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= SCAN_WAIT_MS)
            return -1;

        ssize_t len = nl_recv_wait(rbuf, sizeof(rbuf),
                                    (int)(SCAN_WAIT_MS - elapsed_ms));
        if (len <= 0)
            continue;

        struct nlmsghdr *n;
        for (n = (struct nlmsghdr *)rbuf; NLMSG_OK(n, (unsigned int)len);
             n = NLMSG_NEXT(n, len)) {
            struct genlmsghdr *g;
            if (n->nlmsg_type != (unsigned)nl80211_id)
                continue;
            g = NLMSG_DATA(n);
            if (g->cmd == NL80211_CMD_NEW_SCAN_RESULTS)
                return 1;
            if (g->cmd == NL80211_CMD_SCAN_ABORTED)
                return 0;
        }
    }
}

struct scan_entry {
    char ssid[34];
    int signal;
    int seen;
};

static void parse_scan_attrs(struct nlattr **bss, struct scan_entry *e)
{
    if (bss[NL80211_BSS_CAPABILITY]) {
        uint16_t caps = *(uint16_t *)((char *)bss[NL80211_BSS_CAPABILITY] + NLA_HDRLEN);
        if (!(caps & 0x1))
            return;
    }
    if (bss[NL80211_BSS_SIGNAL_MBM]) {
        int s = *(int32_t *)((char *)bss[NL80211_BSS_SIGNAL_MBM] + NLA_HDRLEN);
        e->signal = s / 100;
    }
    if (bss[NL80211_BSS_INFORMATION_ELEMENTS]) {
        const uint8_t *ie = (const uint8_t *)((char *)bss[NL80211_BSS_INFORMATION_ELEMENTS] + NLA_HDRLEN);
        int ielen = (int)bss[NL80211_BSS_INFORMATION_ELEMENTS]->nla_len - NLA_HDRLEN;
        int off = 0;
        while (off + 2 <= ielen) {
            uint8_t id = ie[off];
            uint8_t elen = ie[off + 1];
            if (off + 2 + elen > ielen)
                break;
            if (id == 0 && elen > 0 && elen < (int)sizeof(e->ssid)) {
                memcpy(e->ssid, ie + off + 2, elen);
                e->ssid[elen] = '\0';
                e->seen = 1;
            }
            off += 2 + elen;
        }
    }
}

static void print_scan_results(int wait_result)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char buf[64];
    } req;
    char rbuf[RECV_BUF_SIZE];
    static struct scan_entry entries[MAX_ENTRIES];
    int n_entries = 0;
    int seq = (int)time(NULL) + 2;

    memset(entries, 0, sizeof(entries));
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req.n.nlmsg_type = (uint16_t)nl80211_id;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.n.nlmsg_seq = (uint32_t)seq;
    req.g.cmd = NL80211_CMD_GET_SCAN;
    nla_put(&req.n, sizeof(req), NL80211_ATTR_IFINDEX, 4, &ifindex);
    if (nl_send(&req.n) < 0) {
        puts("scan dump send fail");
        return;
    }

    if (wait_result < 0)
        puts("(scan-done notification timed out -- showing best-effort results)");
    else if (wait_result == 0)
        puts("(scan was aborted by the driver -- showing whatever results exist)");

    for (;;) {
        /* Real dumps for a busy area can span many datagrams; give each
         * fragment a bounded wait instead of a single MSG_DONTWAIT burst,
         * which could truncate results still in flight. */
        ssize_t len = nl_recv_wait(rbuf, sizeof(rbuf), 3000);
        if (len <= 0)
            break;
        struct nlmsghdr *n;
        for (n = (struct nlmsghdr *)rbuf; NLMSG_OK(n, (unsigned int)len);
             n = NLMSG_NEXT(n, len)) {
            struct genlmsghdr *g;
            struct nlattr *tb[NL80211_ATTR_MAX + 1];
            struct nlattr *bss[NL80211_BSS_MAX + 1];
            struct scan_entry e;

            if (n->nlmsg_type == NLMSG_DONE)
                goto done;
            if (n->nlmsg_type == NLMSG_ERROR)
                goto done;
            if (n->nlmsg_type != (unsigned)nl80211_id)
                continue;
            g = NLMSG_DATA(n);
            if (g->cmd != NL80211_CMD_NEW_SCAN_RESULTS &&
                g->cmd != NL80211_CMD_GET_SCAN)
                continue;

            memset(tb, 0, sizeof(tb));
            memset(bss, 0, sizeof(bss));
            memset(&e, 0, sizeof(e));
            e.signal = -100;

            struct nlattr *attrs = (struct nlattr *)((char *)g + GENL_HDRLEN);
            int alen = (int)n->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
            while (alen > 0) {
                struct nlattr *a = attrs;
                if (a->nla_len < NLA_HDRLEN)
                    break;
                if (a->nla_type <= NL80211_ATTR_MAX)
                    tb[a->nla_type] = a;
                int step = NLA_ALIGN(a->nla_len);
                attrs = (struct nlattr *)((char *)attrs + step);
                alen -= step;
            }
            if (!tb[NL80211_ATTR_BSS])
                continue;
            attrs = (struct nlattr *)((char *)tb[NL80211_ATTR_BSS] + NLA_HDRLEN);
            alen = tb[NL80211_ATTR_BSS]->nla_len - NLA_HDRLEN;
            while (alen > 0) {
                struct nlattr *a = attrs;
                if (a->nla_len < NLA_HDRLEN)
                    break;
                if (a->nla_type <= NL80211_BSS_MAX)
                    bss[a->nla_type] = a;
                int step = NLA_ALIGN(a->nla_len);
                attrs = (struct nlattr *)((char *)attrs + step);
                alen -= step;
            }
            parse_scan_attrs(bss, &e);
            if (!e.seen || !e.ssid[0])
                continue;
            for (int i = 0; i < n_entries; i++) {
                if (!strcmp(entries[i].ssid, e.ssid)) {
                    if (e.signal > entries[i].signal)
                        entries[i].signal = e.signal;
                    goto next_bss;
                }
            }
            if (n_entries < MAX_ENTRIES)
                entries[n_entries++] = e;
        next_bss:;
        }
    }
done:
    if (!n_entries) {
        puts("no networks found");
        return;
    }
    printf("found %d network(s):\r\n", n_entries);
    for (int i = 0; i < n_entries; i++)
        printf("  %s  %d dBm\r\n", entries[i].ssid, entries[i].signal);
}

int main(void)
{
    ifindex = if_nametoindex("wlan0");
    if (!ifindex) {
        puts("wlan0 missing");
        return 1;
    }
    if (nl80211_init() < 0) {
        puts("nl80211 init fail");
        return 1;
    }
    join_scan_mcast();

    int rc = trigger_scan();
    if (rc < 0) {
        printf("scan trigger fail (errno %d: %s)\r\n", -rc, strerror(-rc));
        return 1;
    }

    int wr = wait_scan_done();
    print_scan_results(wr);
    return 0;
}
