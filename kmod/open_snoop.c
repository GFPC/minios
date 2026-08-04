// SPDX-License-Identifier: GPL-2.0
/*
 * MiniOS diagnostic: kprobes on do_filp_open + sys_mkdirat + sys_lchown,
 * filtered to a short list of path substrings (NV/EFS/persist/modemst-
 * related) — printk's the full path whenever a match is seen.
 *
 * See MEMORY.md §4.5ay/az/b0: the open-only version showed real Android's
 * tftp_server opening /mnt/vendor/persist/rfs/mdm/* right before WLAN FW
 * ready, but zero equivalent activity from MiniOS's own tftp_server —
 * BUT real disassembly (Ghidra) of tftp_server showed its actual RFS work
 * is done via lchown()/mkdir(), NOT open() — do_filp_open's kprobe was
 * structurally blind to it the whole time. Added mkdirat/lchown hooks to
 * settle whether MiniOS's tftp_server is silently succeeding (invisible to
 * the open-only version) or genuinely never reaching this code at all.
 * Filtering in-kernel (not dumping every call) keeps this cheap enough to
 * run through a real boot.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/sched.h>

#define OS_MAX_HITS 4000

static atomic_t os_count = ATOMIC_INIT(0);

static const char *needles[] = {
	"persist", "/efs/", "nv_", "modemst", "nvitem", "/nv/",
	"wlan_config", "wlan_offload", "wlan_pd", "qcril", "radio_config",
	NULL
};

static void report_if_match(const char *tag, const char *buf)
{
	int i;

	for (i = 0; needles[i]; i++) {
		if (strnstr(buf, needles[i], 192)) {
			atomic_inc(&os_count);
			pr_info("open-snoop: %s comm=%s pid=%d path=%s\n",
				tag, current->comm, current->pid, buf);
			break;
		}
	}
}

/* int do_filp_open(int dfd, struct filename *pathname, const struct open_flags *op)
 * pathname is an already-kernel-resolved struct filename*, not a raw user
 * pointer -- probe_kernel_read (not probe_user_read) is correct here. */
static int open_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct filename *name;
	const char *path;
	char buf[192];
	long rc;

	if (atomic_read(&os_count) > OS_MAX_HITS)
		return 0;

	name = (struct filename *)regs->regs[1];
	rc = probe_kernel_read(&path, &name->name, sizeof(path));
	if (rc || !path)
		return 0;

	rc = probe_kernel_read(buf, path, sizeof(buf) - 1);
	if (rc)
		return 0;
	buf[sizeof(buf) - 1] = '\0';
	report_if_match("open", buf);
	return 0;
}

/* long sys_mkdirat(int dfd, const char __user *pathname, umode_t mode)
 * long sys_lchown(const char __user *filename, uid_t user, gid_t group)
 * Both take the path as a RAW USERSPACE pointer (pre-getname()) -- must
 * use probe_user_read, not probe_kernel_read, or this reads garbage /
 * faults. */
static int mkdirat_pre(struct kprobe *p, struct pt_regs *regs)
{
	char buf[192];
	long rc;

	if (atomic_read(&os_count) > OS_MAX_HITS)
		return 0;

	rc = probe_user_read(buf, (const void __user *)regs->regs[1], sizeof(buf) - 1);
	if (rc)
		return 0;
	buf[sizeof(buf) - 1] = '\0';
	report_if_match("mkdirat", buf);
	return 0;
}

static int lchown_pre(struct kprobe *p, struct pt_regs *regs)
{
	char buf[192];
	long rc;

	if (atomic_read(&os_count) > OS_MAX_HITS)
		return 0;

	rc = probe_user_read(buf, (const void __user *)regs->regs[0], sizeof(buf) - 1);
	if (rc)
		return 0;
	buf[sizeof(buf) - 1] = '\0';
	report_if_match("lchown", buf);
	return 0;
}

static struct kprobe kp_open = {
	.symbol_name = "do_filp_open",
	.pre_handler = open_pre,
};

static struct kprobe kp_mkdirat = {
	.symbol_name = "sys_mkdirat",
	.pre_handler = mkdirat_pre,
};

static struct kprobe kp_lchown = {
	.symbol_name = "sys_lchown",
	.pre_handler = lchown_pre,
};

static int __init open_snoop_init(void)
{
	int rc;

	rc = register_kprobe(&kp_open);
	if (rc < 0)
		pr_err("open-snoop: register do_filp_open kprobe failed %d\n", rc);
	else
		pr_info("open-snoop: hook on do_filp_open OK (cap=%d hits)\n", OS_MAX_HITS);

	rc = register_kprobe(&kp_mkdirat);
	if (rc < 0)
		pr_err("open-snoop: register sys_mkdirat kprobe failed %d\n", rc);
	else
		pr_info("open-snoop: hook on sys_mkdirat OK\n");

	rc = register_kprobe(&kp_lchown);
	if (rc < 0)
		pr_err("open-snoop: register sys_lchown kprobe failed %d\n", rc);
	else
		pr_info("open-snoop: hook on sys_lchown OK\n");

	return 0;
}

static void __exit open_snoop_exit(void)
{
	unregister_kprobe(&kp_open);
	unregister_kprobe(&kp_mkdirat);
	unregister_kprobe(&kp_lchown);
	pr_info("open-snoop: unloaded, total=%d\n", atomic_read(&os_count));
}

module_init(open_snoop_init);
module_exit(open_snoop_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MiniOS filtered vfs-open/mkdir/lchown snoop via kprobe");
