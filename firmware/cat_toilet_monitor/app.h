#pragma once

#include "event_store.h"
#include "measurement_engine.h"
#include "network_manager.h"
#include "time_service.h"

class CatToiletApplication {
 public:
  void begin();
  void loop();

 private:
  static bool eventCallback(EventRecord& event, void* context);
  bool saveEvent(EventRecord& event);

  HX711 scale_;
  TimeService timeService_;
  EventStore eventStore_;
  MeasurementEngine measurement_;
  CatScaleNetworkManager network_;
  bool storageReady_ = false;
};
