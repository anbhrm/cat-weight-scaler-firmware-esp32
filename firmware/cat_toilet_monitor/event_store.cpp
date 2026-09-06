#include "event_store.h"

#include <algorithm>
#include <cstring>
#include <Esp.h>

#include "config.h"

namespace {
constexpr char QUEUE_DIR[] = "/queue";
constexpr char FAILED_DIR[] = "/failed";
constexpr char STORE_NAMESPACE[] = "cat-scale-queue";
constexpr char FILE_PREFIX[] = "/queue/e-";
constexpr char FILE_SUFFIX[] = ".json";
constexpr char TEMP_SUFFIX[] = ".tmp";
constexpr size_t JSON_CAPACITY = 4096;
}

bool EventStore::lock(TickType_t timeout) const {
  return mutex_ != nullptr && xSemaphoreTake(mutex_, timeout) == pdTRUE;
}

void EventStore::unlock() const {
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
}

bool EventStore::begin() {
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
  }
  if (mutex_ == nullptr || !preferences_.begin(STORE_NAMESPACE, false)) {
    Serial.println(F("storage=failed reason=preferences"));
    return false;
  }

  droppedCount_ = preferences_.getUInt("dropped", 0);
  nextSequence_ = preferences_.getULong64("next_seq", 1);
  if (nextSequence_ == 0) {
    nextSequence_ = 1;
  }

  const bool fsMarkedInitialized = preferences_.getBool("fs_init", false);
  bool mounted = LittleFS.begin(false);
  if (!mounted && !fsMarkedInitialized) {
    Serial.println(F("storage=first_init action=format"));
    if (LittleFS.format()) {
      mounted = LittleFS.begin(false);
    }
  }
  if (!mounted) {
    Serial.println(F("storage=failed reason=littlefs_mount_no_format"));
    return false;
  }

  if (!lock()) {
    return false;
  }
  const bool directoriesOk = ensureDirectoriesLocked();
  const bool recoveryOk = directoriesOk && recoverTemporaryFilesLocked();
  unlock();
  if (!directoriesOk || !recoveryOk) {
    Serial.println(F("storage=failed reason=directory_or_recovery"));
    return false;
  }

  if (!fsMarkedInitialized) {
    preferences_.putBool("fs_init", true);
  }
  ready_ = true;
  Serial.print(F("storage=ready pending="));
  Serial.print(pendingCount());
  Serial.print(F(" dropped="));
  Serial.println(droppedCount_);
  return true;
}

bool EventStore::ensureDirectoriesLocked() {
  if (!LittleFS.exists(QUEUE_DIR) && !LittleFS.mkdir(QUEUE_DIR)) {
    return false;
  }
  if (!LittleFS.exists(FAILED_DIR) && !LittleFS.mkdir(FAILED_DIR)) {
    return false;
  }
  return true;
}

String EventStore::basename(const String& path) {
  const int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

String EventStore::makeQuarantinePath(const String& path) {
  const String name = basename(path);
  String candidate = String(FAILED_DIR) + "/" + name;
  uint16_t suffix = 1;
  while (LittleFS.exists(candidate) && suffix < 1000) {
    candidate = String(FAILED_DIR) + "/" + name + "." + String(suffix++);
  }
  return candidate;
}

bool EventStore::validateJsonLocked(const String& path, EventRecord* event) const {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    return false;
  }
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    return false;
  }
  if (event != nullptr && !deserializeEvent(doc, *event)) {
    return false;
  }
  return true;
}

bool EventStore::recoverTemporaryFilesLocked() {
  const char* directories[] = {QUEUE_DIR, FAILED_DIR};
  for (const char* directory : directories) {
    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) {
      return false;
    }
    File file = root.openNextFile();
    while (file) {
      // In ESP32 Arduino core 3.3.x File::name() returns only the basename.
      // Filesystem operations require the absolute path returned by path().
      const String current = file.path();
      file.close();
      file = root.openNextFile();
      if (!current.endsWith(TEMP_SUFFIX)) {
        continue;
      }

      if (validateJsonLocked(current)) {
        String finalPath = current.substring(0, current.length() - strlen(TEMP_SUFFIX));
        if (LittleFS.exists(finalPath)) {
          LittleFS.remove(finalPath);
        }
        if (!LittleFS.rename(current, finalPath)) {
          return false;
        }
        Serial.print(F("storage=recovered path="));
        Serial.println(finalPath);
      } else {
        const String corruptPath = current + ".corrupt";
        if (LittleFS.exists(corruptPath)) {
          LittleFS.remove(corruptPath);
        }
        if (!LittleFS.rename(current, corruptPath)) {
          return false;
        }
        Serial.print(F("storage=quarantined_corrupt_temp path="));
        Serial.println(corruptPath);
      }
    }
    root.close();
  }
  return true;
}

String EventStore::eventPath(uint64_t sequence) const {
  char number[24] = {};
  snprintf(number, sizeof(number), "%020llu", static_cast<unsigned long long>(sequence));
  return String(FILE_PREFIX) + number + FILE_SUFFIX;
}

void EventStore::serializeWebhook(JsonDocument& doc, const EventRecord& event) {
  doc["schema_version"] = 2;
  doc["device_id"] = CatScaleConfig::DEVICE_ID;
  doc["event_id"] = event.eventId;
  if (event.hasStartedAt) {
    doc["started_at"] = event.startedAt;
  } else {
    doc["started_at"] = nullptr;
  }
  if (event.hasEndedAt) {
    doc["ended_at"] = event.endedAt;
  } else {
    doc["ended_at"] = nullptr;
  }
  doc["duration_sec"] = event.durationSec;
  if (event.hasWeight) {
    doc["weight_g"] = event.weightG;
  } else {
    doc["weight_g"] = nullptr;
  }
  doc["quality"] = event.quality;
  if (event.hasBaselineBefore) {
    doc["baseline_before_g"] = event.baselineBeforeG;
  } else {
    doc["baseline_before_g"] = nullptr;
  }
  if (event.hasBaselineAfter) {
    doc["baseline_after_g"] = event.baselineAfterG;
  } else {
    doc["baseline_after_g"] = nullptr;
  }
  if (event.hasExcretion) {
    doc["excretion_g"] = event.excretionG;
  } else {
    doc["excretion_g"] = nullptr;
  }
  doc["excretion_quality"] = event.excretionQuality;
  doc["time_quality"] = event.timeQuality;
  doc["termination_reason"] = event.terminationReason;
  doc["queue_dropped_count"] = event.queueDroppedCount;
  doc["firmware_version"] = CatScaleConfig::FIRMWARE_VERSION;
}

void EventStore::serializeEvent(JsonDocument& doc, const EventRecord& event) {
  serializeWebhook(doc, event);

  // Internal timing fields allow an event saved before NTP synchronization to
  // receive timestamps later in the same boot. They are harmless diagnostics
  // for a webhook receiver and are deliberately not used after reboot.
  doc["_monotonic_started_ms"] = event.startedMillis;
  doc["_monotonic_ended_ms"] = event.endedMillis;
  doc["_boot_id"] = event.bootId;
  doc["_sequence"] = event.sequence;
}

String EventStore::webhookJson(const EventRecord& event) const {
  JsonDocument doc;
  serializeWebhook(doc, event);
  String json;
  serializeJson(doc, json);
  return json;
}

bool EventStore::deserializeEvent(JsonDocument& doc, EventRecord& event) {
  JsonVariantConst id = doc["event_id"];
  if (id.isNull()) {
    return false;
  }
  event.eventId = id.as<String>();
  event.sequence = doc["_sequence"].as<uint64_t>();
  event.bootId = doc["_boot_id"] | 0UL;
  event.startedMillis = doc["_monotonic_started_ms"] | 0UL;
  event.endedMillis = doc["_monotonic_ended_ms"] | 0UL;
  event.durationSec = doc["duration_sec"] | 0UL;
  event.quality = String(doc["quality"] | "low");
  event.excretionQuality = String(doc["excretion_quality"] | "unavailable");
  event.timeQuality = String(doc["time_quality"] | "unavailable");
  event.terminationReason = String(doc["termination_reason"] | "normal");
  event.queueDroppedCount = doc["queue_dropped_count"] | 0UL;

  JsonVariantConst started = doc["started_at"];
  event.hasStartedAt = !started.isNull();
  if (event.hasStartedAt) event.startedAt = started.as<String>();
  JsonVariantConst ended = doc["ended_at"];
  event.hasEndedAt = !ended.isNull();
  if (event.hasEndedAt) event.endedAt = ended.as<String>();

  JsonVariantConst weight = doc["weight_g"];
  event.hasWeight = !weight.isNull();
  if (event.hasWeight) event.weightG = weight.as<float>();
  JsonVariantConst before = doc["baseline_before_g"];
  event.hasBaselineBefore = !before.isNull();
  if (event.hasBaselineBefore) event.baselineBeforeG = before.as<float>();
  JsonVariantConst after = doc["baseline_after_g"];
  event.hasBaselineAfter = !after.isNull();
  if (event.hasBaselineAfter) event.baselineAfterG = after.as<float>();
  JsonVariantConst excretion = doc["excretion_g"];
  event.hasExcretion = !excretion.isNull();
  if (event.hasExcretion) event.excretionG = excretion.as<float>();
  return true;
}

bool EventStore::writeAtomicLocked(const String& path, EventRecord& event) {
  const String temporaryPath = path + TEMP_SUFFIX;
  JsonDocument doc;
  serializeEvent(doc, event);

  File file = LittleFS.open(temporaryPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  const size_t written = serializeJson(doc, file);
  file.flush();
  file.close();
  if (written == 0 || !validateJsonLocked(temporaryPath)) {
    LittleFS.remove(temporaryPath);
    return false;
  }

  if (LittleFS.exists(path)) {
    LittleFS.remove(path);
  }
  if (!LittleFS.rename(temporaryPath, path)) {
    // Keep the already-validated temporary file. Startup recovery can restore
    // it, whereas deleting it here could lose an accepted event update.
    Serial.print(F("storage=rename_failed temp_preserved path="));
    Serial.println(temporaryPath);
    return false;
  }
  return true;
}

bool EventStore::findOldestLocked(String& path) const {
  File root = LittleFS.open(QUEUE_DIR);
  if (!root || !root.isDirectory()) {
    return false;
  }
  String oldest;
  File file = root.openNextFile();
  while (file) {
    const String candidate = file.path();
    file.close();
    file = root.openNextFile();
    if (!candidate.endsWith(FILE_SUFFIX)) {
      continue;
    }
    if (oldest.length() == 0 || candidate < oldest) {
      oldest = candidate;
    }
  }
  root.close();
  if (oldest.length() == 0) {
    return false;
  }
  path = oldest;
  return true;
}

bool EventStore::removeOldestLocked() {
  String oldest;
  if (!findOldestEvictableLocked(oldest) || !LittleFS.remove(oldest)) {
    return false;
  }
  ++droppedCount_;
  preferences_.putUInt("dropped", droppedCount_);
  Serial.print(F("storage=dropped_oldest path="));
  Serial.print(oldest);
  Serial.print(F(" dropped="));
  Serial.println(droppedCount_);
  return true;
}

bool EventStore::findOldestEvictableLocked(String& path) const {
  const char* directories[] = {QUEUE_DIR, FAILED_DIR};
  String oldestPath;
  String oldestName;
  for (const char* directory : directories) {
    File root = LittleFS.open(directory);
    if (!root || !root.isDirectory()) continue;
    File file = root.openNextFile();
    while (file) {
      const String candidate = file.path();
      file.close();
      file = root.openNextFile();
      const String name = basename(candidate);
      if (!name.startsWith("e-") || name.indexOf(FILE_SUFFIX) < 0 ||
          name.endsWith(TEMP_SUFFIX) || name.endsWith(".corrupt")) {
        continue;
      }
      if (oldestName.length() == 0 || name < oldestName) {
        oldestName = name;
        oldestPath = candidate;
      }
    }
    root.close();
  }
  if (oldestPath.length() == 0) return false;
  path = oldestPath;
  return true;
}

bool EventStore::ensureSpaceLocked(size_t requiredBytes) {
  const size_t total = LittleFS.totalBytes();
  const size_t maxBytes = (total * CatScaleConfig::MAX_QUEUE_PERCENT) / 100;
  while (true) {
    size_t count = 0;
    File root = LittleFS.open(QUEUE_DIR);
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      while (file) {
        const String candidate = file.path();
        file.close();
        file = root.openNextFile();
        if (candidate.endsWith(FILE_SUFFIX)) ++count;
      }
      root.close();
    }
    const size_t used = LittleFS.usedBytes();
    const bool countFull = count >= CatScaleConfig::MAX_QUEUE_EVENTS;
    const bool bytesFull = (maxBytes > 0 && used + requiredBytes > maxBytes);
    if (!countFull && !bytesFull) {
      return true;
    }
    if (!removeOldestLocked()) {
      return false;
    }
  }
}

bool EventStore::save(EventRecord& event) {
  if (!ready_) {
    return false;
  }
  if (!lock()) return false;

  if (event.sequence == 0) {
    event.sequence = nextSequence_++;
    preferences_.putULong64("next_seq", nextSequence_);
  }
  if (event.eventId.length() == 0) {
    char id[96] = {};
    const uint64_t chipId = ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;
    snprintf(id, sizeof(id), "%s-%012llx-%08lx-%020llu", CatScaleConfig::DEVICE_ID,
             static_cast<unsigned long long>(chipId),
             static_cast<unsigned long>(event.bootId),
             static_cast<unsigned long long>(event.sequence));
    event.eventId = id;
  }
  const String path = eventPath(event.sequence);
  JsonDocument preview;
  serializeEvent(preview, event);
  const size_t required = measureJson(preview) + 32;
  if (!ensureSpaceLocked(required)) {
    unlock();
    return false;
  }
  event.queueDroppedCount = droppedCount_;
  const bool result = writeAtomicLocked(path, event);
  unlock();
  if (result) {
    Serial.print(F("event=saved path="));
    Serial.println(path);
  }
  return result;
}

bool EventStore::loadLocked(const String& path, EventRecord& event, String& json) const {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;
  json.reserve(file.size() + 1);
  while (file.available()) json += static_cast<char>(file.read());
  file.close();
  JsonDocument doc;
  if (deserializeJson(doc, json) || !deserializeEvent(doc, event)) return false;
  return true;
}

bool EventStore::nextPending(PendingEvent& pending) {
  if (!ready_ || !lock()) return false;
  String path;
  bool result = false;
  while (findOldestLocked(path)) {
    EventRecord event;
    String json;
    if (loadLocked(path, event, json)) {
      pending.path = path;
      pending.json = json;
      pending.event = event;
      result = true;
      break;
    }
    Serial.print(F("storage=pending_read_failed path="));
    Serial.println(path);
    const String badPath = makeQuarantinePath(path);
    if (!LittleFS.rename(path, badPath)) {
      Serial.println(F("storage=quarantine_failed; pending event retained"));
      break;
    }
    Serial.print(F("storage=quarantined_corrupt path="));
    Serial.println(badPath);
    path = String();
  }
  unlock();
  return result;
}

bool EventStore::update(const String& path, EventRecord& event) {
  if (!ready_ || path.length() == 0 || !lock()) return false;
  const bool result = writeAtomicLocked(path, event);
  unlock();
  return result;
}

bool EventStore::markSent(const String& path) {
  if (!ready_ || path.length() == 0 || !lock()) return false;
  const bool result = LittleFS.remove(path);
  unlock();
  return result;
}

bool EventStore::quarantine(const String& path, const char* reason) {
  if (!ready_ || path.length() == 0 || !lock()) return false;
  const String target = makeQuarantinePath(path);
  const bool result = LittleFS.rename(path, target);
  unlock();
  if (result) {
    Serial.print(F("storage=quarantined reason="));
    Serial.print(reason == nullptr ? "unknown" : reason);
    Serial.print(F(" path="));
    Serial.println(target);
  }
  return result;
}

size_t EventStore::pendingCount() const {
  if (!ready_ && mutex_ == nullptr) return 0;
  if (!lock()) return 0;
  size_t count = 0;
  File root = LittleFS.open(QUEUE_DIR);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      const String candidate = file.path();
      file.close();
      file = root.openNextFile();
      if (candidate.endsWith(FILE_SUFFIX)) ++count;
    }
    root.close();
  }
  unlock();
  return count;
}

size_t EventStore::failedCount() const {
  if (!ready_ && mutex_ == nullptr) return 0;
  if (!lock()) return 0;
  size_t count = 0;
  File root = LittleFS.open(FAILED_DIR);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) ++count;
      file.close();
      file = root.openNextFile();
    }
    root.close();
  }
  unlock();
  return count;
}
