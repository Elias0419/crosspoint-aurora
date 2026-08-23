#include <BatteryMonitor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();

  // Serial-injected buttons: edges live for exactly one update() frame, and a
  // press never starts in the frame its predecessor releases in, so activities
  // see the same press → hold → release cadence real buttons produce.
  injectPressEdge = false;
  injectReleaseEdge = false;
  if (injectActive) {
    if (millis() - injectedPressStart >= injectedHoldMs) {
      injectActive = false;
      injectReleaseEdge = true;
    }
  } else if (pendingInjectButton != 0xFF) {
    injectedButton = pendingInjectButton;
    injectedHoldMs = pendingInjectHoldMs;
    pendingInjectButton = 0xFF;
    injectedPressStart = millis();
    injectActive = true;
    injectPressEdge = true;
  }

  // Injected touch gestures fire for exactly one update() frame.
  activeTouch = pendingTouch;
  pendingTouch = InjectTouch::None;

  // Track the CHARGING state, not the raw detect pin: boards with no usbDetect
  // GPIO would otherwise never report an edge, and main.cpp uses this edge to
  // repaint the battery icon when the cable goes in or out.
  const bool connected = isCharging();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

void HalGPIO::injectButton(uint8_t buttonIndex, unsigned long holdMs) {
  pendingInjectButton = buttonIndex;
  pendingInjectHoldMs = holdMs;
}

void HalGPIO::injectTouchTap(float nx, float ny) {
  pendingTouch = InjectTouch::Tap;
  injTouchX1 = nx;
  injTouchY1 = ny;
}

void HalGPIO::injectTouchLongPress(float nx, float ny) {
  pendingTouch = InjectTouch::LongPress;
  injTouchX1 = nx;
  injTouchY1 = ny;
}

void HalGPIO::injectSwipe(float nx1, float ny1, float nx2, float ny2) {
  pendingTouch = InjectTouch::Swipe;
  injTouchX1 = nx1;
  injTouchY1 = ny1;
  injTouchX2 = nx2;
  injTouchY2 = ny2;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

// A masked button is invisible to the normal queries — injection included, so a
// serial-injected press of that button exercises the same dispatcher path the
// real key does instead of a route the hardware no longer has.
bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (masked(buttonIndex)) return false;
  return inputMgr.isPressed(buttonIndex) || (injectActive && injectedButton == buttonIndex);
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (masked(buttonIndex)) return false;
  return inputMgr.wasPressed(buttonIndex) || (injectPressEdge && injectedButton == buttonIndex);
}

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed() || injectPressEdge; }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (masked(buttonIndex)) return false;
  return inputMgr.wasReleased(buttonIndex) || (injectReleaseEdge && injectedButton == buttonIndex);
}

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased() || injectReleaseEdge; }

// Raw (unmasked) accessors for the configurable-button dispatcher: it owns the
// masked button's edges, physical or injected.
bool HalGPIO::rawIsPressed(uint8_t buttonIndex) const {
  return inputMgr.isPressed(buttonIndex) || (injectActive && injectedButton == buttonIndex);
}

bool HalGPIO::rawWasPressed(uint8_t buttonIndex) const {
  return inputMgr.wasPressed(buttonIndex) || (injectPressEdge && injectedButton == buttonIndex);
}

bool HalGPIO::rawWasReleased(uint8_t buttonIndex) const {
  return inputMgr.wasReleased(buttonIndex) || (injectReleaseEdge && injectedButton == buttonIndex);
}

unsigned long HalGPIO::getHeldTime() const {
  const unsigned long real = inputMgr.getHeldTime();
  if (!injectActive && !injectReleaseEdge) return real;
  const unsigned long injected = millis() - injectedPressStart;
  return injected > real ? injected : real;
}

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const {
  if (activeTouch == InjectTouch::Tap) {
    nx = injTouchX1;
    ny = injTouchY1;
    return true;
  }
  return inputMgr.wasTouchTap(nx, ny);
}

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const {
  // An injected tap is a full contact: report its release edge too.
  return (inputMgr.wasTouchReleased()) || activeTouch == InjectTouch::Tap;
}

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isPowerButtonPhysicallyPressed() const { return digitalRead(InputManager::POWER_BUTTON_PIN) == LOW; }

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const {
  if (activeTouch == InjectTouch::LongPress) {
    nx = injTouchX1;
    ny = injTouchY1;
    return true;
  }
  return inputMgr.wasTouchLongPress(nx, ny);
}

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  if (activeTouch == InjectTouch::Swipe) {
    nxStart = injTouchX1;
    nyStart = injTouchY1;
    nxEnd = injTouchX2;
    nyEnd = injTouchY2;
    return true;
  }
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

// Injected touch counts as activity too: it comes from the serial debug
// channel, not from the glass, but it is still someone driving the device.
bool HalGPIO::wasTouchActivity() const { return activeTouch != InjectTouch::None || (inputMgr.wasTouchActivity()); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro;
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  // X4 Pro wakes on any power-button press; other boards retain the configured
  // hold-duration verification below.
  if (BoardConfig::isX4Pro() || BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }
#if defined(FREEINK_DEVICE_M5PAPER) && FREEINK_DEVICE_M5PAPER
  return true;
#endif
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return true;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot.
  const unsigned long calibration = millis();
  const unsigned long calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

namespace {
// Cached BQ25896 status register (REG0B). Both charging questions below come
// from this one byte, so it is read once per interval rather than once per
// question -- this is I2C on a bus shared with the panel PMIC.
bool readChargerStatusCached(uint8_t& out) {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.chargerAddr == 0) return false;

  static unsigned long lastPollMs = 0;
  static uint8_t cachedStatus = 0;
  static bool cachedOk = false;

  const unsigned long now = millis();
  if (lastPollMs != 0 && now - lastPollMs < HalGPIO::CHARGE_POLL_MS) {
    out = cachedStatus;
    return cachedOk;
  }
  lastPollMs = now;

  constexpr uint8_t BQ25896_REG_STATUS = 0x0B;
  TwoWire& bus =
#if SOC_I2C_NUM > 1
      (g.i2cBus == 1) ? Wire1 :
#endif
                      Wire;
  bus.beginTransmission(g.chargerAddr);
  bus.write(BQ25896_REG_STATUS);
  cachedOk = bus.endTransmission(false) == 0 && bus.requestFrom(static_cast<int>(g.chargerAddr), 1) == 1;
  if (cachedOk) cachedStatus = bus.read();
  out = cachedStatus;
  return cachedOk;
}
}  // namespace

bool HalGPIO::isCharging() const {
  // A dedicated detect pin is instant and authoritative wherever a board has one.
  if (BoardConfig::ACTIVE.usbDetect >= 0) {
    return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }

  // Prefer PG_STAT (power good) over the charger's CHRG_STAT. The question the
  // battery icon answers is "am I on the cable?", and CHRG_STAT cannot answer
  // it: once the pack tops off, the charger moves to 0b11 (termination done)
  // and BatteryMonitor::isCharging() -- which only accepts pre-charge (0b01) or
  // fast-charge (0b10) -- goes false while the cable is still plugged in. On
  // this board that is the NORMAL resting state, so the icon would spend most
  // of its plugged-in life looking exactly like an unplugged one. PG_STAT stays
  // set for as long as valid external power is present (verified on hardware:
  // it survives even an EN_HIZ input disconnect), which is what a phone's
  // charging glyph reports too.
  constexpr uint8_t BQ25896_PG_STAT = 1u << 2;
  uint8_t status = 0;
  if (readChargerStatusCached(status)) {
    return (status & BQ25896_PG_STAT) != 0;
  }

  // No charger IC (or the read failed): fall back to the gauge's own view.
  static const BatteryMonitor chargeMonitor;
  return chargeMonitor.isCharging();
}

bool HalGPIO::isChargeComplete() const {
  constexpr uint8_t BQ25896_PG_STAT = 1u << 2;
  constexpr uint8_t BQ25896_CHRG_DONE = 0x03;  // CHRG_STAT bits [4:3]
  uint8_t status = 0;
  if (!readChargerStatusCached(status)) return false;
  return (status & BQ25896_PG_STAT) != 0 && ((status >> 3) & 0x03) == BQ25896_CHRG_DONE;
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  if (BoardConfig::ACTIVE.usbDetect < 0) {
    return false;
  }
  return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
