#include "time_service.h"

#include <esp_system.h>

namespace {
constexpr time_t MIN_VALID_EPOCH = 1700000000;  // 2023-11-14; rejects epoch 0.
constexpr time_t JST_OFFSET_SECONDS = 9 * 60 * 60;
}

void TimeService::begin() {
  // Keep the system clock as UTC. JST conversion is performed explicitly in
  // formatJst(), so it does not depend on the ESP32 process-wide TZ setting.
  configTime(0, 0, CatScaleConfig::NTP_SERVER_1, CatScaleConfig::NTP_SERVER_2);

  bootId_ = esp_random();
  if (bootId_ == 0) {
    bootId_ = 1;
  }
  portENTER_CRITICAL(&mux_);
  synced_ = false;
  syncEpoch_ = 0;
  syncMillis_ = 0;
  portEXIT_CRITICAL(&mux_);
}

void TimeService::refresh() {
  const time_t now = time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    return;
  }

  portENTER_CRITICAL(&mux_);
  syncEpoch_ = now;
  syncMillis_ = millis();
  synced_ = true;
  portEXIT_CRITICAL(&mux_);
}

bool TimeService::isSynced() const {
  bool result;
  portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&mux_));
  result = synced_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&mux_));
  return result;
}

bool TimeService::snapshot(time_t& syncEpoch, uint32_t& syncMillis, uint32_t& bootId) const {
  bool result;
  portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&mux_));
  result = synced_;
  syncEpoch = syncEpoch_;
  syncMillis = syncMillis_;
  bootId = bootId_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&mux_));
  return result;
}

String TimeService::formatJst(time_t epoch) {
  // JST is always UTC+09:00 and has no daylight-saving time. Add the fixed
  // offset first, then use the UTC calendar conversion so the date/time body
  // and the trailing +09:00 offset can never disagree.
  const time_t jstEpoch = epoch + JST_OFFSET_SECONDS;
  struct tm jstTime;
  if (gmtime_r(&jstEpoch, &jstTime) == nullptr) {
    return String();
  }

  char datePart[32] = {};
  if (strftime(datePart, sizeof(datePart), "%Y-%m-%dT%H:%M:%S", &jstTime) == 0) {
    return String();
  }
  return String(datePart) + "+09:00";
}

bool TimeService::timestampFor(uint32_t eventMillis, uint32_t eventBootId, String& output) const {
  time_t syncEpoch;
  uint32_t syncMillis;
  uint32_t bootId;
  if (!snapshot(syncEpoch, syncMillis, bootId) || eventBootId == 0 || eventBootId != bootId) {
    return false;
  }

  // Signed subtraction keeps the calculation correct across millis() wrap.
  const int32_t deltaMs = static_cast<int32_t>(eventMillis - syncMillis);
  const time_t eventEpoch = syncEpoch + static_cast<time_t>(deltaMs / 1000);
  output = formatJst(eventEpoch);
  return output.length() > 0;
}

void TimeService::fillEventTimestamps(EventRecord& event) {
  if (event.hasStartedAt && event.hasEndedAt) {
    return;
  }

  String timestamp;
  if (!event.hasStartedAt && timestampFor(event.startedMillis, event.bootId, timestamp)) {
    event.startedAt = timestamp;
    event.hasStartedAt = true;
  }
  if (!event.hasEndedAt && timestampFor(event.endedMillis, event.bootId, timestamp)) {
    event.endedAt = timestamp;
    event.hasEndedAt = true;
  }
  event.timeQuality = (event.hasStartedAt && event.hasEndedAt) ? "synced" : "unavailable";
}
