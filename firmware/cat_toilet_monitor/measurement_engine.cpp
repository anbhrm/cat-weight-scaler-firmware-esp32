#include "measurement_engine.h"

#include <math.h>

namespace {
constexpr char MEASUREMENT_NAMESPACE[] = "cat-scale-meas";
constexpr uint32_t SENSOR_NOT_READY_LIMIT = 10;
constexpr uint32_t SENSOR_RECOVERY_SAMPLES = 10;
constexpr float STARTUP_RAW_RANGE_G = 20.0f;
constexpr float MAX_REASONABLE_CAT_WEIGHT_G = 20000.0f;
constexpr float MIN_REASONABLE_CAT_WEIGHT_G = 300.0f;
}

void MeasurementEngine::begin(HX711* scale, uint32_t bootId, EventReadyCallback callback,
                              void* context) {
  scale_ = scale;
  bootId_ = bootId;
  eventCallback_ = callback;
  callbackContext_ = context;
  state_ = MonitorState::STARTUP_ZERO;
  stateSince_ = millis();
  lastSampleAt_ = millis() - CatScaleConfig::SAMPLE_INTERVAL_MS;
  lastStatusAt_ = millis();

  pinMode(CatScaleConfig::PIN_MAINTENANCE_BUTTON, INPUT_PULLUP);
  preferences_.begin(MEASUREMENT_NAMESPACE, false);
  if (preferences_.isKey("offset")) {
    offset_ = preferences_.getLong64("offset", 0);
    offsetLoaded_ = true;
  }

  scale_->begin(CatScaleConfig::PIN_HX711_DOUT, CatScaleConfig::PIN_HX711_SCK);
  scale_->set_scale(calibrationFactor_);
  if (offsetLoaded_) {
    scale_->set_offset(offset_);
  }

  Serial.print(F("measurement=starting factor="));
  Serial.print(calibrationFactor_, 6);
  Serial.print(F(" sample_ms="));
  Serial.println(CatScaleConfig::SAMPLE_INTERVAL_MS);
  if (offsetLoaded_) {
    Serial.println(F("measurement=previous_offset_loaded; startup_tare_requires_30s_stability"));
  } else {
    Serial.println(F("measurement=no_saved_offset; startup_tare_requires_30s_stability"));
  }
  publishDiagnosticSnapshot(millis());
}

void MeasurementEngine::update() {
  const uint32_t now = millis();
  updateButton(now);
  bool sampled = false;
  if (static_cast<uint32_t>(now - lastSampleAt_) >= CatScaleConfig::SAMPLE_INTERVAL_MS) {
    lastSampleAt_ = now;
    sampled = sample(now);
    // State transitions and rolling windows must consume each HX711 sample once.
    // Running them on every fast loop() pass would duplicate the same value.
    if (sampled && hasFilteredSample_) {
      updateState(now);
    }
    publishDiagnosticSnapshot(now);
  }
  if (static_cast<uint32_t>(now - lastStatusAt_) >= CatScaleConfig::LOG_INTERVAL_MS) {
    lastStatusAt_ = now;
    printStatus(now);
  }
}

MeasurementDiagnosticSnapshot MeasurementEngine::diagnosticSnapshot() const {
  MeasurementDiagnosticSnapshot snapshot;
  portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&diagnosticMux_));
  snapshot = diagnosticSnapshot_;
  portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&diagnosticMux_));
  return snapshot;
}

void MeasurementEngine::publishDiagnosticSnapshot(uint32_t now) {
  MeasurementDiagnosticSnapshot snapshot;
  snapshot.state = state_;
  snapshot.hasSample = hasFilteredSample_;
  snapshot.startupTareDone = startupTareDone_;
  snapshot.currentGrams = filteredGrams_;
  snapshot.currentRaw = filteredRaw_;
  snapshot.lastReadyAt = lastReadyAt_;
  if (state_ == MonitorState::STARTUP_ZERO && startupReferenceSet_) {
    snapshot.startupStableElapsedMs = static_cast<uint32_t>(now - startupStableSince_);
  }

  portENTER_CRITICAL(&diagnosticMux_);
  diagnosticSnapshot_ = snapshot;
  portEXIT_CRITICAL(&diagnosticMux_);
}

bool MeasurementEngine::sample(uint32_t now) {
  if (scale_ == nullptr) return false;
  if (!scale_->is_ready()) {
    consecutiveReady_ = 0;
    if (consecutiveNotReady_ < 255) ++consecutiveNotReady_;
    if (consecutiveNotReady_ >= SENSOR_NOT_READY_LIMIT && state_ != MonitorState::SENSOR_FAULT) {
      if (state_ == MonitorState::OCCUPIED || state_ == MonitorState::EXIT_CANDIDATE ||
          state_ == MonitorState::POST_EXIT) {
        currentEvent_.endedMillis = now;
        currentEvent_.durationSec =
            static_cast<uint32_t>(now - currentEvent_.startedMillis) / 1000;
        currentEvent_.quality = "low";
        currentEvent_.hasBaselineAfter = false;
        currentEvent_.hasExcretion = false;
        currentEvent_.excretionQuality = "unavailable";
        currentEvent_.terminationReason = "sensor_fault";
        sensorInterruptedEvent_ = true;
      }
      enterState(MonitorState::SENSOR_FAULT, F("HX711_not_ready"));
      Serial.println(F("sensor=fault action=unplug_power_check_wiring"));
    }
    return false;
  }

  consecutiveNotReady_ = 0;
  if (consecutiveReady_ < 255) ++consecutiveReady_;
  lastReadyAt_ = now;
  const long raw = scale_->read_average(1);
  const float grams = static_cast<float>((static_cast<double>(raw) -
                                          static_cast<double>(scale_->get_offset())) /
                                         static_cast<double>(calibrationFactor_));
  if (!isfinite(grams)) return false;

  rawHistory_[filterNext_] = static_cast<float>(raw);
  gramHistory_[filterNext_] = grams;
  filterNext_ = (filterNext_ + 1) % CatScaleConfig::FILTER_WINDOW_SAMPLES;
  if (filterCount_ < CatScaleConfig::FILTER_WINDOW_SAMPLES) ++filterCount_;

  filteredRaw_ = static_cast<long long>(llround(medianOf(rawHistory_, filterCount_)));
  filteredGrams_ = medianOf(gramHistory_, filterCount_);
  hasFilteredSample_ = true;

  stableHistory_[stableNext_] = filteredGrams_;
  stableNext_ = (stableNext_ + 1) % CatScaleConfig::STABLE_WINDOW_SAMPLES;
  if (stableCount_ < CatScaleConfig::STABLE_WINDOW_SAMPLES) ++stableCount_;

  if (state_ == MonitorState::SENSOR_FAULT && consecutiveReady_ >= SENSOR_RECOVERY_SAMPLES) {
    if (sensorInterruptedEvent_) {
      sensorInterruptedEvent_ = false;
      if (!publishEvent(currentEvent_)) {
        unsavedEventPending_ = true;
        tareAfterPendingSave_ = false;
        startupAfterPendingSave_ = true;
        blockedWaitForUnload_ = false;
        lastSaveRetryAt_ = now;
        enterState(MonitorState::TIMEOUT_BLOCKED, F("sensor_fault_event_save_failed"));
        return true;
      }
      currentEvent_ = EventRecord();
      Serial.println(F("event=sensor_fault saved"));
    }
    clearMeasurementWindows();
    startupStableSince_ = now;
    startupReferenceSet_ = false;
    enterState(MonitorState::STARTUP_ZERO, F("HX711_recovered"));
  }
  return true;
}

float MeasurementEngine::medianOf(const float* values, size_t count) const {
  if (count == 0) return 0.0f;
  float copy[300] = {};
  const size_t bounded = min(count, static_cast<size_t>(300));
  for (size_t i = 0; i < bounded; ++i) copy[i] = values[i];
  for (size_t i = 1; i < bounded; ++i) {
    const float value = copy[i];
    size_t j = i;
    while (j > 0 && copy[j - 1] > value) {
      copy[j] = copy[j - 1];
      --j;
    }
    copy[j] = value;
  }
  if ((bounded & 1U) != 0) return copy[bounded / 2];
  return (copy[(bounded / 2) - 1] + copy[bounded / 2]) / 2.0f;
}

void MeasurementEngine::updateButton(uint32_t now) {
  const bool reading = (digitalRead(CatScaleConfig::PIN_MAINTENANCE_BUTTON) == LOW);
  if (reading != rawButtonState_) {
    rawButtonState_ = reading;
    buttonChangedAt_ = now;
  }
  if (static_cast<uint32_t>(now - buttonChangedAt_) < CatScaleConfig::BUTTON_DEBOUNCE_MS ||
      stableButtonState_ == rawButtonState_) {
    if (stableButtonState_ && !longPressHandled_ &&
        static_cast<uint32_t>(now - buttonPressedAt_) >=
            CatScaleConfig::MAINTENANCE_LONG_PRESS_MS) {
      longPressHandled_ = true;
      if (state_ == MonitorState::IDLE) {
        enterState(MonitorState::MAINTENANCE, F("button_long_press"));
        Serial.println(F("button=maintenance_enter"));
      } else if (state_ == MonitorState::MAINTENANCE) {
        float median = 0.0f;
        float range = 0.0f;
        if (!stableWeightWindow(median, range)) {
          Serial.println(F("button=maintenance_exit_deferred reason=platform_not_stable"));
        } else if (performTare(F("maintenance_end"))) {
          idleBaselineG_ = 0.0f;
          enterState(MonitorState::IDLE, F("maintenance_end"));
        }
      } else {
        Serial.print(F("button=ignored state="));
        Serial.println(monitorStateName(state_));
      }
    }
    return;
  }

  stableButtonState_ = rawButtonState_;
  if (stableButtonState_) {
    buttonPressedAt_ = now;
    longPressHandled_ = false;
    return;
  }

  const uint32_t heldMs = static_cast<uint32_t>(now - buttonPressedAt_);
  if (!longPressHandled_ && heldMs >= CatScaleConfig::MANUAL_TARE_MIN_MS &&
      heldMs < CatScaleConfig::MAINTENANCE_LONG_PRESS_MS) {
    if (state_ == MonitorState::IDLE || state_ == MonitorState::STARTUP_ZERO) {
      if (performTare(F("button_manual")) && state_ == MonitorState::STARTUP_ZERO) {
        startupTareDone_ = true;
        idleBaselineG_ = 0.0f;
        enterState(MonitorState::IDLE, F("startup_manual_tare_done"));
        Serial.println(F("measurement=ready; automatic event detection enabled"));
      }
    } else {
      Serial.print(F("button=tare_ignored state="));
      Serial.println(monitorStateName(state_));
    }
  }
}

void MeasurementEngine::updateState(uint32_t now) {
  updateOneSecondWindow(now);
  switch (state_) {
    case MonitorState::STARTUP_ZERO: updateStartup(now); break;
    case MonitorState::IDLE: updateIdle(now); break;
    case MonitorState::ENTRY_CANDIDATE: updateEntryCandidate(now); break;
    case MonitorState::OCCUPIED: updateOccupied(now); break;
    case MonitorState::EXIT_CANDIDATE: updateExitCandidate(now); break;
    case MonitorState::POST_EXIT: updatePostExit(now); break;
    case MonitorState::TIMEOUT_BLOCKED: updateBlocked(now); break;
    case MonitorState::MAINTENANCE: updateMaintenance(now); break;
    case MonitorState::SENSOR_FAULT: break;
  }
}

void MeasurementEngine::updateOneSecondWindow(uint32_t now) {
  if (secondBucketCount_ == 0) secondBucketSince_ = now;
  if (secondBucketCount_ < 10) secondBucket_[secondBucketCount_++] = filteredGrams_;
  if (static_cast<uint32_t>(now - secondBucketSince_) < 1000 || secondBucketCount_ == 0) return;

  const float secondMedian = medianOf(secondBucket_, secondBucketCount_);
  secondsHistory_[secondsNext_] = secondMedian;
  secondsNext_ = (secondsNext_ + 1) % 300;
  if (secondsCount_ < 300) ++secondsCount_;
  secondBucketCount_ = 0;
  secondBucketSince_ = now;
}

bool MeasurementEngine::stableStartupWindow() const {
  if (!startupReferenceSet_ || filterCount_ == 0) return false;
  const double rawRange = static_cast<double>(STARTUP_RAW_RANGE_G) *
                          static_cast<double>(calibrationFactor_);
  return static_cast<double>(startupMaxRaw_ - startupMinRaw_) <= fabs(rawRange);
}

void MeasurementEngine::updateStartup(uint32_t now) {
  if (!startupReferenceSet_) {
    startupReferenceRaw_ = filteredRaw_;
    startupMinRaw_ = filteredRaw_;
    startupMaxRaw_ = filteredRaw_;
    startupReferenceSet_ = true;
    startupStableSince_ = now;
  } else {
    startupMinRaw_ = min(startupMinRaw_, filteredRaw_);
    startupMaxRaw_ = max(startupMaxRaw_, filteredRaw_);
  }
  if (!stableStartupWindow()) {
    startupReferenceRaw_ = filteredRaw_;
    startupMinRaw_ = filteredRaw_;
    startupMaxRaw_ = filteredRaw_;
    startupStableSince_ = now;
  }

  if (static_cast<uint32_t>(now - startupStableSince_) < CatScaleConfig::STARTUP_STABLE_MS) return;

  if (!performTare(F("startup_stable"))) return;
  startupTareDone_ = true;
  idleBaselineG_ = 0.0f;
  enterState(MonitorState::IDLE, F("startup_tare_done"));
  Serial.println(F("measurement=ready; automatic event detection enabled"));
}

bool MeasurementEngine::autoTareWindowReady(float& center, float& width) const {
  if (secondsCount_ < 300) return false;
  float sorted[300] = {};
  for (size_t i = 0; i < secondsCount_; ++i) {
    // The ring order is irrelevant for a distribution check.
    sorted[i] = secondsHistory_[i];
  }
  for (size_t i = 1; i < secondsCount_; ++i) {
    const float value = sorted[i];
    size_t j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = value;
  }
  // 2.5th to 97.5th percentile is the central 95% of the samples.
  const size_t p025 = (secondsCount_ - 1) * 25 / 1000;
  const size_t p975 = (secondsCount_ - 1) * 975 / 1000;
  center = (sorted[p025] + sorted[p975]) / 2.0f;
  width = sorted[p975] - sorted[p025];
  return width <= CatScaleConfig::AUTO_TARE_BAND_G &&
         fabsf(filteredGrams_) <= CatScaleConfig::IDLE_AUTO_TARE_MAX_OFFSET_G &&
         fabsf(center) <= CatScaleConfig::IDLE_AUTO_TARE_MAX_OFFSET_G;
}

void MeasurementEngine::updateIdle(uint32_t now) {
  if (secondsCount_ > 0) {
    const size_t count = min(secondsCount_, static_cast<size_t>(30));
    float recent[30] = {};
    for (size_t i = 0; i < count; ++i) {
      const size_t index = (secondsNext_ + 300 - count + i) % 300;
      recent[i] = secondsHistory_[index];
    }
    float minValue = recent[0];
    float maxValue = recent[0];
    for (size_t i = 1; i < count; ++i) {
      minValue = min(minValue, recent[i]);
      maxValue = max(maxValue, recent[i]);
    }
    if ((maxValue - minValue) <= CatScaleConfig::STABLE_WINDOW_RANGE_G) {
      idleBaselineG_ = medianOf(recent, count);
    }
  }

  float center = 0.0f;
  float width = 0.0f;
  if (autoTareWindowReady(center, width)) {
    if (!performTare(F("automatic_5min_stable"))) return;
    idleBaselineG_ = 0.0f;
    Serial.print(F("tare=automatic center_g="));
    Serial.print(center, 1);
    Serial.print(F(" width_g="));
    Serial.println(width, 1);
    return;
  }

  if (aboveEntryThreshold(idleBaselineG_)) {
    startEntryCandidate(now, idleBaselineG_);
  }
}

bool MeasurementEngine::aboveEntryThreshold(float baseline) const {
  return filteredGrams_ - baseline >= CatScaleConfig::ENTRY_THRESHOLD_G;
}

void MeasurementEngine::startEntryCandidate(uint32_t now, float baseline) {
  if (entryCandidateSet_) return;
  entryCandidateSet_ = true;
  entryCandidateSince_ = now;
  entryBaselineG_ = baseline;
  enterState(MonitorState::ENTRY_CANDIDATE, F("load_over_1000g"));
}

void MeasurementEngine::updateEntryCandidate(uint32_t now) {
  if (!aboveEntryThreshold(entryBaselineG_)) {
    entryCandidateSet_ = false;
    enterState(MonitorState::IDLE, F("entry_candidate_reset"));
    return;
  }
  if (static_cast<uint32_t>(now - entryCandidateSince_) >= CatScaleConfig::ENTRY_CONFIRM_MS) {
    confirmEntry(now);
  }
}

void MeasurementEngine::confirmEntry(uint32_t now) {
  currentEvent_ = EventRecord();
  currentEvent_.bootId = bootId_;
  currentEvent_.startedMillis = entryCandidateSince_;
  currentEvent_.baselineBeforeG = entryBaselineG_;
  currentEvent_.hasBaselineBefore = true;
  currentEvent_.terminationReason = "normal";
  currentEvent_.quality = "low";
  currentEvent_.excretionQuality = "unavailable";
  currentEvent_.timeQuality = "unavailable";
  clearMeasurementWindows();
  // Keep the candidate's threshold-crossing timestamp but use fresh samples
  // for the cat-weight stability window.
  enterState(MonitorState::OCCUPIED, F("entry_confirmed"));
  entryCandidateSet_ = false;
  Serial.print(F("event=start_candidate confirmed_at_ms="));
  Serial.print(now);
  Serial.print(F(" started_at_ms="));
  Serial.println(currentEvent_.startedMillis);
}

bool MeasurementEngine::stableWeightWindow(float& median, float& range) const {
  if (stableCount_ < CatScaleConfig::STABLE_WINDOW_SAMPLES) return false;
  float minValue = stableHistory_[0];
  float maxValue = stableHistory_[0];
  for (size_t i = 1; i < stableCount_; ++i) {
    minValue = min(minValue, stableHistory_[i]);
    maxValue = max(maxValue, stableHistory_[i]);
  }
  range = maxValue - minValue;
  median = medianOf(stableHistory_, stableCount_);
  return range <= CatScaleConfig::STABLE_WINDOW_RANGE_G;
}

void MeasurementEngine::updateOccupied(uint32_t now) {
  float stableMedian = 0.0f;
  float stableRange = 0.0f;
  if (stableWeightWindow(stableMedian, stableRange) && !currentEvent_.hasWeight) {
    const float candidateWeight = stableMedian - currentEvent_.baselineBeforeG;
    if (candidateWeight >= MIN_REASONABLE_CAT_WEIGHT_G &&
        candidateWeight <= MAX_REASONABLE_CAT_WEIGHT_G) {
      currentEvent_.weightG = candidateWeight;
      currentEvent_.hasWeight = true;
      currentEvent_.quality = (stableRange <= CatScaleConfig::STABLE_WINDOW_RANGE_G) ? "good" : "low";
    }
  }

  if (static_cast<uint32_t>(now - currentEvent_.startedMillis) >= CatScaleConfig::EVENT_TIMEOUT_MS) {
    finishTimeout(now);
    return;
  }
  if (belowExitThreshold()) {
    beginExitCandidate(now);
  }
}

bool MeasurementEngine::belowExitThreshold() const {
  return filteredGrams_ - currentEvent_.baselineBeforeG < CatScaleConfig::EXIT_THRESHOLD_G;
}

void MeasurementEngine::beginExitCandidate(uint32_t now) {
  if (exitCandidateSet_) return;
  exitCandidateSet_ = true;
  exitCandidateSince_ = now;
  enterState(MonitorState::EXIT_CANDIDATE, F("load_below_200g"));
}

void MeasurementEngine::updateExitCandidate(uint32_t now) {
  if (!belowExitThreshold()) {
    exitCandidateSet_ = false;
    enterState(MonitorState::OCCUPIED, F("exit_candidate_reset"));
    return;
  }
  if (static_cast<uint32_t>(now - exitCandidateSince_) >= CatScaleConfig::EXIT_CONFIRM_MS) {
    beginPostExit(now);
  }
}

void MeasurementEngine::beginPostExit(uint32_t now) {
  postExitSince_ = now;
  postExitCount_ = 0;
  exitCandidateSet_ = false;
  enterState(MonitorState::POST_EXIT, F("exit_confirmed"));
}

void MeasurementEngine::updatePostExit(uint32_t now) {
  if (filteredGrams_ - currentEvent_.baselineBeforeG >= CatScaleConfig::ENTRY_THRESHOLD_G) {
    finishRapidReentry(now);
    return;
  }
  if (postExitCount_ < 100) postExitHistory_[postExitCount_++] = filteredGrams_;
  if (static_cast<uint32_t>(now - postExitSince_) < CatScaleConfig::POST_EXIT_SETTLE_MS) return;
  finishNormalEvent(now);
}

void MeasurementEngine::setEventQuality() {
  if (!currentEvent_.hasWeight) {
    currentEvent_.quality = "low";
  } else if (currentEvent_.quality != "good") {
    currentEvent_.quality = "low";
  }
}

void MeasurementEngine::finishNormalEvent(uint32_t now) {
  currentEvent_.endedMillis = exitCandidateSince_;
  currentEvent_.durationSec = static_cast<uint32_t>(currentEvent_.endedMillis -
                                                    currentEvent_.startedMillis) /
                              1000;
  setEventQuality();
  if (postExitCount_ > 0) {
    currentEvent_.baselineAfterG = medianOf(postExitHistory_, postExitCount_);
    currentEvent_.hasBaselineAfter = true;
    const float difference = currentEvent_.baselineAfterG - currentEvent_.baselineBeforeG;
    if (difference >= CatScaleConfig::EXCRETION_MIN_G &&
        difference <= CatScaleConfig::EXCRETION_MAX_G) {
      currentEvent_.excretionG = difference;
      currentEvent_.hasExcretion = true;
      const float postRange = [&]() {
        float minValue = postExitHistory_[0];
        float maxValue = postExitHistory_[0];
        for (size_t i = 1; i < postExitCount_; ++i) {
          minValue = min(minValue, postExitHistory_[i]);
          maxValue = max(maxValue, postExitHistory_[i]);
        }
        return maxValue - minValue;
      }();
      currentEvent_.excretionQuality =
          (CatScaleConfig::EXCRETION_GOOD_ENABLED &&
           difference >= CatScaleConfig::EXCRETION_GOOD_MIN_G &&
           postRange <= CatScaleConfig::EXCRETION_STABLE_RANGE_G &&
           static_cast<uint32_t>(now) >= CatScaleConfig::WARMUP_MS)
              ? "good"
              : "low";
    } else {
      currentEvent_.excretionQuality = "unavailable";
      currentEvent_.hasExcretion = false;
    }
  } else {
    currentEvent_.excretionQuality = "unavailable";
  }
  currentEvent_.terminationReason = "normal";
  if (publishEvent(currentEvent_)) {
    // Saving first is intentional: never tare away the difference before it
    // is durable in the local queue.
    currentEvent_ = EventRecord();
    if (performTare(F("event_saved"))) {
      idleBaselineG_ = 0.0f;
      enterState(MonitorState::IDLE, F("event_complete"));
    }
  } else {
    unsavedEventPending_ = true;
    tareAfterPendingSave_ = true;
    blockedWaitForUnload_ = false;
    lastSaveRetryAt_ = now;
    enterState(MonitorState::TIMEOUT_BLOCKED, F("event_save_failed"));
  }
}

void MeasurementEngine::finishRapidReentry(uint32_t now) {
  currentEvent_.endedMillis = exitCandidateSince_;
  currentEvent_.durationSec = static_cast<uint32_t>(currentEvent_.endedMillis -
                                                    currentEvent_.startedMillis) /
                              1000;
  currentEvent_.terminationReason = "rapid_reentry";
  currentEvent_.hasExcretion = false;
  currentEvent_.excretionQuality = "unavailable";
  if (!publishEvent(currentEvent_)) {
    // Do not start a second event while the first one could not be made
    // durable. The blocked state lets the operator repair storage/network
    // without silently throwing away the completed visit.
    unsavedEventPending_ = true;
    tareAfterPendingSave_ = false;
    blockedWaitForUnload_ = true;
    lastSaveRetryAt_ = now;
    enterState(MonitorState::TIMEOUT_BLOCKED, F("rapid_reentry_save_failed"));
    return;
  }
  Serial.println(F("event=rapid_reentry saved; excretion unavailable"));
  const float nextBaseline = postExitCount_ > 0
                                 ? medianOf(postExitHistory_, postExitCount_)
                                 : currentEvent_.baselineBeforeG;
  currentEvent_ = EventRecord();
  postExitCount_ = 0;
  entryCandidateSet_ = true;
  entryCandidateSince_ = now;
  entryBaselineG_ = nextBaseline;
  enterState(MonitorState::ENTRY_CANDIDATE, F("rapid_reentry_new_candidate"));
}

void MeasurementEngine::finishTimeout(uint32_t now) {
  currentEvent_.endedMillis = now;
  currentEvent_.durationSec = static_cast<uint32_t>(now - currentEvent_.startedMillis) / 1000;
  currentEvent_.terminationReason = "timeout";
  currentEvent_.hasBaselineAfter = false;
  currentEvent_.hasExcretion = false;
  currentEvent_.excretionQuality = "unavailable";
  setEventQuality();
  blockedWaitForUnload_ = true;
  tareAfterPendingSave_ = false;
  if (publishEvent(currentEvent_)) {
    currentEvent_ = EventRecord();
    enterState(MonitorState::TIMEOUT_BLOCKED, F("timeout_saved"));
  } else {
    unsavedEventPending_ = true;
    lastSaveRetryAt_ = now;
    enterState(MonitorState::TIMEOUT_BLOCKED, F("timeout_save_failed"));
  }
}

void MeasurementEngine::updateBlocked(uint32_t now) {
  if (unsavedEventPending_) {
    if (static_cast<uint32_t>(now - lastSaveRetryAt_) < 5000) return;
    lastSaveRetryAt_ = now;
    if (!publishEvent(currentEvent_)) {
      Serial.println(F("event=save_retry_failed"));
      return;
    }
    Serial.println(F("event=save_retry_succeeded"));
    unsavedEventPending_ = false;
    currentEvent_ = EventRecord();
    if (startupAfterPendingSave_) {
      startupAfterPendingSave_ = false;
      clearMeasurementWindows();
      startupStableSince_ = now;
      enterState(MonitorState::STARTUP_ZERO, F("pending_sensor_event_saved"));
      return;
    }
    if (tareAfterPendingSave_) {
      tareAfterPendingSave_ = false;
      if (performTare(F("event_saved_after_retry"))) {
        idleBaselineG_ = 0.0f;
        enterState(MonitorState::IDLE, F("event_complete_after_retry"));
      }
      return;
    }
  }

  if (!blockedWaitForUnload_) return;
  // A timed-out event is never tared while a load remains. Once the platform
  // is back near its baseline for 5s, monitoring can resume without hiding
  // that the timeout event had no reliable excretion amount.
  if (filteredGrams_ - idleBaselineG_ < CatScaleConfig::EXIT_THRESHOLD_G) {
    if (blockedLowSince_ == 0) blockedLowSince_ = now;
    if (static_cast<uint32_t>(now - blockedLowSince_) >= CatScaleConfig::EXIT_CONFIRM_MS) {
      if (performTare(F("blocked_load_cleared"))) {
        idleBaselineG_ = 0.0f;
        blockedLowSince_ = 0;
        blockedWaitForUnload_ = false;
        enterState(MonitorState::IDLE, F("blocked_load_cleared"));
      }
    }
  } else {
    blockedLowSince_ = 0;
  }
}

void MeasurementEngine::updateMaintenance(uint32_t /*now*/) {
  // Measurements are intentionally ignored while the owner cleans or refills
  // the tray. The 8-second button action exits this state and tares.
}

bool MeasurementEngine::publishEvent(EventRecord& event) {
  if (eventCallback_ == nullptr) {
    Serial.println(F("event=not_saved reason=no_callback"));
    return false;
  }
  event.bootId = bootId_;
  event.timeQuality = "unavailable";
  return eventCallback_(event, callbackContext_);
}

bool MeasurementEngine::performTare(const __FlashStringHelper* reason) {
  if (scale_ == nullptr) {
    Serial.println(F("tare=failed reason=HX711_not_ready"));
    enterState(MonitorState::SENSOR_FAULT, F("tare_HX711_not_ready"));
    return false;
  }

  // This function is often called immediately after consuming a 10 SPS
  // sample. At that instant DOUT is high while the HX711 performs its next
  // conversion, which is normal and must not be treated as a wiring fault.
  const uint32_t readyWaitStarted = millis();
  while (!scale_->is_ready() &&
         static_cast<uint32_t>(millis() - readyWaitStarted) < 1200) {
    delay(5);
  }
  if (!scale_->is_ready()) {
    Serial.println(F("tare=failed reason=HX711_ready_timeout"));
    enterState(MonitorState::SENSOR_FAULT, F("tare_HX711_ready_timeout"));
    return false;
  }
  Serial.print(F("tare=start reason="));
  Serial.println(reason);
  scale_->tare(CatScaleConfig::TARE_SAMPLES);
  offset_ = scale_->get_offset();
  preferences_.putLong64("offset", offset_);
  offsetLoaded_ = true;
  clearMeasurementWindows();
  Serial.print(F("tare=done offset="));
  Serial.println(static_cast<long long>(offset_));
  return true;
}

void MeasurementEngine::clearMeasurementWindows() {
  filterCount_ = 0;
  filterNext_ = 0;
  stableCount_ = 0;
  stableNext_ = 0;
  secondBucketCount_ = 0;
  secondsCount_ = 0;
  secondsNext_ = 0;
  hasFilteredSample_ = false;
  startupReferenceSet_ = false;
  startupMinRaw_ = 0;
  startupMaxRaw_ = 0;
}

bool MeasurementEngine::isEventState() const {
  return state_ == MonitorState::ENTRY_CANDIDATE || state_ == MonitorState::OCCUPIED ||
         state_ == MonitorState::EXIT_CANDIDATE || state_ == MonitorState::POST_EXIT;
}

void MeasurementEngine::enterState(MonitorState next, const __FlashStringHelper* reason) {
  if (state_ == next) return;
  if (next == MonitorState::TIMEOUT_BLOCKED) blockedLowSince_ = 0;
  state_ = next;
  stateSince_ = millis();
  Serial.print(F("state="));
  Serial.print(monitorStateName(next));
  if (reason != nullptr) {
    Serial.print(F(" reason="));
    Serial.print(reason);
  }
  Serial.println();
}

void MeasurementEngine::printStatus(uint32_t now) {
  Serial.print(F("state="));
  Serial.print(monitorStateName(state_));
  Serial.print(F(" weight_g="));
  if (hasFilteredSample_) Serial.print(filteredGrams_, 1);
  else Serial.print(F("NA"));
  Serial.print(F(" raw="));
  if (hasFilteredSample_) Serial.print(filteredRaw_);
  else Serial.print(F("NA"));
  Serial.print(F(" uptime_s="));
  Serial.println(now / 1000);
}
