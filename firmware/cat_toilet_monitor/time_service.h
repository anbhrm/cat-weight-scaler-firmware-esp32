#pragma once

#include <Arduino.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "config.h"
#include "types.h"

class TimeService {
 public:
  void begin();
  void refresh();

  bool isSynced() const;
  uint32_t bootId() const { return bootId_; }

  // Return an ISO-8601 JST timestamp for a monotonic event time. This works
  // only when the event was recorded during the current boot and NTP has
  // completed. Events from a previous boot remain explicitly unavailable.
  bool timestampFor(uint32_t eventMillis, uint32_t eventBootId, String& output) const;
  void fillEventTimestamps(EventRecord& event);

 private:
  bool snapshot(time_t& syncEpoch, uint32_t& syncMillis, uint32_t& bootId) const;
  static String formatJst(time_t epoch);

  volatile bool synced_ = false;
  time_t syncEpoch_ = 0;
  uint32_t syncMillis_ = 0;
  uint32_t bootId_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
