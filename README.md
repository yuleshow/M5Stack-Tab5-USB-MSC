# M5Stack Tab5 — USB Mass Storage over USB-C

> **Turn your M5Stack Tab5's SD card into a USB thumb drive.**

A minimal, self-contained PlatformIO project that exposes the SD card on
the [M5Stack Tab5](https://docs.m5stack.com/en/core/M5Tab5) (ESP32-P4) as
a USB Mass Storage device via its USB-C port.  Plug it into a Mac, PC, or
Linux machine and the SD card appears as a removable drive — no card
reader needed.

![Platform](https://img.shields.io/badge/platform-ESP32--P4-blue)
![Framework](https://img.shields.io/badge/framework-Arduino-green)
![License](https://img.shields.io/badge/license-MIT-brightgreen)

---

## Why is this needed?

The ESP32-P4's USB hardware is more complex than earlier ESP32 variants.
Getting USB MSC to work on the Tab5 requires working around **three
framework bugs** and performing a **runtime PHY swap**.  None of this is
documented by Espressif or M5Stack (as of June 2025).

This project packages everything into a drop-in solution.

---

## Hardware Background

The ESP32-P4 has **two** DWC2 USB controller instances:

| Controller | Base Address   | Speed        | PHY default      |
|------------|---------------|--------------|------------------|
| OTG11      | `0x50040000`  | Full-Speed   | PHY 1 (internal) |
| OTG20      | `0x50000000`  | High-Speed   | —                |

And **two** physical USB PHYs:

| PHY   | Connection    | Default owner          |
|-------|--------------|------------------------|
| PHY 0 | USB-C port   | USB Serial/JTAG (USJ)  |
| PHY 1 | Internal     | DWC2 OTG11             |

By default, the DWC2 controller (which TinyUSB drives) is connected to
PHY 1 (internal, no external connector), while the USB-C port is owned by
the USB Serial/JTAG controller used for flashing and `Serial` output.

To make USB MSC appear on the USB-C port, we must **swap the PHY
assignment at runtime** so that DWC2 → PHY 0 (USB-C).

---

## The Three Framework Bugs

### Bug 1 — Wrong max-packet-size (MPS) for Full-Speed bulk endpoints

**File:** `cores/esp32/USBMSC.cpp` and `USBCDC.cpp`

The Arduino ESP32 framework's `esp32-hal-tinyusb.h` defines:
```c
#define CFG_TUD_ENDOINT_SIZE 512   // for ESP32-P4
```

This is the **High-Speed** bulk MPS.  But the Tab5 uses the FSLS PHY
(Full-Speed only), where the USB 2.0 spec limits bulk MPS to **64 bytes**.

When the host sees a Full-Speed device with 512-byte bulk endpoints, it
rejects `SET_CONFIGURATION` and the device never mounts.

The `-DCFG_TUD_ENDOINT_SIZE=64` build flag *should* fix this via the
`#ifndef` guard in the header, but the framework sources are
**pre-compiled** into the PlatformIO package — the `#ifndef` was already
evaluated at framework build time, not at your project's build time.

**Fix:** Replace `CFG_TUD_ENDOINT_SIZE` with the literal `64` in the
source files.

### Bug 2 — Wrong FIFO depth in TinyUSB DWC2 driver

**File:** `libarduino_tinyusb.a` (pre-compiled)

TinyUSB's `_dwc2_controller[]` table sets `ep_fifo_size = 1024` for
ESP32-P4 (implying 256 × 4-byte words).  But the OTG11 controller's
`GHWCFG3` register reports only **200 words** (800 bytes).

The driver allocates the non-periodic TX FIFO beyond the hardware's
actual boundary, so bulk IN data never makes it to the host.

**Fix:** Binary-patch `1024` → `800` at two offsets in the `.a` file.

### Bug 3 — PHY mux default & USB_WRAP register instability

Not a framework *code* bug per se, but the default PHY assignment means
DWC2 never reaches the USB-C port.  Additionally, the PHY initialisation
path can silently clear the DP pull-up bit in the `USB_WRAP.otg_conf`
register, preventing the host from detecting the device.

**Fix:** Runtime PHY swap via `LP_SYS` registers, with a microsecond
timer to enforce the pull-up until enumeration completes.

---

## Quick Start

### Prerequisites

- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html)
  or the PlatformIO IDE extension for VS Code
- M5Stack Tab5 with a micro-SD card inserted
- USB-C cable

### Steps

```bash
# 1. Clone the repository
git clone https://github.com/user/M5Stack-Tab5-USB-MSC.git
cd M5Stack-Tab5-USB-MSC

# 2. Build once (downloads framework packages)
pio run

# 3. Apply the framework patches
chmod +x patches/apply_patches.sh
./patches/apply_patches.sh

# 4. Rebuild with patches applied
pio run

# 5. Flash
pio run -t upload

# 6. Connect the Tab5's USB-C port to your computer
#    → The SD card appears as a removable drive!
```

### Reverting patches

```bash
./patches/revert_patches.sh
```

---

## How It Works (Runtime)

The firmware performs these steps in `initUsbMsc()`:

1. **Configure MSC** — Set vendor/product strings, register read/write
   callbacks backed by `SD_MMC.readRAW()` / `writeRAW()`.
2. **`USB.begin()`** — Start TinyUSB on OTG11 (FSLS mode).
3. **Soft-disconnect** — Set `DCTL.SftDiscon` so the host doesn't see a
   half-configured device.
4. **PHY swap** — `LP_SYS.usb_ctrl.sw_usb_phy_sel = 1` routes DWC2 →
   PHY 0 (USB-C port).
5. **VBUS override** — Force B-device / session-valid bits in `GOTGCTL`.
6. **Force Full-Speed** — Set `DCFG.DevSpd = 1`.
7. **DP pull-up** — Set `USB_WRAP.otg_conf` bits for `pad_pull_override`
   + `dp_pullup`, with a 500 µs enforcement timer.
8. **Soft-connect** — Clear `DCTL.SftDiscon`.  The host detects the
   device and begins enumeration.

---

## Project Structure

```
├── platformio.ini          # Build configuration & flags
├── partitions.csv          # Flash partition table
├── src/
│   └── main.cpp            # Minimal USB MSC example (~200 lines)
├── patches/
│   ├── apply_patches.sh    # Apply all 3 framework patches
│   ├── revert_patches.sh   # Restore originals from .bak
│   └── README.md           # Detailed patch descriptions
└── README.md               # This file
```

---

## Key Build Flags

| Flag | Purpose |
|------|---------|
| `-DARDUINO_USB_MODE=1` | Use TinyUSB stack (not the default CDC-only mode) |
| `-DARDUINO_USB_CDC_ON_BOOT=1` | Keep CDC serial for debug output before PHY swap |
| `-DARDUINO_USB_FSLS_P4=1` | Select the Full-Speed / Low-Speed PHY on ESP32-P4 |
| `-DCFG_TUD_ENDOINT_SIZE=64` | Intended MPS override (needs source patch to take effect) |
| `-DCFG_TUD_ENDOINT0_SIZE=64` | EP0 MPS override |
| `-DBOARD_HAS_PSRAM` | Enable PSRAM (Tab5 has 16 MB) |

---

## Tested With

- **Hardware:** M5Stack Tab5 (ESP32-P4)
- **Framework:** pioarduino `54.03.21`
- **Host OS:** macOS

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Build error about `tud_mounted` | Framework not using TinyUSB | Ensure `-DARDUINO_USB_MODE=1` is set |
| Device appears briefly then disconnects | MPS patch not applied | Run `apply_patches.sh` and rebuild |
| No USB device detected at all | FIFO patch not applied, or PHY swap not happening | Check all three patches; verify Tab5 USB-C port is used |
| SD card not detected | Card not inserted or not FAT32 | Try a different SD card; check `SD_MMC.begin()` return value |

---

## Related Issues

- [espressif/arduino-esp32 — USB MSC on ESP32-P4](https://github.com/espressif/arduino-esp32/issues) (no official fix as of June 2025)
- [espressif/esp-idf — DWC2 FIFO depth for OTG11](https://github.com/espressif/esp-idf/issues)

---

## License

MIT — see [LICENSE](LICENSE).
