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
  struct NetworkDiagnosticSnapshot {
    uint32_t lastWebhookAttemptAt = 0;
    uint32_t lastWebhookSuccessAt = 0;
    uint32_t consecutiveWebhookFailures = 0;
    uint32_t nextRetryAt = 0;
    uint32_t lastWifiDisconnectedAt = 0;
    uint32_t lastWifiConnectedAt = 0;
    uint32_t lastWifiOutageDurationMs = 0;
    uint32_t wifiReconnectAttemptCount = 0;
    uint32_t lastWifiRecoveryAttemptCount = 0;
    uint32_t wifiRadioResetCount = 0;
    uint8_t lastWebhookAttemptNumber = 0;
    int lastWebhookStatus = 0;
    bool hasWebhookAttempt = false;
    char lastWebhookEventId[96] = {};
    char lastWebhookResult[128] = {};
    char lastWebhookIsolationReason[96] = {};
  };

  static void taskEntry(void* context);
  static void diagnosticTaskEntry(void* context);
  void taskLoop();
  void diagnosticTaskLoop();
  void maintainWifi(uint32_t now);
  void configureWifiStation();
  void startWifiConnection(uint32_t now, bool resetRadio);
  void refreshTime(uint32_t now);
  void processQueue(uint32_t now);
  void handleDiagnosticClient(uint32_t now);
  bool readDiagnosticRequest(WiFiClient& client, char* requestLine, size_t capacity);
  void sendDiagnosticPage(WiFiClient& client, uint32_t now);
  void sendDiagnosticError(WiFiClient& client, int statusCode, const __FlashStringHelper* reason);
  void printHtmlEscaped(WiFiClient& client, const String& value);
  void printDiagnosticAdvice(WiFiClient& client, const MeasurementDiagnosticSnapshot& measurement,
                             bool wifiConnected, bool timeSynced, size_t pendingCount,
                             size_t failedCount, int lastWebhookStatus);
  String diagnosticTimestamp(uint32_t timestampMillis) const;
  const char* resetReasonName() const;
  void publishNetworkDiagnosticSnapshot();
  NetworkDiagnosticSnapshot networkDiagnosticSnapshot() const;
  bool postEvent(const PendingEvent& pending, int& statusCode);
  void resetRetry();
  void scheduleRetry(const String& path, uint32_t now);
  void quarantineWithoutRetry(const PendingEvent& pending, const char* reason,
                              const __FlashStringHelper* resultSuffix);

  EventStore* store_ = nullptr;
  TimeService* timeService_ = nullptr;
  MeasurementEngine* measurement_ = nullptr;
  TaskHandle_t task_ = nullptr;
  TaskHandle_t diagnosticTask_ = nullptr;
  WiFiServer diagnosticServer_{CatScaleConfig::DIAGNOSTIC_HTTP_PORT};
  uint32_t lastWifiAttempt_ = 0;
  uint32_t disconnectedSince_ = 0;
  uint32_t lastWifiRadioReset_ = 0;
  uint32_t lastNtpRefresh_ = 0;
  uint32_t nextRetryAt_ = 0;
  uint32_t lastWebhookAttemptAt_ = 0;
  uint32_t lastWebhookSuccessAt_ = 0;
  uint32_t consecutiveWebhookFailures_ = 0;
  uint8_t activeAttemptCount_ = 0;
  uint8_t lastWebhookAttemptNumber_ = 0;
  String retryPath_;
  String lastWebhookEventId_;
  String lastWebhookResult_;
  String lastWebhookIsolationReason_;
  String quarantineCleanupPath_;
  String quarantineCleanupReason_;
  uint32_t quarantineCleanupNextAt_ = 0;
  int lastWebhookStatus_ = 0;
  bool hasWebhookAttempt_ = false;
  bool wifiOutageActive_ = false;
  bool wifiCredentialsApplied_ = false;
  bool announcedWifi_ = false;
  volatile bool diagnosticServerReady_ = false;
  volatile bool diagnosticServerRestartRequested_ = false;
  String lastAnnouncedIp_;
  uint32_t lastWifiDisconnectedAt_ = 0;
  uint32_t lastWifiConnectedAt_ = 0;
  uint32_t lastWifiOutageDurationMs_ = 0;
  uint32_t wifiReconnectAttemptCount_ = 0;
  uint32_t lastWifiRecoveryAttemptCount_ = 0;
  uint32_t wifiRadioResetCount_ = 0;
  NetworkDiagnosticSnapshot networkDiagnosticSnapshot_;
  mutable portMUX_TYPE networkDiagnosticMux_ = portMUX_INITIALIZER_UNLOCKED;
  esp_reset_reason_t resetReason_ = ESP_RST_UNKNOWN;
};
