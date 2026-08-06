// SPDX-License-Identifier: GPL-2.0
/*
 * MiniOS diagnostic: kprobe on qrtr_local_enqueue() (net/qrtr/qrtr.c), the
 * function that delivers QRTR packets between two endpoints on the SAME
 * processor (kernel<->userspace or userspace<->userspace), as opposed to
 * qcom_smd_qrtr_send()/qrtr_endpoint_post() (already hooked by qrtr_snoop.c)
 * which only carry traffic crossing to/from the modem chip.
 *
 * Why this exists: icnss's wlan_pd domain lookup (SERVREG_LOC_GET_DOMAIN_LIST,
 * sent to pd-mapper, a local AP-side userspace daemon) never crosses the
 * smd_qrtr transport at all -- confirmed live, qrtr_snoop.c's RX/TX hooks
 * never see it, on either MiniOS or a real Lineage reference capture. Only
 * pd-mapper's own text log line ("No matching domains found") is visible;
 * the actual request/response bytes have never been seen. This hook reads
 * the destination port (to->sq_port) and a hex dump of the skb payload for
 * every local delivery, via probe_kernel_read only -- mirrors the existing,
 * already-proven-safe qrtr_snoop.c RX/TX hooks exactly, never dereferencing
 * a raw pointer directly. NOTE: a prior session found that kprobes on two
 * OTHER, service-notifier-specific static functions near this exact boot
 * phase (root_service_service_ind_cb, send_notif_listener_msg_req) caused
 * two reproduced reboots (see qrtr_snoop.c's own comment) -- this hooks a
 * different, far more generic core qrtr.c function instead, but test via
 * scripts/capture-lineage-qrtr.sh's temporary `fastboot boot` path first if
 * possible, not a flashed boot partition, precisely because of that history.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>

#define QSL_MAX_DUMP 128
#define QSL_MAX_PKTS 20000

struct qsl_sockaddr_qrtr {
	unsigned short sq_family;
	__u32 sq_node;
	__u32 sq_port;
};

static atomic_t qsl_count = ATOMIC_INIT(0);

/* static int qrtr_local_enqueue(struct qrtr_node *node, struct sk_buff *skb,
 *                                int type, struct sockaddr_qrtr *from,
 *                                struct sockaddr_qrtr *to, unsigned int flags)
 * AArch64: x0=node x1=skb x2=type x3=from x4=to x5=flags
 */
static int qsl_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct sk_buff *skb = (struct sk_buff *)regs->regs[1];
	int type = (int)regs->regs[2];
	struct qsl_sockaddr_qrtr *from_ptr = (struct qsl_sockaddr_qrtr *)regs->regs[3];
	struct qsl_sockaddr_qrtr *to_ptr = (struct qsl_sockaddr_qrtr *)regs->regs[4];
	struct sk_buff sb;
	struct qsl_sockaddr_qrtr from, to;
	unsigned char buf[QSL_MAX_DUMP];
	char hex[QSL_MAX_DUMP * 3 + 1];
	size_t n, i;
	long rc;

	if (atomic_inc_return(&qsl_count) > QSL_MAX_PKTS)
		return 0;

	if (probe_kernel_read(&from, from_ptr, sizeof(from))) {
		pr_info("qrtr-snoop-local: from unreadable\n");
		return 0;
	}
	if (probe_kernel_read(&to, to_ptr, sizeof(to))) {
		pr_info("qrtr-snoop-local: to unreadable\n");
		return 0;
	}
	if (probe_kernel_read(&sb, skb, sizeof(sb))) {
		pr_info("qrtr-snoop-local: skb unreadable\n");
		return 0;
	}

	n = sb.len < QSL_MAX_DUMP ? sb.len : QSL_MAX_DUMP;
	rc = probe_kernel_read(buf, sb.data, n);
	if (rc) {
		pr_info("qrtr-snoop-local: type=%d from=%u:%u to=%u:%u len=%u (payload unreadable, rc=%ld)\n",
			type, from.sq_node, from.sq_port, to.sq_node, to.sq_port,
			sb.len, rc);
		return 0;
	}
	for (i = 0; i < n; i++)
		scnprintf(hex + i * 3, 4, "%02x ", buf[i]);
	hex[n * 3] = '\0';
	pr_info("qrtr-snoop-local: type=%d from=%u:%u to=%u:%u len=%u data=%s\n",
		type, from.sq_node, from.sq_port, to.sq_node, to.sq_port,
		sb.len, hex);
	return 0;
}

static struct kprobe kp_local = {
	.symbol_name = "qrtr_local_enqueue",
	.pre_handler = qsl_pre,
};

static int __init qrtr_snoop_local_init(void)
{
	int rc = register_kprobe(&kp_local);

	if (rc < 0)
		pr_err("qrtr-snoop-local: register kprobe failed %d\n", rc);
	else
		pr_info("qrtr-snoop-local: hook on qrtr_local_enqueue OK\n");

	pr_info("qrtr-snoop-local: loaded (cap=%d pkts)\n", QSL_MAX_PKTS);
	return 0;
}

static void __exit qrtr_snoop_local_exit(void)
{
	unregister_kprobe(&kp_local);
	pr_info("qrtr-snoop-local: unloaded, total=%d\n", atomic_read(&qsl_count));
}

module_init(qrtr_snoop_local_init);
module_exit(qrtr_snoop_local_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MiniOS local QRTR delivery snoop via kprobe on qrtr_local_enqueue");
