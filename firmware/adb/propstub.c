/* Property stubs for adbd — freestanding, links against device bionic libc. */
typedef unsigned int size_t;
typedef struct prop_info prop_info;

#if defined(__aarch64__)
#define PS_NR_nanosleep 101
static void ps_sleep_ms(unsigned ms)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts;
	long rem[2];

	ts.tv_sec = (long)(ms / 1000U);
	ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
	rem[0] = rem[1] = 0;
	__asm__ volatile("mov x8, %2\n\tmov x0, %0\n\tmov x1, %1\n\tsvc #0"
			 : "+r"(ts), "+r"(rem)
			 : "i"(PS_NR_nanosleep)
			 : "x0", "x1", "x8", "memory");
}
#else
static void ps_sleep_ms(unsigned ms)
{
	volatile unsigned long n = (unsigned long)ms * 50000UL;
	while (n--)
		;
}
#endif

#define PROP_MAX 48
#define PROP_KEY_LEN 64
#define PROP_VAL_LEN 96

static int ps_strlen(const char *s)
{
	int n = 0;
	while (s && s[n])
		n++;
	return n;
}

static int ps_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static void ps_strcpy(char *d, const char *s)
{
	while ((*d++ = *s++))
		;
}

static void ps_strncpy(char *d, const char *s, size_t n)
{
	size_t i;
	for (i = 0; i + 1 < n && s[i]; i++)
		d[i] = s[i];
	if (n)
		d[i] = '\0';
}

static struct {
	char key[PROP_KEY_LEN];
	char val[PROP_VAL_LEN];
} prop_live[PROP_MAX];
static int prop_live_n;

static const struct {
	const char *key;
	const char *val;
} prop_defaults[] = {
	{ "ro.bootmode", "recovery" },
	{ "ro.boot.selinux", "permissive" },
	{ "ro.adb.secure", "0" },
	{ "ro.secure", "0" },
	{ "ro.debuggable", "1" },
	{ "ro.arch", "arm64" },
	{ "ro.crypto.state", "encrypted" },
	{ "ro.boot.verifiedbootstate", "orange" },
	{ "ro.build.version.release", "14" },
	{ "ro.build.version.codename", "REL" },
	{ "ro.build.version.sdk", "34" },
	{ "ro.build.version.sdk_full", "34" },
	{ "ro.product.device", "willow" },
	{ "ro.product.model", "Redmi Note 8T" },
	{ "ro.product.name", "willow" },
	{ "ro.serialno", "142a2efd" },
	{ "service.adb.root", "1" },
	{ "service.adb.tcp.port", "5555" },
	{ "persist.adb.tcp.port", "5555" },
	{ "service.adb.listen_addrs", "tcp:5555" },
	{ "init.svc.mdnsd", "running" },
	{ "persist.adb.wifi.guid", "adb-minios-willow" },
	{ "service.adb.tls.port", "0" },
	{ "sys.usb.config", "adb" },
	{ "sys.usb.state", "adb" },
	{ "sys.usb.configfs", "1" },
	{ "sys.usb.ffs.ready", "1" },
	{ "sys.usb.adb.disabled", "0" },
	{ "persist.sys.usb.config", "adb" },
	{ "persist.adb.watchdog.disable", "1" },
	{ "persist.adb.watchdog", "0" },
	{ "persist.adb.tls_server.enable", "0" },
	{ "service.adb.tls.port", "0" },
	{ "ro.build.tags", "test-keys" },
	{ "ro.build.fingerprint", "Xiaomi/willow/willow:14/UP1A.231005.007/V12.5.3.0.RCWMIXM:user/release-keys" },
	{ "init.svc.adbd", "running" },
	{ "ro.build.type", "userdebug" },
	{ "sys.usb.controller", "a600000.dwc3" },
	{ "ro.boot.usbcontroller", "a600000.dwc3" },
	{ "vendor.usb.controller", "a600000.dwc3" },
	{ "wifi.interface", "wlan0" },
	{ "ro.hardware", "qcom" },
	{ "ro.board.platform", "sm6125" },
	{ "ro.vendor.product.device", "willow" },
	{ "ro.boot.hardware", "qcom" },
	{ "ro.vendor.wlan.chip", "qca6174" },
	/* Corrected to the real captured values (MEMORY.md §7.4, live getprop on
	 * the working ROM) — the old "2"/"vendor.peripheral.modem.ready=1" pair
	 * was invented/mismatched: the real property is named
	 * vendor.peripheral.modem.state (not .ready) with value ONLINE, and
	 * vendor.wlan.driver.version is a real version string, not "2". Also
	 * added the previously entirely-missing vendor.wlan.firmware.version.
	 * Whether anything in cnss-daemon's dependency chain actually reads
	 * these was unverified when found (§7.4) and still is — cheap,
	 * correctness-improving regardless of whether it changes behavior. */
	{ "vendor.wlan.driver.version", "5.2.022.12B" },
	{ "vendor.wlan.firmware.version", "FW:3.0.2.0.656.1 HW:HW_VERSION=40670000." },
	{ "vendor.peripheral.modem.state", "ONLINE" },
	{ "ro.boot.wificountrycode", "00" },
	{ "persist.vendor.wlan.enable", "1" },
	{ "init.svc.cnss-daemon", "running" },
	{ "init.svc.hwservicemanager", "running" },
	{ "init.svc.vendor.qrtr-ns", "running" },
	{ "init.svc.vendor.pd_mapper", "running" },
	{ "vendor.wlan.driver.status", "ok" },
	{ "vendor.qrtr.enable", "1" },
	{ "ro.vendor.qti.soc_model", "SM6125" },
	{ "ro.vendor.qti.soc_id", "400" },
	{ "persist.vendor.radio.atfwd.start", "true" },
	/* cnss-daemon's wsvc_printf() has TWO independent gates, not one:
	 * priority > wsvc_debug_level -> skipped entirely (controlled by the
	 * -d/-dd argv flags, already raised in radio/cnss.c's argv), AND
	 * separately priority > wsvc_kmsg_logging -> not written to kmsg even
	 * if it passed the first gate. wsvc_kmsg_logging is ONLY settable via
	 * this property (property_get_int32("persist.vendor.cnss-daemon.
	 * kmsg_logging", wsvc_kmsg_logging) — no command-line flag for it at
	 * all) — confirmed via decompile this session: -dd alone was not
	 * enough to see priority-3/4 messages (WLFW service connected, FW
	 * status: 0x%lx, FW memory is ready, Wait for FW memory ready
	 * indication, Received FW memory ready indication) even though -dd
	 * definitely raised wsvc_debug_level (confirmed via the real argv in
	 * cnss.exec.log). Set to 4 (max) so every priority level reaches
	 * kmsg. */
	{ "persist.vendor.cnss-daemon.kmsg_logging", "4" },
	{ 0, 0 }
};

static const char *prop_lookup(const char *name)
{
	for (int i = 0; i < prop_live_n; i++) {
		if (!ps_strcmp(prop_live[i].key, name))
			return prop_live[i].val;
	}
	for (int i = 0; prop_defaults[i].key; i++) {
		if (ps_strcmp(prop_defaults[i].key, name) == 0)
			return prop_defaults[i].val;
	}
		return 0;
}

int __system_property_set(const char *name, const char *value)
{
	if (!name || !value)
		return -1;
	for (int i = 0; i < prop_live_n; i++) {
		if (!ps_strcmp(prop_live[i].key, name)) {
			ps_strncpy(prop_live[i].val, value, PROP_VAL_LEN);
			return 0;
		}
	}
	if (prop_live_n >= PROP_MAX)
		return -1;
	ps_strncpy(prop_live[prop_live_n].key, name, PROP_KEY_LEN);
	ps_strncpy(prop_live[prop_live_n].val, value, PROP_VAL_LEN);
	prop_live_n++;
	return 0;
}

int __system_property_get(const char *name, char *value)
{
	const char *v = prop_lookup(name);

	if (!v) {
		value[0] = '\0';
		return 0;
	}
	ps_strcpy(value, v);
	return ps_strlen(value);
}

const prop_info *__system_property_find(const char *name)
{
	if (prop_lookup(name))
		return (const prop_info *)name;
		return 0;
}

void __system_property_read_callback(const prop_info *pi,
				     void (*callback)(void *cookie,
						      const char *name,
						      const char *value,
						      unsigned serial),
				     void *cookie)
{
	const char *name = (const char *)pi;
	const char *val;

	if (!pi || !callback)
		return;
	val = prop_lookup(name);
	if (val)
		callback(cookie, name, val, 1);
}

int __system_property_wait(const prop_info *pi, unsigned int old_serial,
			   unsigned int *new_serial, const void *rel_time)
{
	(void)rel_time;
	(void)old_serial;

	if (!pi)
		return -1;
	if (new_serial)
		*new_serial = old_serial + 1;
	return 1;
}

/* Stubs so adbd/libselinux does not abort without policy. */
int is_selinux_enabled(void)
{
	return 0;
}

int selinux_getenforce(void)
{
	return 0;
}

typedef unsigned int uid_t;

int selinux_android_setcontext(uid_t uid, int is_system_app,
				 const char *seinfo, const char *pkgname)
{
	(void)uid;
	(void)is_system_app;
	(void)seinfo;
	(void)pkgname;
	return 0;
}

int selinux_android_setcon(const char *con)
{
	(void)con;
	return 0;
}

int selinux_android_restorecon(const char *path, unsigned int flags)
{
	(void)path;
	(void)flags;
	return 0;
}
