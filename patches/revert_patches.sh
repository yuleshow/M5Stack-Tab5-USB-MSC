#!/usr/bin/env bash
#
# revert_patches.sh — Restore the original (unpatched) framework files.
#
# Usage:
#   ./patches/revert_patches.sh
set -euo pipefail

FWCORE=""
FWLIBS=""

for d in "$HOME"/.platformio/packages/framework-arduinoespressif32*/; do
  [[ -d "$d/cores/esp32" ]] && FWCORE="$d"
done
for d in "$HOME"/.platformio/packages/framework-arduinoespressif32-libs*/; do
  [[ -d "$d/esp32p4/lib" ]] && FWLIBS="$d"
done

reverted=0

for f in \
  "$FWCORE/cores/esp32/USBMSC.cpp" \
  "$FWCORE/cores/esp32/USBCDC.cpp" \
  "$FWLIBS/esp32p4/lib/libarduino_tinyusb.a"
do
  if [[ -f "${f}.bak" ]]; then
    echo "Restoring: $f"
    mv "${f}.bak" "$f"
    ((reverted++))
  fi
done

if (( reverted == 0 )); then
  echo "No .bak files found — nothing to revert."
else
  echo "Reverted $reverted file(s)."
fi
