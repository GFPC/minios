// SPDX-License-Identifier: GPL-2.0
/*
 * MiniOS: boot modem via subsystem_get() for WLAN wlfw QMI service.
 * Uses kallsyms_lookup_name to avoid modversions CRC mismatch with stock kernel.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

typedef void *(*subsystem_get_t)(const char *);
typedef void (*subsystem_put_t)(void *);

static subsystem_get_t pil_get;
static subsystem_put_t pil_put;
static void *modem_pil;
static struct kobject *boot_modem_kobj;
static char boot_status[64] = "idle";

static void set_boot_status(const char *msg)
{
	strlcpy(boot_status, msg, sizeof(boot_status));
	pr_info("minios_modem_boot: %s\n", msg);
}

static int resolve_pil_symbols(void)
{
	if (pil_get)
		return 0;

	pil_get = (subsystem_get_t)kallsyms_lookup_name("subsystem_get");
	pil_put = (subsystem_put_t)kallsyms_lookup_name("subsystem_put");
	if (!pil_get) {
		set_boot_status("no subsystem_get");
		return -ENOENT;
	}
	return 0;
}

static void modem_boot_work(struct work_struct *work)
{
	if (modem_pil) {
		set_boot_status("already booted");
		return;
	}

	if (resolve_pil_symbols() != 0)
		return;

	set_boot_status("subsystem_get(modem)...");
	modem_pil = pil_get("modem");
	if (IS_ERR(modem_pil)) {
		snprintf(boot_status, sizeof(boot_status),
			 "fail err=%ld", PTR_ERR(modem_pil));
		pr_err("minios_modem_boot: subsystem_get failed %ld\n",
		       PTR_ERR(modem_pil));
		modem_pil = NULL;
	} else {
		set_boot_status("ok");
	}
}

static DECLARE_WORK(modem_boot_wq, modem_boot_work);

static ssize_t boot_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) < 0 || val != 1)
		return -EINVAL;

	schedule_work(&modem_boot_wq);
	return count;
}

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", boot_status);
}

static struct kobj_attribute boot_attr = __ATTR(boot, 0220, NULL, boot_store);
static struct kobj_attribute status_attr = __ATTR(status, 0444, status_show, NULL);

static struct attribute *attrs[] = {
	&boot_attr.attr,
	&status_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int __init minios_modem_boot_init(void)
{
	if (resolve_pil_symbols() != 0)
		pr_warn("minios_modem_boot: symbols not resolved yet\n");

	boot_modem_kobj = kobject_create_and_add("boot_modem", kernel_kobj);
	if (!boot_modem_kobj)
		return -ENOMEM;

	pr_info("minios_modem_boot: registered\n");
	return sysfs_create_group(boot_modem_kobj, &attr_group);
}

static void __exit minios_modem_boot_exit(void)
{
	if (boot_modem_kobj) {
		sysfs_remove_group(boot_modem_kobj, &attr_group);
		kobject_put(boot_modem_kobj);
		boot_modem_kobj = NULL;
	}
	if (modem_pil && pil_put) {
		pil_put(modem_pil);
		modem_pil = NULL;
	}
}

module_init(minios_modem_boot_init);
module_exit(minios_modem_boot_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MiniOS modem boot via kallsyms subsystem_get");
