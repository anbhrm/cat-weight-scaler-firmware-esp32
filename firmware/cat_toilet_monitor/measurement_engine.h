#pragma once

#include <Arduino.h>
#include <HX711.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "config.h"
#include "types.h"

using EventReadyCallback = bool (*)(EventRecord& event, void* context);

struct MeasurementDiagnosticSnapshot {
  MonitorState state = MonitorState::STARTUP_ZERO;
  bool hasSample = false;
  bool startupTareDone = false;
  float currentGrams = 0.0f;
  long long currentRaw = 0;
  uint32_t lastReadyAt = 0;
  uint32_t startupStableElapsedMs = 0;
};

class MeasurementEngine {
 public:
  MeasurementEngine() = default;

  void begin(HX711* scale, uint32_t bootId, EventReadyCallback callback, void* context);
  void update();

  MonitorState state() const { return state_; }
  bool sensorFault() const { return state_ == MonitorState::SENSOR_FAULT; }
  float currentGrams() const { return filteredGrams_; }
  long long currentRaw() const { return filteredRaw_; }
  bool hasSample() const { return hasFilteredSample_; }
  MeasurementDiagnosticSnapshot diagnosticSnapshot() const;

 private:
  bool sample(uint32_t now);
  void updateButton(uint32_t now);
  void updateState(uint32_t now);
  void updateOneSecondWindow(uint32_t now);
  void updateStartup(uint32_t now);
  void updateIdle(uint32_t now);
  void updateEntryCandidate(uint32_t now);
  void updateOccupied(uint32_t now);
  void updateExitCandidate(uint32_t now);
  void updatePostExit(uint32_t now);
  void updateBlocked(uint32_t now);
  void updateMaintenance(uint32_t now);

  bool stableWeightWindow(float& median, float& range) const;
  bool stableStartupWindow() const;
  bool autoTareWindowReady(float& center, float& width) const;
  float medianOf(const float* values, size_t count) const;
  void clearMeasurementWindows();
  bool performTare(const __FlashStringHelper* reason);
  void startEntryCandidate(uint32_t now, float baseline);
  void confirmEntry(uint32_t now);
  void beginExitCandidate(uint32_t now);
  void beginPostExit(uint32_t now);
  void finishNormalEvent(uint32_t now);
  void finishRapidReentry(uint32_t now);
  void finishTimeout(uint32_t now);
  bool publishEvent(EventRecord& event);
  void enterState(MonitorState next, const __FlashStringHelper* reason = nullptr);
  bool isEventState() const;
  bool belowExitThreshold() const;
  bool aboveEntryThreshold(float baseline) const;
  void setEventQuality();
  void printStatus(uint32_t now);
  void publishDiagnosticSnapshot(uint32_t now);

  HX711* scale_ = nullptr;
  Preferences preferences_;
  uint32_t bootId_ = 0;
  EventReadyCallback eventCallback_ = nullptr;
  void* callbackContext_ = nullptr;

  float calibrationFactor_ = CatScaleConfig::CALIBRATION_FACTOR;
  long long offset_ = 0;
  bool offsetLoaded_ = false;
  bool startupTareDone_ = false;

  MonitorState state_ = MonitorState::STARTUP_ZERO;
  uint32_t stateSince_ = 0;
  uint32_t startupStableSince_ = 0;
  long long startupReferenceRaw_ = 0;
  long long startupMinRaw_ = 0;
  long long startupMaxRaw_ = 0;
  bool startupReferenceSet_ = false;
  uint32_t lastSampleAt_ = 0;
  uint32_t lastStatusAt_ = 0;
  uint32_t lastReadyAt_ = 0;
  uint8_t consecutiveNotReady_ = 0;
  uint8_t consecutiveReady_ = 0;

  float rawHistory_[CatScaleConfig::FILTER_WINDOW_SAMPLES] = {};
  float gramHistory_[CatScaleConfig::FILTER_WINDOW_SAMPLES] = {};
  size_t filterCount_ = 0;
  size_t filterNext_ = 0;
  bool hasFilteredSample_ = false;
  long long filteredRaw_ = 0;
  float filteredGrams_ = 0.0f;

  float stableHistory_[CatScaleConfig::STABLE_WINDOW_SAMPLES] = {};
  size_t stableCount_ = 0;
  size_t stableNext_ = 0;

  float secondBucket_[10] = {};
  size_t secondBucketCount_ = 0;
  uint32_t secondBucketSince_ = 0;
  float secondsHistory_[300] = {};
  size_t secondsCount_ = 0;
  size_t secondsNext_ = 0;

  float idleBaselineG_ = 0.0f;
  uint32_t entryCandidateSince_ = 0;
  float entryBaselineG_ = 0.0f;
  bool entryCandidateSet_ = false;

  EventRecord currentEvent_;
  uint32_t exitCandidateSince_ = 0;
  bool exitCandidateSet_ = false;
  uint32_t postExitSince_ = 0;
  size_t postExitCount_ = 0;
  float postExitHistory_[100] = {};

  bool unsavedEventPending_ = false;
  bool blockedWaitForUnload_ = false;
  bool tareAfterPendingSave_ = false;
  bool startupAfterPendingSave_ = false;
  bool sensorInterruptedEvent_ = false;
  uint32_t lastSaveRetryAt_ = 0;
  uint32_t blockedLowSince_ = 0;

  bool rawButtonState_ = false;
  bool stableButtonState_ = false;
  bool longPressHandled_ = false;
  uint32_t buttonChangedAt_ = 0;
  uint32_t buttonPressedAt_ = 0;

  MeasurementDiagnosticSnapshot diagnosticSnapshot_;
  mutable portMUX_TYPE diagnosticMux_ = portMUX_INITIALIZER_UNLOCKED;
};
