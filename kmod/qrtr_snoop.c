// SPDX-License-Identifier: GPL-2.0
/*
 * MiniOS diagnostic: kprobe-based QRTR packet snoop.
 * Hooks the real SMD QRTR transport (net/qrtr/smd.c) RX/TX entry points and
 * printk's a hex dump of every packet crossing the AP<->modem QRTR fabric,
 * so it flows into the existing dmesg/boot.log/pstore capture pipeline with
 * zero new infra. See MEMORY.md for why: nobody has ever looked at the raw
 * QRTR packet bytes before, only QMI-application-layer strace (proven
 * irrelevant, §4.5b) or discrete kernel events (regulator/PIL, no payload).
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>

#define QS_MAX_DUMP 64
/* Was 2000 — real-ROM captures showed that's exhausted by ~7s of chatty
 * QRTR traffic (t=8.5s module load to t=15.9s cap, MEMORY.md §4.5av),
 * long before MiniOS's own modem-reset (~t=78s) or panic (~t=118s) points
 * it actually needs to span. Raised 10x: at the same ~270 pkt/s burst rate
 * each hexdump line is up to ~250 bytes, so 20000 lines is at most ~5MB —
 * leaves headroom in the 8MB log_buf_len ring for everything else MiniOS
 * already logs during a ~2 minute boot. */
#define QS_MAX_PKTS 20000

static atomic_t qs_count = ATOMIC_INIT(0);

static void qs_hexdump(const char *tag, const void *ptr, size_t len)
{
	unsigned char buf[QS_MAX_DUMP];
	char hex[QS_MAX_DUMP * 3 + 1];
	size_t n, i;
	long rc;

	if (atomic_inc_return(&qs_count) > QS_MAX_PKTS) {
		return;
	}

	n = len < QS_MAX_DUMP ? len : QS_MAX_DUMP;
	rc = probe_kernel_read(buf, ptr, n);
	if (rc) {
		pr_info("qrtr-snoop: %s len=%zu (unreadable, rc=%ld)\n", tag, len, rc);
		return;
	}
	for (i = 0; i < n; i++)
		scnprintf(hex + i * 3, 4, "%02x ", buf[i]);
	hex[n * 3] = '\0';
	pr_info("qrtr-snoop: %s len=%zu data=%s\n", tag, len, hex);
}

/* RX: int qrtr_endpoint_post(struct qrtr_endpoint *ep, const void *data, size_t len) */
static int rx_pre(struct kprobe *p, struct pt_regs *regs)
{
	const void *data = (const void *)regs->regs[1];
	size_t len = (size_t)regs->regs[2];

	qs_hexdump("RX", data, len);
	return 0;
}

static struct kprobe kp_rx = {
	.symbol_name = "qrtr_endpoint_post",
	.pre_handler = rx_pre,
};

/* TX: int qcom_smd_qrtr_send(struct qrtr_endpoint *ep, struct sk_buff *skb) */
static int tx_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct sk_buff *skb = (struct sk_buff *)regs->regs[1];
	struct sk_buff sb;
	long rc;

	rc = probe_kernel_read(&sb, skb, sizeof(sb));
	if (rc) {
		pr_info("qrtr-snoop: TX skb unreadable rc=%ld\n", rc);
		return 0;
	}
	qs_hexdump("TX", sb.data, sb.len);
	return 0;
}

static struct kprobe kp_tx = {
	.symbol_name = "qcom_smd_qrtr_send",
	.pre_handler = tx_pre,
};

/* REVERTED (same night): kprobes on root_service_service_ind_cb() and
 * send_notif_listener_msg_req() were added to observe the §4.5ao open
 * question directly, but the very next two live tests each rebooted the
 * phone at almost exactly the point in boot where these two hooks would
 * fire (right after icnss-state reached qrtr=1/daemon=1, i.e. right around
 * wlan_pd registration) — a real, reproduced-twice correlation, confirmed
 * NOT a captured kernel panic (pstore was empty both times, ruling out a
 * normal oops/panic() path) — consistent with either a bad kprobe insertion
 * point (register_kprobe() can reject/misbehave on certain instructions,
 * §4.5w's own risk-assessment note) or a genuinely bad struct/offset
 * assumption corrupting something reachable from that code path. Removed
 * both rather than risk a third reboot chasing it further tonight — the
 * proven-safe RX/TX hooks below are unaffected and already answer the
 * wlan_pd question via raw hex (§4.5al/ao). If this is revisited, treat
 * these two symbols as needing kretprobes or a from-source kernel debug
 * pass instead of blind pre-handler kprobes on unfamiliar static
 * functions. */

static int __init qrtr_snoop_init(void)
{
	int rc;

	rc = register_kprobe(&kp_rx);
	if (rc < 0)
		pr_err("qrtr-snoop: register RX kprobe failed %d\n", rc);
	else
		pr_info("qrtr-snoop: RX hook on qrtr_endpoint_post OK\n");

	rc = register_kprobe(&kp_tx);
	if (rc < 0)
		pr_err("qrtr-snoop: register TX kprobe failed %d\n", rc);
	else
		pr_info("qrtr-snoop: TX hook on qcom_smd_qrtr_send OK\n");

	pr_info("qrtr-snoop: loaded (cap=%d pkts)\n", QS_MAX_PKTS);
	return 0;
}

static void __exit qrtr_snoop_exit(void)
{
	unregister_kprobe(&kp_rx);
	unregister_kprobe(&kp_tx);
	pr_info("qrtr-snoop: unloaded, total=%d\n", atomic_read(&qs_count));
}

module_init(qrtr_snoop_init);
module_exit(qrtr_snoop_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MiniOS QRTR packet snoop via kprobes");
