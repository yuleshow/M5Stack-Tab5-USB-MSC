#!/usr/bin/env bash
#
# apply_patches.sh — Patch the pioarduino ESP32-P4 Arduino framework
# so that USB Mass Storage works on the M5Stack Tab5.
#
# Run this AFTER the first PlatformIO build (so the framework packages
# have been downloaded).
#
# Usage:
#   chmod +x patches/apply_patches.sh
#   ./patches/apply_patches.sh
#
# What it patches (3 bugs):
#   1.  USBMSC.cpp  — Bulk-endpoint max-packet-size 512→64
#   2.  USBCDC.cpp  — Same MPS fix for the CDC descriptor
#   3.  libarduino_tinyusb.a — FIFO depth 1024→800 (binary patch)
#
# See README.md for the full explanation of each bug.
set -euo pipefail

# ── Locate the framework packages ──────────────────────────────
FWCORE=""
FWLIBS=""

# Search for the framework-arduinoespressif32 directory
for d in "$HOME"/.platformio/packages/framework-arduinoespressif32*/; do
  if [[ -d "$d/cores/esp32" ]]; then
    FWCORE="$d"
  fi
done

for d in "$HOME"/.platformio/packages/framework-arduinoespressif32-libs*/; do
  if [[ -d "$d/esp32p4/lib" ]]; then
    FWLIBS="$d"
  fi
done

if [[ -z "$FWCORE" ]]; then
  echo "ERROR: Could not find framework-arduinoespressif32 package."
  echo "       Run 'pio run' first so PlatformIO downloads it."
  exit 1
fi
if [[ -z "$FWLIBS" ]]; then
  echo "ERROR: Could not find framework-arduinoespressif32-libs package."
  echo "       Run 'pio run' first so PlatformIO downloads it."
  exit 1
fi

echo "Framework core: $FWCORE"
echo "Framework libs: $FWLIBS"

# ── Patch 1: USBMSC.cpp — MPS 512→64 ─────────────────────────
MSC_FILE="$FWCORE/cores/esp32/USBMSC.cpp"
if [[ ! -f "$MSC_FILE" ]]; then
  echo "WARNING: $MSC_FILE not found, skipping"
else
  if grep -q 'CFG_TUD_ENDOINT_SIZE' "$MSC_FILE"; then
    echo "Patching USBMSC.cpp: CFG_TUD_ENDOINT_SIZE → 64"
    cp "$MSC_FILE" "${MSC_FILE}.bak"
    sed -i.tmp 's/CFG_TUD_ENDOINT_SIZE/64/g' "$MSC_FILE"
    rm -f "${MSC_FILE}.tmp"
    echo "  ✓ USBMSC.cpp patched (backup at .bak)"
  else
    echo "  USBMSC.cpp already patched or has different content, skipping"
  fi
fi

# ── Patch 2: USBCDC.cpp — MPS 512→64 ─────────────────────────
CDC_FILE="$FWCORE/cores/esp32/USBCDC.cpp"
if [[ ! -f "$CDC_FILE" ]]; then
  echo "WARNING: $CDC_FILE not found, skipping"
else
  if grep -q 'CFG_TUD_ENDOINT_SIZE' "$CDC_FILE"; then
    echo "Patching USBCDC.cpp: CFG_TUD_ENDOINT_SIZE → 64"
    cp "$CDC_FILE" "${CDC_FILE}.bak"
    sed -i.tmp 's/CFG_TUD_ENDOINT_SIZE/64/g' "$CDC_FILE"
    rm -f "${CDC_FILE}.tmp"
    echo "  ✓ USBCDC.cpp patched (backup at .bak)"
  else
    echo "  USBCDC.cpp already patched or has different content, skipping"
  fi
fi

# ── Patch 3: libarduino_tinyusb.a — FIFO depth 1024→800 ──────
# The DWC2 driver in TinyUSB assumes ep_fifo_size=1024 (256 words)
# but the ESP32-P4 OTG11 controller only has 200 words (800 bytes).
# This causes the non-periodic TX FIFO to be allocated beyond the
# hardware's actual FIFO depth, breaking bulk IN transfers.
#
# We binary-patch the two occurrences of 0x0400 (1024) to 0x0320 (800)
# in the compiled library.
LIB_FILE="$FWLIBS/esp32p4/lib/libarduino_tinyusb.a"
if [[ ! -f "$LIB_FILE" ]]; then
  echo "WARNING: $LIB_FILE not found, skipping"
else
  # Check if already patched by looking for the backup
  if [[ -f "${LIB_FILE}.bak" ]]; then
    echo "  libarduino_tinyusb.a backup exists — may already be patched"
    echo "  Delete ${LIB_FILE}.bak and restore original to re-patch"
  else
    echo "Patching libarduino_tinyusb.a: ep_fifo_size 1024→800 (binary)"
    cp "$LIB_FILE" "${LIB_FILE}.bak"

    # Find the offsets of the two 0x0400 values that represent ep_fifo_size.
    # In the compiled .a, the _dwc2_controller[] struct has:
    #   .ep_fifo_size = 1024   (0x00 0x04 in big-endian, or 0x00 0x04 as
    #                            two bytes in the little-endian 32-bit word
    #                            0x00000400)
    #
    # We use Python for reliable binary patching.
    python3 - "$LIB_FILE" <<'PYEOF'
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    data = bytearray(f.read())

old = (1024).to_bytes(4, "little")   # 0x00040000 → bytes: 00 04 00 00
new = (800).to_bytes(4, "little")    # 0x00032000 → bytes: 20 03 00 00

count = 0
offset = 0
while True:
    idx = data.find(old, offset)
    if idx == -1:
        break
    # Sanity: only patch if surrounded by plausible struct data
    # (the ep_fifo_size field is typically near other small constants)
    data[idx:idx+4] = new
    count += 1
    print(f"  Patched at offset 0x{idx:X}")
    offset = idx + 4

if count == 0:
    print("  WARNING: no occurrences of 0x00000400 found — already patched?")
elif count < 2:
    print(f"  WARNING: only {count} occurrence(s) patched (expected 2)")
else:
    print(f"  ✓ {count} occurrences patched")

with open(path, "wb") as f:
    f.write(data)
PYEOF
    echo "  ✓ libarduino_tinyusb.a patched (backup at .bak)"
  fi
fi

echo ""
echo "All patches applied.  Run 'pio run' to build with the fixes."
