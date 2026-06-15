#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NLMSG_TAIL(nmsg) \
    ((struct nlattr *)(((char *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

static int nl_sock = -1;
static int nl80211_id = -1;
static int ifindex;

static int nla_put(struct nlmsghdr *n, int attrtype, int attrlen, const void *data)
{
    int len = NLA_HDRLEN + attrlen;
    struct nlattr *nla;

    if (NLMSG_ALIGN(n->nlmsg_len) + NLMSG_ALIGN(len) > 8192)
        return -1;
    nla = NLMSG_TAIL(n);
    nla->nla_type = (uint16_t)attrtype;
    nla->nla_len = (uint16_t)len;
    if (attrlen)
        memcpy((char *)nla + NLA_HDRLEN, data, (size_t)attrlen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + NLMSG_ALIGN(len);
    return 0;
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
    nla_put(&req.n, CTRL_ATTR_FAMILY_NAME, 8, "nl80211");
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
            if (tb[CTRL_ATTR_FAMILY_ID]) {
                nl80211_id = *(int16_t *)((char *)tb[CTRL_ATTR_FAMILY_ID] + NLA_HDRLEN);
                return nl80211_id >= 0 ? 0 : -1;
            }
        }
    }
}

static int trigger_scan(void)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char buf[256];
    } req;
    int seq = (int)time(NULL) + 1;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req.n.nlmsg_type = (uint16_t)nl80211_id;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP;
    req.n.nlmsg_seq = (uint32_t)seq;
    req.g.cmd = NL80211_CMD_TRIGGER_SCAN;
    nla_put(&req.n, NL80211_ATTR_IFINDEX, 4, &ifindex);
    return nl_send(&req.n);
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

static void print_scan_results(void)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char buf[64];
    } req;
    char buf[8192];
    struct scan_entry entries[64];
    int n_entries = 0;
    int seq = (int)time(NULL) + 2;

    memset(entries, 0, sizeof(entries));
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req.n.nlmsg_type = (uint16_t)nl80211_id;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.n.nlmsg_seq = (uint32_t)seq;
    req.g.cmd = NL80211_CMD_GET_SCAN;
    nla_put(&req.n, NL80211_ATTR_IFINDEX, 4, &ifindex);
    if (nl_send(&req.n) < 0) {
        puts("scan dump send fail");
        return;
    }

    for (;;) {
        ssize_t len = recv(nl_sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (len <= 0)
            break;
        struct nlmsghdr *n;
        for (n = (struct nlmsghdr *)buf; NLMSG_OK(n, (unsigned int)len);
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
            if (n_entries < 64)
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
    if (trigger_scan() < 0) {
        puts("scan trigger fail");
        return 1;
    }
    sleep(4);
    print_scan_results();
    return 0;
}
