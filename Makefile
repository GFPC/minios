# MiniOS userspace build (cross-compile from host).
ROOT   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
OUT    := $(ROOT)../out
CC     := aarch64-linux-gnu-gcc
CFLAGS := -static -Os -I$(ROOT)include -I$(ROOT)radio -I$(ROOT)ui
LDFLAGS := -static

INIT_BIN    := $(OUT)/init
KMS_BIN     := $(OUT)/kms_paint
WLAN_BIN    := $(OUT)/wlan_scan

INIT_SRCS := \
	init/main.c init/triggers.c \
	core/log.c core/sysfs.c core/watchdog.c core/power.c \
	core/process.c core/selinux.c core/plog.c \
	hw/devnodes.c hw/led.c hw/display.c hw/boot_display.c \
	usb/state.c usb/gadget.c usb/recover.c usb/adb.c usb/com.c usb/net.c \
	radio/radio.c radio/radio_utils.c radio/modem.c radio/cnss.c \
	radio/wlan.c radio/bt.c radio/firmware.c radio/blockdev.c \
	firmware/fwload.c \
	ui/touch.c

UI_SRCS := ui/kms_paint.c ui/minui.c ui/touch.c ui/ui.c

.PHONY: all init kms_paint wlan_scan ffs_adb propstub cnss_shim drm_dump qrtr_lookup clean

all: init kms_paint

$(OUT):
	mkdir -p $(OUT)

init: $(INIT_BIN)

$(INIT_BIN): $(INIT_SRCS) | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(INIT_SRCS)

kms_paint: $(KMS_BIN)

$(KMS_BIN): $(UI_SRCS) | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(UI_SRCS)

wlan_scan: $(WLAN_BIN)

$(WLAN_BIN): radio/wlan_scan.c | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

ffs_adb: $(OUT)/ffs_adb

$(OUT)/ffs_adb: firmware/adb/ffs_adb.c | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

propstub: $(OUT)/propstub.so

$(OUT)/propstub.so: firmware/adb/propstub.c | $(OUT)
	$(CC) -shared -fPIC -Os -nostdlib -Wl,-soname,libpropstub.so \
		-o $@ $<

cnss_shim: $(OUT)/libcnss_shim.so

$(OUT)/libcnss_shim.so: firmware/adb/cnss_shim.c | $(OUT)
	$(CC) -shared -fPIC -Os -nostdlib -Wl,-soname,libcnss_shim.so \
		-o $@ $<

drm_dump: $(OUT)/drm_dump

$(OUT)/drm_dump: tools/drm_dump.c | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

qrtr_lookup: $(OUT)/qrtr_lookup

$(OUT)/qrtr_lookup: tools/qrtr_lookup.c | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(INIT_BIN) $(KMS_BIN) $(WLAN_BIN) $(OUT)/ffs_adb $(OUT)/propstub.so $(OUT)/libcnss_shim.so $(OUT)/drm_dump $(OUT)/qrtr_lookup
