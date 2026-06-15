/*
 * FunctionFS ADB — write descriptors, wait for ENABLE, exec adbd.
 * adbd takes over ep0/ep1/ep2 after the host enumerates the gadget.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef __NR_syslog
#define __NR_syslog 116
#endif
#define SYSLOG_ACTION_WRITE 7

#define USB_DT_INTERFACE       0x04
#define USB_DT_ENDPOINT        0x05
#define USB_CLASS_VENDOR_SPEC  0xff
#define USB_ENDPOINT_XFER_BULK 0x02
#define FUNCTIONFS_DESCRIPTORS_MAGIC_V2 3
#define FUNCTIONFS_STRINGS_MAGIC        2
#define FUNCTIONFS_HAS_FS_DESC          1
#define FUNCTIONFS_HAS_HS_DESC          2

#define FUNCTIONFS_BIND     0
#define FUNCTIONFS_UNBIND   1
#define FUNCTIONFS_ENABLE   2
#define FUNCTIONFS_DISABLE  3
#define FUNCTIONFS_SETUP    4

#define FFS_EP0 "/dev/usb-ffs/adb/ep0"

#define cpu_to_le32(x) ((uint32_t)(x))
#define cpu_to_le16(x) ((uint16_t)(x))

typedef struct {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bEndpointAddress;
	uint8_t  bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t  bInterval;
} __attribute__((packed)) usb_ep_desc;

typedef struct {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bInterfaceNumber;
	uint8_t bAlternateSetting;
	uint8_t bNumEndpoints;
	uint8_t bInterfaceClass;
	uint8_t bInterfaceSubClass;
	uint8_t bInterfaceProtocol;
	uint8_t iInterface;
} __attribute__((packed)) usb_intf_desc;

struct usb_functionfs_event {
	union {
		struct {
			uint8_t  bmRequestType;
			uint8_t  bRequest;
			uint16_t wValue;
			uint16_t wIndex;
			uint16_t wLength;
		} __attribute__((packed)) setup;
	} u;
	uint8_t type;
	uint8_t _pad[3];
} __attribute__((packed));

static void logmsg(const char *msg)
{
	char kbuf[256];
	int fd;

	snprintf(kbuf, sizeof(kbuf), "<6>ffs_adb: %s\n", msg);
	syscall(__NR_syslog, SYSLOG_ACTION_WRITE, kbuf, (int)strlen(kbuf));
	fd = open("/tmp/ffs_adb.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		write(fd, msg, strlen(msg));
		write(fd, "\n", 1);
		close(fd);
	}
}

static const struct {
	struct { uint32_t magic; uint32_t length; uint32_t flags; } header;
	uint32_t fs_count;
	uint32_t hs_count;
	struct { usb_intf_desc intf; usb_ep_desc ep_in; usb_ep_desc ep_out; } fs_descs, hs_descs;
} __attribute__((packed)) descriptors = {
	.header = {
		.magic = cpu_to_le32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
		.length = cpu_to_le32(sizeof(descriptors)),
		.flags = cpu_to_le32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
	},
	.fs_count = cpu_to_le32(3),
	.hs_count = cpu_to_le32(3),
	.fs_descs = {
		.intf = { sizeof(usb_intf_desc), USB_DT_INTERFACE, 0, 0, 2,
			USB_CLASS_VENDOR_SPEC, 0x42, 0x01, 1 },
		.ep_in = { sizeof(usb_ep_desc), USB_DT_ENDPOINT, 0x81,
			USB_ENDPOINT_XFER_BULK, cpu_to_le16(64), 0 },
		.ep_out = { sizeof(usb_ep_desc), USB_DT_ENDPOINT, 0x01,
			USB_ENDPOINT_XFER_BULK, cpu_to_le16(64), 0 },
	},
	.hs_descs = {
		.intf = { sizeof(usb_intf_desc), USB_DT_INTERFACE, 0, 0, 2,
			USB_CLASS_VENDOR_SPEC, 0x42, 0x01, 1 },
		.ep_in = { sizeof(usb_ep_desc), USB_DT_ENDPOINT, 0x81,
			USB_ENDPOINT_XFER_BULK, cpu_to_le16(512), 0 },
		.ep_out = { sizeof(usb_ep_desc), USB_DT_ENDPOINT, 0x01,
			USB_ENDPOINT_XFER_BULK, cpu_to_le16(512), 0 },
	},
};

#define STR_INTERFACE "ADB Interface"

static const struct {
	struct { uint32_t magic; uint32_t length; uint32_t str_count; uint32_t lang_count; } header;
	struct { uint16_t code; char str[sizeof(STR_INTERFACE)]; } lang0;
} __attribute__((packed)) strings = {
	.header = {
		.magic = cpu_to_le32(FUNCTIONFS_STRINGS_MAGIC),
		.length = cpu_to_le32(sizeof(strings)),
		.str_count = cpu_to_le32(1),
		.lang_count = cpu_to_le32(1),
	},
	.lang0 = { cpu_to_le16(0x0409), STR_INTERFACE },
};

static void handle_setup(int fd, const struct usb_functionfs_event *ev)
{
	char buf[256];
	uint16_t len = ev->u.setup.wLength;

	if (!len)
		return;
	if (len > sizeof(buf))
		len = sizeof(buf);
	if (ev->u.setup.bmRequestType & 0x80) {
		memset(buf, 0, len);
		write(fd, buf, len);
	} else {
		read(fd, buf, len);
	}
}

static int setup_adbd_socket(void)
{
	const char *path = "/dev/socket/adbd";
	struct sockaddr_un addr;
	int fd;

	mkdir("/dev/socket", 0777);
	unlink(path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	chmod(path, 0666);
	listen(fd, 4);
	return fd;
}

static void exec_adbd(void)
{
	int sock, lfd;

	logmsg("ENABLE -> exec adbd");
	sock = setup_adbd_socket();
	if (sock >= 0) {
		if (sock != 3) {
			dup2(sock, 3);
			close(sock);
		}
		setenv("ANDROID_SOCKET_adbd", "3", 1);
	} else {
		logmsg("WARN adbd socket");
	}

	lfd = open("/tmp/adbd.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (lfd >= 0) {
		dup2(lfd, 1);
		dup2(lfd, 2);
		close(lfd);
	}
	setenv("ANDROID_ROOT", "/system", 1);
	setenv("ANDROID_DATA", "/data", 1);
	setenv("ANDROID_RUNTIME_ROOT", "/system", 1);
	setenv("TMPDIR", "/tmp", 1);
	setenv("LD_LIBRARY_PATH", "/system/lib64:/lib64", 1);
	if (access("/lib64/propstub.so", F_OK) == 0)
		setenv("LD_PRELOAD", "/lib64/propstub.so", 1);
	execl("/system/bin/adbd", "adbd", "--root_seclabel=u:r:su:s0",
	      "--device_banner=recovery", NULL);
	execl("/sbin/adbd", "adbd", "--root_seclabel=u:r:su:s0",
	      "--device_banner=recovery", NULL);
	logmsg("FAIL exec adbd");
	_exit(127);
}

static void ep0_event_loop(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	struct usb_functionfs_event ev[4];
	static const char *names[] = {
		"BIND", "UNBIND", "ENABLE", "DISABLE", "SETUP"
	};

	logmsg("ep0 event loop");
	for (;;) {
		char msg[48];
		ssize_t n;
		int i, count;

		if (poll(&pfd, 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			logmsg("FAIL poll");
			return;
		}
		n = read(fd, ev, sizeof(ev));
		if (n <= 0)
			continue;
		count = (int)(n / (ssize_t)sizeof(ev[0]));
		for (i = 0; i < count; i++) {
			if (ev[i].type <= FUNCTIONFS_SETUP) {
				snprintf(msg, sizeof(msg), "event %s", names[ev[i].type]);
				logmsg(msg);
			}
			if (ev[i].type == FUNCTIONFS_ENABLE) {
				close(fd);
				exec_adbd();
			}
			if (ev[i].type == FUNCTIONFS_SETUP)
				handle_setup(fd, &ev[i]);
		}
	}
}

int main(void)
{
	ssize_t n;
	int fd;

	logmsg("starting");
	fd = open(FFS_EP0, O_RDWR);
	if (fd < 0) {
		logmsg("FAIL open ep0");
		return 1;
	}
	n = write(fd, &descriptors, sizeof(descriptors));
	if (n != (ssize_t)sizeof(descriptors)) {
		logmsg("FAIL write descriptors");
		return 1;
	}
	logmsg("descriptors written");
	n = write(fd, &strings, sizeof(strings));
	if (n != (ssize_t)sizeof(strings)) {
		logmsg("FAIL write strings");
		return 1;
	}
	logmsg("strings written");
	ep0_event_loop(fd);
	return 1;
}
