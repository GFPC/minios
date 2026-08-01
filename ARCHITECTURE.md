# MiniOS architecture

MiniOS is a static, single-binary `init` that boots directly from the
initramfs on a Redmi Note 8T (Qualcomm SM6125 / `trinket` / `ginkgo`) —
no Android framework, no Zygote/ART/SystemServer. It runs on top of the
LineageOS `lineage-23.2` kernel for this device (see `../kernel/`,
patched — patches documented in `../KERNEL_CHANGES.md`), which is built
and packaged separately (see `../scripts/build-minios.sh` and
`../scripts/build-minios-repack.sh`).

This file describes what each directory is responsible for. For *why*
specific decisions were made (protocol quirks, timing constants, crash
root-causes), see `../MEMORY.md` — this file explains structure, MEMORY.md
explains history.

## Build

`Makefile` cross-compiles everything here with `aarch64-linux-gnu-gcc
-static`, no libc headers assumed beyond what the bundled toolchain
provides. Output binaries land in `../out/`, then get packed into the
initramfs by `../scripts/build-initramfs.sh`.

Two binaries are `-nostdlib` shared objects loaded via `LD_PRELOAD` into
*vendor* daemons (not part of `init` itself) — see `firmware/adb/`.

## Directory map

| Path | Responsibility |
|---|---|
| `init/` | Entry point (`main.c`) and boot-sequence triggers (`triggers.c`). Owns the top-level boot state machine: mount, USB gadget bring-up, display splash, then hands off to `radio/` and `ui/`. |
| `core/` | Cross-cutting OS-level primitives with no hardware-specific knowledge: logging (`log.c`, `plog.c` — the SD-card-persistent logger read throughout this project's debugging), sysfs helpers (`sysfs.c`), process spawn/reap helpers (`process.c`), power (`power.c`), the MSM hardware watchdog pet loop (`watchdog.c`), and a minimal SELinux stub (`selinux.c`). |
| `hw/` | Direct hardware/device-node ownership: `devnodes.c` (mknod/udev-equivalent), `display.c`/`boot_display.c` (DRM/KMS framebuffer splash), `led.c`. |
| `usb/` | The ConfigFS USB gadget stack: `gadget.c` (UDC bind/unbind, mode switching), `state.c`, `adb.c` (adbd FunctionFS wiring), `com.c` (the custom COM debug-console protocol used for live device control throughout development — see `../scripts/com_tools.py`/`com-cli.py`), `net.c`, `recover.c` (self-healing UDC-drop watchdog, `usb_com_maintain()`). |
| `radio/` | The modem/WLAN bring-up pipeline — the largest and most complex subsystem: `modem.c` (PIL modem boot, RMTFS), `cnss.c` (QMI service daemons: qrtr-ns, pd-mapper, cnss-daemon), `wlan.c`/`wlan_scan.c` (WCN3990 WiFi bring-up and scanning), `bt.c` (Bluetooth), `firmware.c`/`blockdev.c` (firmware file staging), `radio.c`/`radio_utils.c` (shared helpers: `vendor_bin()`, `stage_vendor_bin()`, `proc_running()`, daemon spawn/log-redirect). This is the subsystem still being actively debugged (see MEMORY.md for the WiFi bring-up saga). |
| `firmware/` | `fwload.c` — generic firmware-file loading. `firmware/adb/` holds the three pieces that make *unmodified vendor Android binaries* runnable under MiniOS's non-Android userspace: `propstub.c` (`__system_property_*`/libselinux stubs, `-nostdlib`, `LD_PRELOAD`ed), `cnss_shim.c` (bionic-symbol shims + the raw-syscall QMI wire-trace logger written to `/mnt/sdcard/minios/qmi_trace.log`, `-nostdlib`, `LD_PRELOAD`ed into every vendor CNSS daemon), `ffs_adb.c` (standalone adbd FunctionFS helper binary). |
| `ui/` | Minimal DRM/KMS-based UI: `minui.c`/`ui.c` (drawing primitives), `kms_paint.c` (standalone splash-paint binary), `touch.c`/`touchmon` (touchscreen input). |
| `tools/` | Small standalone diagnostic binaries, not linked into `init`: `drm_dump.c`, `qrtr_lookup.c`. |
| `kmod/` | Out-of-tree kernel modules built against `../kernel/` (`make -C ../kernel M=$(pwd)/kmod`), *not* this directory's own Makefile: `qrtr_snoop.c` (kprobe-based QRTR/QMI packet sniffer — see MEMORY.md for the servreg_notif/PD_MON findings this captured), `minios_modem_boot.c` (modem PIL boot helper). |
| `assets/` | Static payloads bundled into the initramfs verbatim: `vendor_radio/` (vendor QMI/CNSS binaries + libs pulled from a working ROM — see `../archive/vendor-willow-oss/` and `../stock_miui/` for the sources these were extracted from), `firmware/` (touch controller firmware blob). `setup-usb.sh` is dead relative to the active build: it's a shell-script ConfigFS setup used only by the archived mainline-kernel path (`../scripts/legacy/`, paired with `legacy/init-mainline.c`) — the active path does gadget setup in C, `usb/gadget.c`. |
| `include/minios/` | Public headers for every subsystem above, one per major `.c` file/group. |
| `legacy/` | Superseded code kept for reference only, not built: pre-refactor monolithic `init.c.bak`, the mainline-kernel-only `init-acm.c`/`init-mainline.c` variants (mainline-kernel build path itself archived under `../scripts/legacy/`, mid-June, see `../scripts/legacy/` for why), and `split-init.py` (the one-off script that performed the `init.c` → modular split, per the single `git log` entry "Refactor MiniOS into modular initramfs userspace"). |

## Version control

`minios/` is its own git repository (independent of the top-level
`/home/greg/phone` repo, which was only added later — see
`../.gitignore`, which deliberately excludes `minios/` from the parent
repo to avoid nested-repo/submodule confusion). Commit here when changing
anything under this directory.
