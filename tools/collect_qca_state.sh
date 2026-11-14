#!/bin/sh
#
# Collect the AirportQCA bring-up state from a fresh boot.
#
# Usage:  sudo tools/collect_qca_state.sh [output-file]
#
# The driver's IOLog output never reaches the unified log, so the kernel
# ring (dmesg) has to be read for the driver's own messages. The ioreg
# properties carry the register snapshots and breadcrumbs.

OUT="${1:-/tmp/qca-state.txt}"

{
    echo "===== uptime ====="
    uptime

    echo
    echo "===== ioreg QCA properties ====="
    ioreg -l | grep -o '"QCA[A-Za-z0-9]*" = [^;]*' | sort

    echo
    echo "===== ioreg raw QCA block ====="
    ioreg -l | grep -E '"QCA[A-Za-z0-9]*"' | sed 's/^ *//'

    echo
    echo "===== dmesg: AirportQCA ====="
    dmesg 2>/dev/null | grep -i -E "AirportQCA|qca|ath10k|bmi|copy engine|ce[0-7]" | tail -120

    echo
    echo "===== unified log: WCL / IO80211 ====="
    /usr/bin/log show --last 15m --style syslog --info --debug 2>/dev/null \
        | grep -iE "AirportQCA|IO80211Family|corecapture|WiFi|wlan" | tail -80

    echo
    echo "===== network interfaces ====="
    ifconfig -a 2>/dev/null | grep -E "^(en|wl|utun|bridge)[0-9a-z]*:" 
    networksetup -listallhardwareports 2>/dev/null

    echo
    echo "===== loaded kext ====="
    kmutil showloaded 2>/dev/null | grep -i airport
} > "$OUT" 2>&1

echo "written to $OUT"
