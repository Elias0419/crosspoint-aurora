#include "BatteryLog.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <Wire.h>

#include "CrossPointSettings.h"

namespace {

constexpr const char* LOG_PATH = "/.crosspoint/battery.csv";
constexpr unsigned long SAMPLE_INTERVAL_MS = 60UL * 1000UL;
constexpr unsigned long ROW_INTERVAL_MS = 5UL * 60UL * 1000UL;

// Accumulators have to survive deep sleep, which is a full power-down of RAM.
// RTC_NOINIT keeps them across that and across a reset, so a night spent asleep
// does not reset the refresh counter or lose the session's starting charge.
#define BATTERY_LOG_MAGIC 0x42544C47u  // 'BTLG'
RTC_NOINIT_ATTR uint32_t logMagic;
RTC_NOINIT_ATTR uint32_t logRefreshCount;
RTC_NOINIT_ATTR uint32_t logRowCount;
RTC_NOINIT_ATTR uint32_t logFrontlightOnMs;
RTC_NOINIT_ATTR uint32_t logWifiOnMs;
RTC_NOINIT_ATTR uint32_t logHighClockMs;

unsigned long lastSampleMs = 0;
unsigned long lastRowMs = 0;
unsigned long lastAccumMs = 0;
bool headerChecked = false;

// The gauge lives on whichever I2C controller BoardConfig names.
TwoWire& gaugeWire() {
#if SOC_I2C_NUM > 1
  if (BoardConfig::ACTIVE.batteryGauge.i2cBus == 1) return Wire1;
#endif
  return Wire;
}

bool readGauge16(uint8_t reg, uint16_t& out) {
  const uint8_t addr = BoardConfig::ACTIVE.batteryGauge.gaugeAddr;
  if (addr == 0) return false;
  TwoWire& bus = gaugeWire();
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom(static_cast<int>(addr), 2) != 2) return false;
  const uint8_t lo = bus.read();
  const uint8_t hi = bus.read();
  out = static_cast<uint16_t>(lo | (hi << 8));
  return true;
}

bool readCharger8(uint8_t reg, uint8_t& out) {
  const uint8_t addr = BoardConfig::ACTIVE.batteryGauge.chargerAddr;
  if (addr == 0) return false;
  TwoWire& bus = gaugeWire();
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom(static_cast<int>(addr), 1) != 1) return false;
  out = bus.read();
  return true;
}

// Time-weighted accumulators. MUST be called every main-loop pass, not on the
// sampling interval: this credits the whole elapsed slice to whatever state is
// observed right now, so sampling once a minute would attribute a full minute
// to an instant. That is fine for the frontlight, which changes slowly, and
// completely wrong for the CPU clock, which drops to 80 MHz three seconds after
// every page turn and jumps back for each render. First run of the log showed
// hi_clock_ms stuck at 0 for exactly that reason.
void accumulate() {
  const unsigned long now = millis();
  if (lastAccumMs == 0) {
    lastAccumMs = now;
    return;
  }
  const unsigned long dt = now - lastAccumMs;
  lastAccumMs = now;

  if (Frontlight.isOn()) logFrontlightOnMs += dt;
  if (WiFi.getMode() != WIFI_MODE_NULL) logWifiOnMs += dt;
  if (getCpuFrequencyMhz() > 100) logHighClockMs += dt;
}

void writeRow(const char* event) {
  if (!Storage.ready()) return;

  uint16_t remCap = 0, fcc = 0, mv = 0, rawCurrent = 0, soc = 0;
  readGauge16(0x10, remCap);
  readGauge16(0x12, fcc);
  readGauge16(0x08, mv);
  readGauge16(0x0C, rawCurrent);
  readGauge16(0x2C, soc);
  uint8_t chargerStatus = 0;
  const bool chargerOk = readCharger8(0x0B, chargerStatus);

  // O_AT_END rather than the usual openFileForWrite(): that one passes O_TRUNC,
  // which would wipe the log on every row. Storage::open() takes raw SdFat
  // flags, so an append-only log needs no new HAL surface.
  HalFile file = Storage.open(LOG_PATH, O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_DBG("BATTLOG", "Could not open %s", LOG_PATH);
    return;
  }

  // Header only when starting a fresh file, so the log survives reboots as one
  // continuous series rather than restarting each time.
  if (!headerChecked) {
    headerChecked = true;
    if (file.fileSize() == 0) {
      file.print(
          "uptime_s,rtc,event,remcap_mah,fcc_mah,mv,current_ma,soc,chrg_stat,pg,"
          "cpu_mhz,fl_on,fl_pct,fl_on_ms,wifi_on_ms,hi_clock_ms,refreshes,rows,free_heap\n");
    }
  }

  char rtcBuf[24] = "";
  if (!halClock.formatTime(rtcBuf, sizeof(rtcBuf))) rtcBuf[0] = '\0';

  char line[256];
  const int n =
      snprintf(line, sizeof(line), "%lu,%s,%s,%u,%u,%u,%d,%u,%d,%d,%u,%d,%u,%lu,%lu,%lu,%lu,%lu,%u\n",
               millis() / 1000UL, rtcBuf, event, remCap, fcc, mv, static_cast<int>(static_cast<int16_t>(rawCurrent)),
               soc, chargerOk ? ((chargerStatus >> 3) & 0x03) : -1, chargerOk ? ((chargerStatus & 0x04) ? 1 : 0) : -1,
               getCpuFrequencyMhz(), Frontlight.isOn() ? 1 : 0, Frontlight.brightness(),
               static_cast<unsigned long>(logFrontlightOnMs), static_cast<unsigned long>(logWifiOnMs),
               static_cast<unsigned long>(logHighClockMs), static_cast<unsigned long>(logRefreshCount),
               static_cast<unsigned long>(logRowCount), ESP.getFreeHeap());
  if (n > 0) {
    file.print(line);
    file.flush();
    ++logRowCount;
  }
  file.close();
}

}  // namespace

namespace BatteryLog {

void begin() {
  if (logMagic != BATTERY_LOG_MAGIC) {
    // Cold boot (or first run after flashing): start the counters clean. A wake
    // from deep sleep keeps them, which is the whole point of RTC_NOINIT.
    logMagic = BATTERY_LOG_MAGIC;
    logRefreshCount = 0;
    logRowCount = 0;
    logFrontlightOnMs = 0;
    logWifiOnMs = 0;
    logHighClockMs = 0;
  }
  display.setRefreshObserver(&noteDisplayRefresh);

  lastSampleMs = millis();
  lastRowMs = millis();
  lastAccumMs = millis();
  writeRow("BOOT");
}

void tick() {
  // Every pass: see accumulate()'s note on why this cannot ride the sample
  // interval. It is a handful of comparisons plus one addition.
  accumulate();

  const unsigned long now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  if (now - lastRowMs >= ROW_INTERVAL_MS) {
    lastRowMs = now;
    writeRow("SAMPLE");
  }
}

void flushNow(const char* event) {
  accumulate();
  writeRow(event);
  lastRowMs = millis();
}

void noteDisplayRefresh() { ++logRefreshCount; }

}  // namespace BatteryLog
