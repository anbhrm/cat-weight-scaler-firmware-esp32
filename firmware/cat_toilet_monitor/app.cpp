#include "app.h"

void CatToiletApplication::begin() {
  Serial.begin(CatScaleConfig::SERIAL_BAUD);
  delay(800);
  Serial.println();
  Serial.println(F("=== Cat Toilet Monitor ==="));
  Serial.print(F("firmware="));
  Serial.print(CatScaleConfig::FIRMWARE_VERSION);
  Serial.println(F(" board=XIAO_ESP32C3"));
  Serial.print(F("pins=DOUT:GPIO"));
  Serial.print(CatScaleConfig::PIN_HX711_DOUT);
  Serial.print(F(",SCK:GPIO"));
  Serial.print(CatScaleConfig::PIN_HX711_SCK);
  Serial.print(F(",BUTTON:GPIO"));
  Serial.println(CatScaleConfig::PIN_MAINTENANCE_BUTTON);
  Serial.print(F("calibration_factor="));
  Serial.println(CatScaleConfig::CALIBRATION_FACTOR, 6);
  Serial.println(F("network=credentials_loaded_without_printing"));

  timeService_.begin();
  storageReady_ = eventStore_.begin();
  measurement_.begin(&scale_, timeService_.bootId(), eventCallback, this);
  network_.begin(&eventStore_, &timeService_, &measurement_);

  if (!storageReady_) {
    Serial.println(F("startup=degraded reason=event_storage_unavailable; events will not be discarded silently"));
  }
  Serial.println(F("startup=complete; keep toilet_and_litter_still and keep_cat_off until measurement=ready"));
}

void CatToiletApplication::loop() {
  measurement_.update();
  delay(1);
}

bool CatToiletApplication::eventCallback(EventRecord& event, void* context) {
  if (context == nullptr) return false;
  return static_cast<CatToiletApplication*>(context)->saveEvent(event);
}

bool CatToiletApplication::saveEvent(EventRecord& event) {
  if (!storageReady_) {
    Serial.println(F("event=save_failed reason=storage_not_ready"));
    return false;
  }
  timeService_.fillEventTimestamps(event);
  const bool saved = eventStore_.save(event);
  if (!saved) {
    Serial.println(F("event=save_failed reason=littlefs"));
  }
  return saved;
}
