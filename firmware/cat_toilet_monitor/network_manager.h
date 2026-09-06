#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "event_store.h"
#include "measurement_engine.h"
#include "time_service.h"

class CatScaleNetworkManager {
 public:
  void begin(EventStore* store, TimeService* timeService, MeasurementEngine* measurement);

 private:
  static void taskEntry(void* context);
  void taskLoop();
  void maintainWifi(uint32_t now);
  void refreshTime(uint32_t now);
  void processQueue(uint32_t now);
  void maintainDiagnosticAccessPoint(uint32_t now);
  void handleDiagnosticClient(uint32_t now);
  bool readDiagnosticRequest(WiFiClient& client, char* requestLine, size_t capacity);
  void sendDiagnosticPage(WiFiClient& client, uint32_t now);
  void sendDiagnosticError(WiFiClient& client, int statusCode, const __FlashStringHelper* reason);
  void printHtmlEscaped(WiFiClient& client, const String& value);
  void printDiagnosticAdvice(WiFiClient& client, const MeasurementDiagnosticSnapshot& measurement,
                             bool wifiConnected, bool timeSynced, size_t pendingCount,
                             size_t failedCount);
  String diagnosticTimestamp(uint32_t timestampMillis) const;
  const char* resetReasonName() const;
  bool postEvent(const PendingEvent& pending, int& statusCode);
  void handleFailure(const String& path, int statusCode, bool retryable, uint32_t now);
  void resetRetry();
  void scheduleRetry(const String& path, uint32_t now);

  EventStore* store_ = nullptr;
  TimeService* timeService_ = nullptr;
  MeasurementEngine* measurement_ = nullptr;
  TaskHandle_t task_ = nullptr;
  WiFiServer diagnosticServer_{CatScaleConfig::DIAGNOSTIC_HTTP_PORT};
  uint32_t lastWifiAttempt_ = 0;
  uint32_t lastNtpRefresh_ = 0;
  uint32_t lastDiagnosticApAttempt_ = 0;
  uint32_t nextRetryAt_ = 0;
  uint32_t lastWebhookAttemptAt_ = 0;
  uint32_t lastWebhookSuccessAt_ = 0;
  uint32_t consecutiveWebhookFailures_ = 0;
  uint8_t retryExponent_ = 0;
  String retryPath_;
  String lastWebhookEventId_;
  String lastWebhookResult_;
  int lastWebhookStatus_ = 0;
  bool hasWebhookAttempt_ = false;
  bool diagnosticApReady_ = false;
  bool announcedWifi_ = false;
  esp_reset_reason_t resetReason_ = ESP_RST_UNKNOWN;
};
