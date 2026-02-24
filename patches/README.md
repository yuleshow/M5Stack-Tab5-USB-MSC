# Framework Patches

Three bugs in the pioarduino ESP32-P4 Arduino framework prevent USB Mass
Storage from working on the M5Stack Tab5.  The scripts in this directory
fix all three.

## Quick start

```bash
# 1. Build once so PlatformIO downloads the framework packages:
pio run

# 2. Apply the patches:
chmod +x patches/apply_patches.sh
./patches/apply_patches.sh

# 3. Rebuild:
pio run
```

## What gets patched

### Patch 1 — `USBMSC.cpp`: Bulk endpoint MPS 512 → 64

The Arduino USB MSC descriptor uses `CFG_TUD_ENDOINT_SIZE` as the
max-packet-size for the bulk IN/OUT endpoints.  On ESP32-P4, the header
`esp32-hal-tinyusb.h` defaults this to **512** (the High-Speed maximum).

However, the Tab5's USB-C port is connected through the FSLS (Full-Speed /
Low-Speed) PHY via the DWC2 OTG11 controller, which only supports
**Full-Speed**.  The USB 2.0 specification limits bulk endpoint MPS to
**64 bytes** at Full-Speed.

When the host sees a Full-Speed device advertising 512-byte bulk endpoints
it refuses `SET_CONFIGURATION` and the device never enumerates.

The `-DCFG_TUD_ENDOINT_SIZE=64` build flag set in `platformio.ini` should
fix this, but because the framework sources are **pre-compiled** into the
PlatformIO package, the `#ifndef` guard in the header is evaluated at
*framework build time*, not at *your project's build time*.  The only fix
is to change the source directly.

### Patch 2 — `USBCDC.cpp`: Same MPS issue

The CDC-ACM descriptor has the same problem.  Even if you don't use CDC,
Arduino ESP32 registers a CDC interface by default when
`ARDUINO_USB_CDC_ON_BOOT=1`, so its descriptor must also use MPS = 64.

### Patch 3 — `libarduino_tinyusb.a`: FIFO depth 1024 → 800

The TinyUSB DWC2 driver maintains a table (`_dwc2_controller[]`) with
per-chip parameters.  The ESP32-P4 entry sets `ep_fifo_size = 1024`,
meaning 256 × 4-byte words of FIFO.

But the OTG11 controller's `GHWCFG3` register reports a FIFO depth of
only **200 words** (800 bytes).  The driver therefore tries to allocate
the non-periodic TX FIFO beyond the actual hardware FIFO boundary, and
bulk IN data never reaches the host.

Since this value lives inside a pre-compiled static library (`.a`), we
binary-patch the two occurrences of `0x00000400` (1024) to `0x00000320`
(800).

## Reverting

```bash
./patches/revert_patches.sh
```

This restores the original files from the `.bak` backups created by the
apply script.
