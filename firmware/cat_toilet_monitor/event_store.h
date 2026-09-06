#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "types.h"

struct PendingEvent {
  String path;
  String json;
  EventRecord event;
};

class EventStore {
 public:
  EventStore() = default;

  bool begin();
  bool ready() const { return ready_; }

  // Saves an event atomically. The event may be updated with the current
  // queue-drop counter before it is serialized.
  bool save(EventRecord& event);
  bool nextPending(PendingEvent& pending);
  bool update(const String& path, EventRecord& event);
  bool markSent(const String& path);
  bool quarantine(const String& path, const char* reason);
  String webhookJson(const EventRecord& event) const;

  uint32_t droppedCount() const { return droppedCount_; }
  size_t pendingCount() const;
  size_t failedCount() const;

 private:
  bool lock(TickType_t timeout = portMAX_DELAY) const;
  void unlock() const;
  bool ensureDirectoriesLocked();
  bool recoverTemporaryFilesLocked();
  bool validateJsonLocked(const String& path, EventRecord* event = nullptr) const;
  bool loadLocked(const String& path, EventRecord& event, String& json) const;
  bool writeAtomicLocked(const String& path, EventRecord& event);
  bool ensureSpaceLocked(size_t requiredBytes);
  bool findOldestLocked(String& path) const;
  bool findOldestEvictableLocked(String& path) const;
  bool removeOldestLocked();
  String eventPath(uint64_t sequence) const;
  static String basename(const String& path);
  static String makeQuarantinePath(const String& path);

  static void serializeEvent(JsonDocument& doc, const EventRecord& event);
  static void serializeWebhook(JsonDocument& doc, const EventRecord& event);
  static bool deserializeEvent(JsonDocument& doc, EventRecord& event);

  mutable SemaphoreHandle_t mutex_ = nullptr;
  Preferences preferences_;
  bool ready_ = false;
  uint64_t nextSequence_ = 1;
  uint32_t droppedCount_ = 0;
};
