/**
 * M5Stack Tab5 USB Mass Storage — Minimal Working Example
 *
 * Shares the SD card on the M5Stack Tab5 (ESP32-P4) as a USB mass
 * storage device over the USB-C port.
 *
 * Three framework bugs must be patched before this will work.
 * See patches/README.md and the project README for details.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <M5Unified.h>
#include <SD_MMC.h>
#include <USB.h>
#include <USBMSC.h>
#include "tusb.h"
#include "soc/lp_system_struct.h"
#include "soc/usb_wrap_struct.h"
#include "esp_timer.h"

// ───────────── USB-WRAP register enforcement timer ─────────────
// The PHY initialisation path can silently clear the DP pull-up
// that the device needs to be detected on the bus.  A lightweight
// periodic timer re-applies the correct bits until enumeration
// completes.

static esp_timer_handle_t s_pullupTimer = nullptr;
static volatile bool      s_pullupTimerActive = false;

// Bits to clear: exchg_override(5), exchg_pins(6),
//                pad_pull_override(12), dp_pullup(13),
//                dm_pullup(14), dp_pulldown(15),
//                dm_pulldown(16), pullup_value(17).
// We deliberately leave bit 18 (usb_pad_enable) and
// bit 20 (phy_clk_force_on) untouched.
static const uint32_t kWrapClear = (0x3u << 5) | (0x3Fu << 12);
static volatile uint32_t s_timerWrapBits = 0;

static void IRAM_ATTR pullupTimerCb(void* /*arg*/) {
  uint32_t val = USB_WRAP.otg_conf.val;
  val &= ~kWrapClear;
  val |= s_timerWrapBits;
  USB_WRAP.otg_conf.val = val;
}

// ───────────── MSC block-device callbacks ─────────────

static USBMSC  s_msc;
static int      s_sectorSize  = 0;
static uint32_t s_sectorCount = 0;

static int32_t onMscRead(uint32_t lba, uint32_t /*offset*/,
                         void* buffer, uint32_t bufsize) {
  uint32_t count = bufsize / s_sectorSize;
  for (uint32_t i = 0; i < count; i++) {
    if (!SD_MMC.readRAW((uint8_t*)buffer + i * s_sectorSize, lba + i))
      return -1;
  }
  return bufsize;
}

static int32_t onMscWrite(uint32_t lba, uint32_t /*offset*/,
                          uint8_t* buffer, uint32_t bufsize) {
  uint32_t count = bufsize / s_sectorSize;
  for (uint32_t i = 0; i < count; i++) {
    if (!SD_MMC.writeRAW(buffer + i * s_sectorSize, lba + i))
      return -1;
  }
  return bufsize;
}

static bool onMscStartStop(uint8_t power_condition, bool start,
                           bool load_eject) {
  Serial.printf("[MSC] StartStop: power=%d start=%d eject=%d\n",
                power_condition, start, load_eject);
  return true;
}

// ───────────── Core logic ─────────────

static bool initUsbMsc() {
  s_sectorSize  = SD_MMC.sectorSize();
  s_sectorCount = SD_MMC.numSectors();
  if (s_sectorSize == 0 || s_sectorCount == 0) {
    Serial.println("[MSC] SD card returned 0 sector size/count");
    return false;
  }
  Serial.printf("[MSC] SD card: %u sectors × %d bytes = %u MB\n",
                s_sectorCount, s_sectorSize,
                (uint32_t)((uint64_t)s_sectorCount * s_sectorSize / 1048576));

  // ── 1.  Configure MSC descriptors ──
  s_msc.vendorID("M5Stack");
  s_msc.productID("Tab5 SD Card");
  s_msc.productRevision("1.0");
  s_msc.onRead(onMscRead);
  s_msc.onWrite(onMscWrite);
  s_msc.onStartStop(onMscStartStop);
  s_msc.isWritable(true);
  s_msc.begin(s_sectorCount, s_sectorSize);
  s_msc.mediaPresent(true);

  // Pointer to DWC2 OTG11 register block (Full-Speed controller)
  volatile uint32_t* dwc2 = (volatile uint32_t*)0x50040000;

  // ── 2.  Start TinyUSB (attaches to OTG11 in FSLS mode) ──
  USB.begin();

  // ── 3.  Soft-disconnect while we reconfigure the PHY ──
  dwc2[0x804 / 4] |= (1 << 1);   // DCTL.SftDiscon = 1
  delay(100);

  // ── 4.  PHY swap — route DWC2 → PHY 0 (USB-C port) ──
  // By default the DWC2 controller is routed to PHY 1 (internal)
  // and the USB Serial/JTAG (USJ) controller owns PHY 0 (USB-C).
  // We swap them so the DWC2 (and therefore TinyUSB MSC) appears
  // on the external USB-C connector.
  LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 1;   // software-controlled mux
  LP_SYS.usb_ctrl.sw_usb_phy_sel    = 1;   // DWC2 → PHY 0
  delay(50);

  // ── 5.  VBUS override (device / B-device) ──
  uint32_t gotgctl = dwc2[0];
  gotgctl |= (1 << 2) | (1 << 3) | (1 << 6) | (1 << 7);
  dwc2[0] = gotgctl;

  // ── 6.  Create pull-up enforcement timer ──
  if (!s_pullupTimer) {
    esp_timer_create_args_t args = {};
    args.callback        = pullupTimerCb;
    args.name            = "usb_pullup";
    args.dispatch_method = ESP_TIMER_TASK;
    esp_timer_create(&args, &s_pullupTimer);
  }

  // ── 7.  Force DCFG.DevSpd = 1 (Full-Speed) ──
  {
    uint32_t dcfg = dwc2[0x800 / 4];
    dcfg &= ~0x3;
    dcfg |= 1;
    dwc2[0x800 / 4] = dcfg;
  }

  // ── 8.  Apply USB_WRAP: pad_pull_override + dp_pullup ──
  {
    uint32_t val = USB_WRAP.otg_conf.val;
    val &= ~kWrapClear;
    val |= (1u << 12) | (1u << 13);   // pad_pull_override + dp_pullup
    USB_WRAP.otg_conf.val = val;
  }
  USB_WRAP.otg_conf.usb_pad_enable   = 1;
  USB_WRAP.otg_conf.phy_clk_force_on = 1;

  // ── 9.  Start enforcement timer (500 µs period) ──
  s_timerWrapBits = (1u << 12) | (1u << 13);
  esp_timer_start_periodic(s_pullupTimer, 500);
  s_pullupTimerActive = true;

  // ── 10. Soft-connect — device appears on the bus ──
  dwc2[0x804 / 4] &= ~(1 << 1);   // DCTL.SftDiscon = 0

  // ── 11. Wait for host to mount (up to 5 s) ──
  bool mounted = false;
  for (int i = 0; i < 50; i++) {
    delay(100);
    if (tud_mounted()) { mounted = true; break; }
  }

  // Stop enforcement timer
  if (s_pullupTimerActive) {
    esp_timer_stop(s_pullupTimer);
    s_pullupTimerActive = false;
  }

  return mounted;
}

// ───────────── Arduino entry points ─────────────

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  auto& lcd = M5.Display;
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextSize(2);
  lcd.drawString("M5Stack Tab5 USB MSC Demo", 10, 10);
  lcd.drawString("Mounting SD card...", 10, 40);

  // Mount SD card (M5Stack Tab5 uses SD_MMC in 4-bit mode)
  if (!SD_MMC.begin("/sdcard", false, false)) {
    lcd.setTextColor(TFT_RED, TFT_BLACK);
    lcd.drawString("SD mount FAILED!", 10, 70);
    Serial.println("[MSC] SD_MMC.begin() failed");
    return;
  }
  lcd.drawString("SD OK. Starting USB MSC...", 10, 70);
  Serial.printf("[MSC] SD card mounted: %llu MB\n",
                SD_MMC.cardSize() / 1048576);

  bool ok = initUsbMsc();

  lcd.fillScreen(TFT_BLACK);
  if (ok) {
    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setTextSize(3);
    lcd.drawString("USB MSC Mounted!", 10, 10);
    lcd.setTextSize(2);
    lcd.drawString("SD card visible on host.", 10, 50);
    lcd.drawString("Tap screen to eject & reboot.", 10, 80);
    Serial.println("[MSC] *** MOUNTED — host sees the drive ***");
  } else {
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.drawString("USB MSC: Active (not mounted)", 10, 10);
    lcd.drawString("Connect a USB cable to a host.", 10, 40);
    lcd.drawString("Tap screen to reboot.", 10, 70);
    Serial.println("[MSC] Active but not mounted yet");
  }
}

void loop() {
  M5.update();

  // Tap anywhere to eject media and reboot
  if (M5.Touch.getCount() > 0) {
    auto& lcd = M5.Display;
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.drawString("Ejecting & rebooting...", 10, 10);

    s_msc.mediaPresent(false);
    delay(500);
    ESP.restart();
  }

  delay(50);
}
