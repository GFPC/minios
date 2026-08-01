#!/bin/sh
# Ultimate MiniOS USB setup (ADB Composite + FunctionFS)
set -e

log() { echo "setup-usb: $*" > /dev/kmsg; }

mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true

if ! mount -t configfs none /config 2>/dev/null; then
    mount -t configfs configfs /sys/kernel/config 2>/dev/null || true
    G=/sys/kernel/config/usb_gadget/g0
else
    G=/config/usb_gadget/g0
fi

# Reset existing UDC binding
if [ -f "$G/UDC" ]; then
    echo "" > "$G/UDC" 2>/dev/null || true
fi

mkdir -p "$G"
echo 0x18d1 > "$G/idVendor"
echo 0x4ee2 > "$G/idProduct"

mkdir -p "$G/strings/0x409"
echo "minios00" > "$G/strings/0x409/serialnumber"
echo "MiniOS" > "$G/strings/0x409/manufacturer"
echo "Redmi Note 8T" > "$G/strings/0x409/product"

mkdir -p "$G/configs/c.1/strings/0x409"
echo "ADB" > "$G/configs/c.1/strings/0x409/configuration"
echo 500 > "$G/configs/c.1/MaxPower"

# Configure FunctionFS ADB
mkdir -p "$G/functions/ffs.adb"
ln -sf "$G/functions/ffs.adb" "$G/configs/c.1/ffs.adb" 2>/dev/null || true

mkdir -p /dev/usb-ffs/adb
mount -t functionfs adb /dev/usb-ffs/adb 2>/dev/null || true

# Start ADB daemon
if [ -x /sbin/adbd ] || [ -x /bin/adbd ]; then
    adbd &
    log "adbd daemon started"
fi

i=0
UDC=""
while [ "$i" -lt 50 ]; do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
    [ -n "$UDC" ] && break
    i=$((i + 1))
    sleep 0.1
done

if [ -n "$UDC" ]; then
    echo "$UDC" > "$G/UDC"
    log "bound UDC=$UDC to g0 ADB"
else
    log "no UDC found"
fi
