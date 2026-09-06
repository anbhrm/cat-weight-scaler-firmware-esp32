#pragma once

#include <Arduino.h>

enum class MonitorState : uint8_t {
  STARTUP_ZERO = 0,
  IDLE,
  ENTRY_CANDIDATE,
  OCCUPIED,
  EXIT_CANDIDATE,
  POST_EXIT,
  TIMEOUT_BLOCKED,
  MAINTENANCE,
  SENSOR_FAULT,
};

struct EventRecord {
  uint64_t sequence = 0;
  uint32_t bootId = 0;
  uint32_t startedMillis = 0;
  uint32_t endedMillis = 0;
  String eventId;
  String startedAt;
  String endedAt;
  bool hasStartedAt = false;
  bool hasEndedAt = false;
  uint32_t durationSec = 0;
  float weightG = 0.0f;
  bool hasWeight = false;
  float baselineBeforeG = 0.0f;
  bool hasBaselineBefore = false;
  float baselineAfterG = 0.0f;
  bool hasBaselineAfter = false;
  float excretionG = 0.0f;
  bool hasExcretion = false;
  String quality = "low";
  String excretionQuality = "unavailable";
  String timeQuality = "unavailable";
  String terminationReason = "normal";
  uint32_t queueDroppedCount = 0;
};

inline const char* monitorStateName(MonitorState state) {
  switch (state) {
    case MonitorState::STARTUP_ZERO: return "STARTUP_ZERO";
    case MonitorState::IDLE: return "IDLE";
    case MonitorState::ENTRY_CANDIDATE: return "ENTRY_CANDIDATE";
    case MonitorState::OCCUPIED: return "OCCUPIED";
    case MonitorState::EXIT_CANDIDATE: return "EXIT_CANDIDATE";
    case MonitorState::POST_EXIT: return "POST_EXIT";
    case MonitorState::TIMEOUT_BLOCKED: return "TIMEOUT_BLOCKED";
    case MonitorState::MAINTENANCE: return "MAINTENANCE";
    case MonitorState::SENSOR_FAULT: return "SENSOR_FAULT";
  }
  return "UNKNOWN";
}
