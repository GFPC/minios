# MiniOS

Modular initramfs userspace for Redmi Note 8T (willow) demo.

## Layout

```
minios/
  include/minios/   Public headers (log, usb, radio, ui, …)
  init/             Entry point + UI trigger polling
  core/             Logging, sysfs helpers, watchdog, power, selinux
  hw/               Device nodes, LED, display boot splash
  usb/              Gadget, COM shell, ADB, NCM networking
  radio/            WiFi/BT bring-up, block partitions, wlan_scan
  ui/               KMS paint, minui dashboard, touch input
  firmware/         Early firmware loader + adb helpers
  tools/            Standalone debug binaries (drm_dump)
  legacy/           init-mainline.c, init-acm.c, init.c backup
  assets/           Host-side previews, USB setup scripts, TS firmware
  Makefile          Cross-build all binaries → ../out/
```

## Build

```bash
cd minios && make          # → ../out/init, kms_paint, wlan_scan, …
./scripts/build-initramfs.sh
```

## Modules

| Module | Role |
|--------|------|
| `init/main.c` | Mounts, USB-first boot, main watchdog loop |
| `usb/com.c` | COM CLI (`ping`, `radio`, `wifi-scan`, …) |
| `usb/gadget.c` | ConfigFS gadget: ADB-only / COM+NCM |
| `radio/radio.c` | Async WiFi/BT bring-up from vendor partitions |
| `ui/kms_paint.c` | DRM dashboard (separate `/sbin/kms_paint` process) |

The monolithic `init.c` at repo root is deprecated; sources live under subdirectories.
