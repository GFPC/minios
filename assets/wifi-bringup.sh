#!/system/bin/sh
# Interactive Wi-Fi & Radio Bringup Test Script for MiniOS/Android
# Run as root: su -c /tmp/wifi-bringup.sh

export LD_LIBRARY_PATH="/tmp/vendor_radio/lib64:/vendor/lib64:/system/lib64:$LD_LIBRARY_PATH"
export PATH="/tmp/vendor_radio/bin:$PATH"

echo "=========================================="
echo "   Qualcomm Wi-Fi & QMI Bringup Diagnostic"
echo "=========================================="

echo "[1/6] Checking Subsystems (msm_subsys)..."
for subsys in /sys/bus/msm_subsys/devices/*; do
    if [ -d "$subsys" ]; then
        sname=$(basename $(readlink $subsys 2>/dev/null || echo $subsys))
        state=$(cat $subsys/state 2>/dev/null || echo "UNKNOWN")
        echo "  - $sname: $state"
    fi
done

echo "[2/6] Starting Storage & RFS Daemons (rmt_storage, tftp_server)..."
if ! pgrep rmt_storage >/dev/null; then
    rmt_storage &
    echo "  -> rmt_storage started (PID $!)"
else
    echo "  -> rmt_storage already running"
fi

if ! pgrep tftp_server >/dev/null; then
    tftp_server &
    echo "  -> tftp_server started (PID $!)"
else
    echo "  -> tftp_server already running"
fi

sleep 1

echo "[3/6] Starting QMI Router & PD Mapper (qrtr-ns, pd-mapper)..."
if ! pgrep qrtr-ns >/dev/null; then
    qrtr-ns -f &
    echo "  -> qrtr-ns started (PID $!)"
else
    echo "  -> qrtr-ns already running"
fi

sleep 1

if ! pgrep pd-mapper >/dev/null; then
    pd-mapper &
    echo "  -> pd-mapper started (PID $!)"
else
    echo "  -> pd-mapper already running"
fi

sleep 1

echo "[4/6] Starting CNSS Network Daemon (cnss-daemon)..."
if ! pgrep cnss-daemon >/dev/null; then
    cnss-daemon -n -l &
    echo "  -> cnss-daemon started (PID $!)"
else
    echo "  -> cnss-daemon already running"
fi

sleep 2

echo "[5/6] Checking ICNSS & WLAN Platform Driver Status..."
if [ -d "/sys/bus/platform/drivers/icnss/c800000.qcom,icnss" ]; then
    echo "  🟢 icnss driver is BOUND to c800000.qcom,icnss"
else
    echo "  🔴 icnss driver is NOT bound! Attempting manual bind..."
    echo "c800000.qcom,icnss" > /sys/bus/platform/drivers/icnss/bind 2>/dev/null || true
fi

echo "[6/6] Checking Network Interface wlan0..."
if ip link show wlan0 >/dev/null 2>&1; then
    echo "  🟢 wlan0 INTERFACE IS UP AND READY!"
    ip addr show wlan0
else
    echo "  🔴 wlan0 interface not present yet."
    echo "  Checking rfkill..."
    rfkill list 2>/dev/null || true
fi

echo "=========================================="
echo "   Bringup diagnostic complete."
echo "=========================================="
