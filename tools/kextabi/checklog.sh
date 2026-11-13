#!/bin/sh
# Grab the newest OpenCore log from the EFI USB and report on AirportQCA.
#
# Usage: tools/kextabi/checklog.sh [path-to-EFI-volume]
#
# The EFI partition of the boot USB is mounted at /Volumes/USB and the logs
# are written to its root as opencore-<timestamp>.txt.

set -e

VOL="${1:-/Volumes/USB}"

if [ ! -d "$VOL" ]; then
    echo "EFI volume $VOL not found - is the USB mounted?" >&2
    exit 1
fi

LOG=$(ls -t "$VOL"/opencore-*.txt 2>/dev/null | head -1)
if [ -z "$LOG" ]; then
    echo "No opencore-*.txt found in $VOL" >&2
    exit 1
fi

echo "### log: $LOG"
echo

echo "### AirportQCA injection"
grep -n "AirportQCA" "$LOG" || echo "  (no AirportQCA lines)"
echo

echo "### errors"
grep -n -E "Vtable patching failed|Failed to patch symbol|Invalid Parameter|OCAK:.*[Ee]rror|panic|Panic" "$LOG" \
    || echo "  (none)"
echo

echo "### all prelinked injections"
grep -n "Prelinked injection" "$LOG" || echo "  (none)"
