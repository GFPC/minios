#!/bin/sh
# Mainline configfs USB gadget — ACM serial (no FunctionFS / adbd).
set -e

log() { echo "setup-usb: $*" > /dev/kmsg; }

mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
if ! mount -t configfs none /config 2>/dev/null; then
    mount -t configfs configfs /sys/kernel/config 2>/dev/null || true
    G=/sys/kernel/config/usb_gadget/g0
else
    G=/config/usb_gadget/g0
fi
rm -f "$G/UDC" 2>/dev/null || true

mkdir -p "$G"
echo 0x18d1 > "$G/idVendor"
echo 0xd001 > "$G/idProduct"

mkdir -p "$G/strings/0x409"
echo "minios00"  > "$G/strings/0x409/serialnumber"
echo "MiniOS"    > "$G/strings/0x409/manufacturer"
echo "Phone ACM" > "$G/strings/0x409/product"

mkdir -p "$G/configs/c.1/strings/0x409"
echo "ACM" > "$G/configs/c.1/strings/0x409/configuration"
echo 120 > "$G/configs/c.1/MaxPower"

FN=""
for n in acm.0 acm.gs0 acm.usb0; do
    if mkdir "$G/functions/$n" 2>/dev/null; then
        FN="$n"
        break
    fi
done
if [ -z "$FN" ]; then
    log "no acm function"
    exit 1
fi

ln -sf "$G/functions/$FN" "$G/configs/c.1/acm"

i=0
UDC=""
while [ "$i" -lt 60 ]; do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
    [ -n "$UDC" ] && break
    i=$((i + 1))
    sleep 0.1
done
if [ -z "$UDC" ]; then
    log "no UDC"
    exit 1
fi

echo "$UDC" > "$G/UDC"
sleep 1
log "bound $UDC fn=$FN"
