#include "network_manager.h"

#include <esp_system.h>
#include <cstring>

namespace {
void printRowStart(WiFiClient& client, const __FlashStringHelper* label) {
  client.print(F("<tr><th>"));
  client.print(label);
  client.print(F("</th><td>"));
}

void printRowEnd(WiFiClient& client) {
  client.print(F("</td></tr>"));
}
}

void CatScaleNetworkManager::begin(EventStore* store, TimeService* timeService,
                                   MeasurementEngine* measurement) {
  store_ = store;
  timeService_ = timeService;
  measurement_ = measurement;
  resetReason_ = esp_reset_reason();
  xTaskCreate(taskEntry, "cat-net", 8192, this, 1, &task_);
}

void CatScaleNetworkManager::taskEntry(void* context) {
  CatScaleNetworkManager* self = static_cast<CatScaleNetworkManager*>(context);
  if (self != nullptr) self->taskLoop();
  vTaskDelete(nullptr);
}

void CatScaleNetworkManager::taskLoop() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);

  while (true) {
    const uint32_t now = millis();
    maintainDiagnosticAccessPoint(now);
    maintainWifi(now);
    refreshTime(now);
    handleDiagnosticClient(now);
    processQueue(now);
    vTaskDelay(pdMS_TO_TICKS(CatScaleConfig::NETWORK_POLL_INTERVAL_MS));
  }
}

void CatScaleNetworkManager::maintainDiagnosticAccessPoint(uint32_t now) {
  if (diagnosticApReady_) return;
  if (lastDiagnosticApAttempt_ != 0 &&
      static_cast<uint32_t>(now - lastDiagnosticApAttempt_) < 30000UL) {
    return;
  }
  lastDiagnosticApAttempt_ = now;

  const IPAddress ip(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(ip, gateway, subnet)) {
    Serial.println(F("diagnostic_ap=failed reason=ip_configuration"));
    return;
  }
  if (!WiFi.softAP(CatScaleConfig::DIAGNOSTIC_AP_SSID,
                   CatScaleSecrets::DIAGNOSTIC_AP_PASSWORD, 1, false,
                   CatScaleConfig::DIAGNOSTIC_MAX_CLIENTS)) {
    Serial.println(F("diagnostic_ap=failed reason=softap_start"));
    return;
  }
  diagnosticServer_.begin();
  diagnosticApReady_ = true;
  Serial.println(F("diagnostic_ap=ready ip=192.168.4.1 password=hidden"));
}

void CatScaleNetworkManager::maintainWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!announcedWifi_) {
      announcedWifi_ = true;
      Serial.println(F("wifi=connected"));
    }
    return;
  }
  if (announcedWifi_) {
    announcedWifi_ = false;
    Serial.println(F("wifi=disconnected; measurement continues"));
  }
  if (static_cast<uint32_t>(now - lastWifiAttempt_) < CatScaleConfig::WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  lastWifiAttempt_ = now;
  WiFi.disconnect(false, false);
  WiFi.begin(CatScaleSecrets::WIFI_SSID, CatScaleSecrets::WIFI_PASSWORD);
  Serial.println(F("wifi=connecting"));
}

void CatScaleNetworkManager::refreshTime(uint32_t now) {
  if (timeService_ == nullptr) return;
  if (lastNtpRefresh_ != 0 &&
      static_cast<uint32_t>(now - lastNtpRefresh_) < CatScaleConfig::NTP_REFRESH_INTERVAL_MS) {
    return;
  }
  lastNtpRefresh_ = now;
  timeService_->refresh();
  static bool announcedTime = false;
  if (timeService_->isSynced() && !announcedTime) {
    announcedTime = true;
    String currentJst;
    if (timeService_->timestampFor(now, timeService_->bootId(), currentJst)) {
      Serial.print(F("time=ntp_synced timezone=JST now="));
      Serial.println(currentJst);
    } else {
      Serial.println(F("time=ntp_synced timezone=JST now=unavailable"));
    }
  }
}

bool CatScaleNetworkManager::readDiagnosticRequest(WiFiClient& client, char* requestLine,
                                                   size_t capacity) {
  if (requestLine == nullptr || capacity < 2) return false;
  const uint32_t startedAt = millis();
  size_t totalBytes = 0;
  size_t requestLength = 0;
  bool requestLineComplete = false;
  bool currentHeaderHasData = false;

  while (static_cast<uint32_t>(millis() - startedAt) <
         CatScaleConfig::DIAGNOSTIC_REQUEST_TIMEOUT_MS) {
    while (client.available() > 0) {
      const int value = client.read();
      if (value < 0) break;
      const char character = static_cast<char>(value);
      if (++totalBytes > CatScaleConfig::DIAGNOSTIC_REQUEST_MAX_BYTES) return false;

      if (!requestLineComplete) {
        if (character == '\n') {
          requestLine[requestLength] = '\0';
          requestLineComplete = true;
          currentHeaderHasData = false;
        } else if (character != '\r') {
          if (requestLength + 1 >= capacity) return false;
          requestLine[requestLength++] = character;
        }
      } else if (character == '\n') {
        if (!currentHeaderHasData) return true;
        currentHeaderHasData = false;
      } else if (character != '\r') {
        currentHeaderHasData = true;
      }
    }
    if (!client.connected() && client.available() == 0) return false;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return false;
}

void CatScaleNetworkManager::handleDiagnosticClient(uint32_t now) {
  if (!diagnosticApReady_) return;
  WiFiClient client = diagnosticServer_.available();
  if (!client) return;

  char requestLine[128] = {};
  if (!readDiagnosticRequest(client, requestLine, sizeof(requestLine))) {
    sendDiagnosticError(client, 400, F("Bad Request"));
  } else if (strcmp(requestLine, "GET / HTTP/1.1") == 0 ||
             strcmp(requestLine, "GET / HTTP/1.0") == 0) {
    sendDiagnosticPage(client, now);
  } else if (strncmp(requestLine, "GET ", 4) == 0) {
    sendDiagnosticError(client, 404, F("Not Found"));
  } else {
    sendDiagnosticError(client, 405, F("Method Not Allowed"));
  }
  client.stop();
}

void CatScaleNetworkManager::sendDiagnosticError(WiFiClient& client, int statusCode,
                                                 const __FlashStringHelper* reason) {
  client.print(F("HTTP/1.1 "));
  client.print(statusCode);
  client.print(' ');
  client.print(reason);
  client.print(F("\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"));
  client.print(reason);
  client.print('\n');
}

void CatScaleNetworkManager::printHtmlEscaped(WiFiClient& client, const String& value) {
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': client.print(F("&amp;")); break;
      case '<': client.print(F("&lt;")); break;
      case '>': client.print(F("&gt;")); break;
      case '"': client.print(F("&quot;")); break;
      case '\'': client.print(F("&#39;")); break;
      default: client.print(value[i]); break;
    }
  }
}

String CatScaleNetworkManager::diagnosticTimestamp(uint32_t timestampMillis) const {
  if (timestampMillis == 0) return String(F("なし"));
  String timestamp;
  if (timeService_ != nullptr &&
      timeService_->timestampFor(timestampMillis, timeService_->bootId(), timestamp)) {
    return timestamp;
  }
  return String(F("起動後 ")) + String(timestampMillis / 1000UL) + String(F(" 秒"));
}

const char* CatScaleNetworkManager::resetReasonName() const {
  switch (resetReason_) {
    case ESP_RST_POWERON: return "電源投入";
    case ESP_RST_EXT: return "外部リセット";
    case ESP_RST_SW: return "ソフトウェアリセット";
    case ESP_RST_PANIC: return "異常終了（panic）";
    case ESP_RST_INT_WDT: return "割り込みウォッチドッグ";
    case ESP_RST_TASK_WDT: return "タスクウォッチドッグ";
    case ESP_RST_WDT: return "ウォッチドッグ";
    case ESP_RST_DEEPSLEEP: return "Deep Sleep復帰";
    case ESP_RST_BROWNOUT: return "電圧低下";
    case ESP_RST_SDIO: return "SDIOリセット";
    default: return "不明";
  }
}

void CatScaleNetworkManager::printDiagnosticAdvice(
    WiFiClient& client, const MeasurementDiagnosticSnapshot& measurement, bool wifiConnected,
    bool timeSynced, size_t pendingCount, size_t failedCount) {
  client.print(F("<section class=\"card advice\"><h2>次に確認すること</h2><p>"));
  if (store_ == nullptr || !store_->ready()) {
    client.print(F("保存領域を利用できません。USB電源を入れ直し、改善しなければPCでシリアルログを確認してください。"));
  } else if (measurement.state == MonitorState::SENSOR_FAULT) {
    client.print(F("HX711の読み取り異常です。USBを抜いてから、3V3・GND・DOUT・SCKとロードセル配線を確認してください。"));
  } else if (measurement.state == MonitorState::STARTUP_ZERO) {
    client.print(F("起動時風袋の準備中です。猫を載せず、トイレと砂を動かさずに30秒間待ってください。"));
  } else if (measurement.state == MonitorState::TIMEOUT_BLOCKED) {
    client.print(F("長時間荷重が残っています。猫や物を降ろし、200g未満へ戻るまで待ってください。"));
  } else if (measurement.state == MonitorState::MAINTENANCE) {
    client.print(F("メンテナンス中です。掃除後に台を安定させ、物理ボタンを8秒押して終了してください。"));
  } else if (!wifiConnected) {
    client.print(F("自宅Wi-Fiへ接続できていません。ルーターの電源・距離・SSID・パスワードを確認してください。利用記録は本体へ保存されます。"));
  } else if (!timeSynced) {
    client.print(F("時刻同期を待っています。インターネット接続を確認し、数分待ってください。同期まで利用記録は送信されません。"));
  } else if (failedCount > 0) {
    client.print(F("恒久エラーとして隔離された記録があります。WebhookのURL・受信仕様・HTTPステータスを確認してください。"));
  } else if (pendingCount > 0 && lastWebhookStatus_ == HTTPC_ERROR_READ_TIMEOUT) {
    client.print(F("Webhookの応答待ちがタイムアウトしました。記録は保持され、自動的に再送されます。"));
  } else if (pendingCount > 0) {
    client.print(F("未送信記録があります。下の直近Webhook結果と再送予定を確認してください。"));
  } else if (measurement.state == MonitorState::IDLE) {
    client.print(F("正常に待機しています。診断画面は閲覧専用で、通常操作は不要です。"));
  } else {
    client.print(F("トイレ利用または終了処理を計測中です。風袋ボタンを押さず、そのまま完了を待ってください。"));
  }
  client.print(F("</p></section>"));
}

void CatScaleNetworkManager::sendDiagnosticPage(WiFiClient& client, uint32_t now) {
  const MeasurementDiagnosticSnapshot measurement =
      measurement_ != nullptr ? measurement_->diagnosticSnapshot() : MeasurementDiagnosticSnapshot();
  const bool storageReady = store_ != nullptr && store_->ready();
  const size_t pendingCount = storageReady ? store_->pendingCount() : 0;
  const size_t failedCount = storageReady ? store_->failedCount() : 0;
  const uint32_t droppedCount = storageReady ? store_->droppedCount() : 0;
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool timeSynced = timeService_ != nullptr && timeService_->isSynced();

  const bool fault = !storageReady || measurement.state == MonitorState::SENSOR_FAULT ||
                     measurement.state == MonitorState::TIMEOUT_BLOCKED;
  const bool attention = !wifiConnected || !timeSynced || pendingCount > 0 || failedCount > 0;
  const bool detectingUse = measurement.state == MonitorState::ENTRY_CANDIDATE ||
                            measurement.state == MonitorState::OCCUPIED ||
                            measurement.state == MonitorState::EXIT_CANDIDATE ||
                            measurement.state == MonitorState::POST_EXIT;
  const char* overallClass = fault ? "bad" : (attention ? "warn" : "good");
  const char* overallText = "稼働中";
  if (fault) {
    overallText = "異常";
  } else if (measurement.state == MonitorState::STARTUP_ZERO) {
    overallText = "起動中";
  } else if (measurement.state == MonitorState::MAINTENANCE) {
    overallText = "メンテナンス中";
  } else if (detectingUse) {
    overallText = "利用検知中";
  }

  String currentJst;
  const bool hasCurrentJst =
      timeService_ != nullptr && timeService_->timestampFor(now, timeService_->bootId(), currentJst);
  const String configuredSsid(CatScaleSecrets::WIFI_SSID);
  const String stationIp = wifiConnected ? WiFi.localIP().toString() : String(F("-"));

  client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"));
  client.print(F("<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><meta http-equiv=\"refresh\" content=\"3\"><title>猫トイレ診断</title><style>body{margin:0;background:#f3f6f5;color:#172522;font-family:system-ui,sans-serif}main{max-width:760px;margin:auto;padding:16px}.hero,.card{background:#fff;border:1px solid #cbd8d4;border-radius:12px;padding:16px;margin-bottom:12px}.hero.good{border-left:8px solid #16845b}.hero.warn{border-left:8px solid #b26b00}.hero.bad{border-left:8px solid #b42318}h1,h2{margin:.1em 0 .6em}table{width:100%;border-collapse:collapse}th,td{padding:9px;border-bottom:1px solid #dce5e2;text-align:left;vertical-align:top}th{width:42%;color:#344e47}.advice{background:#fff8e8}.note{font-size:.9rem;color:#526963}code{background:#e8efed;color:#163d34;padding:2px 5px;border-radius:4px}</style></head><body><main>"));
  client.print(F("<section class=\"hero "));
  client.print(overallClass);
  client.print(F("\"><h1>"));
  client.print(overallText);
  client.print(F("</h1><p>3秒ごとに自動更新します。診断画面から本体操作はできません。</p></section>"));

  client.print(F("<section class=\"card\"><h2>本体</h2><table>"));
  printRowStart(client, F("ファームウェア")); client.print(CatScaleConfig::FIRMWARE_VERSION); printRowEnd(client);
  printRowStart(client, F("稼働時間")); client.print(now / 1000UL); client.print(F(" 秒")); printRowEnd(client);
  printRowStart(client, F("直前のリセット")); client.print(resetReasonName()); printRowEnd(client);
  printRowStart(client, F("測定状態")); client.print(monitorStateName(measurement.state)); printRowEnd(client);
  printRowStart(client, F("現在重量"));
  if (measurement.hasSample) { client.print(measurement.currentGrams, 1); client.print(F(" g")); } else { client.print(F("取得前")); }
  printRowEnd(client);
  printRowStart(client, F("生値"));
  if (measurement.hasSample) {
    char rawBuffer[32] = {};
    snprintf(rawBuffer, sizeof(rawBuffer), "%lld", measurement.currentRaw);
    client.print(rawBuffer);
  } else { client.print(F("取得前")); }
  printRowEnd(client);
  printRowStart(client, F("最終正常サンプル"));
  if (measurement.lastReadyAt > 0) { client.print((now - measurement.lastReadyAt) / 1000UL); client.print(F(" 秒前")); } else { client.print(F("なし")); }
  printRowEnd(client);
  printRowStart(client, F("起動時風袋"));
  if (measurement.startupTareDone) {
    client.print(F("完了"));
  } else if (measurement.state == MonitorState::STARTUP_ZERO) {
    client.print(F("安定待ち "));
    client.print(min(measurement.startupStableElapsedMs, CatScaleConfig::STARTUP_STABLE_MS) / 1000UL);
    client.print(F(" / 30 秒"));
  } else {
    client.print(F("未完了"));
  }
  printRowEnd(client);
  client.print(F("</table></section>"));

  client.print(F("<section class=\"card\"><h2>通信</h2><table>"));
  printRowStart(client, F("自宅Wi-Fi")); client.print(wifiConnected ? F("接続済み") : F("未接続")); printRowEnd(client);
  printRowStart(client, F("接続先SSID")); printHtmlEscaped(client, configuredSsid); printRowEnd(client);
  printRowStart(client, F("自宅Wi-Fi IP")); printHtmlEscaped(client, stationIp); printRowEnd(client);
  printRowStart(client, F("電波強度"));
  if (wifiConnected) { client.print(WiFi.RSSI()); client.print(F(" dBm")); } else { client.print(F("-")); }
  printRowEnd(client);
  printRowStart(client, F("診断画面IP")); client.print(F("192.168.4.1")); printRowEnd(client);
  printRowStart(client, F("NTP時刻同期")); client.print(timeSynced ? F("同期済み") : F("未同期")); printRowEnd(client);
  printRowStart(client, F("現在JST"));
  if (hasCurrentJst) { printHtmlEscaped(client, currentJst); } else { client.print(F("取得前")); }
  printRowEnd(client);
  client.print(F("</table></section>"));

  client.print(F("<section class=\"card\"><h2>保存とWebhook</h2><table>"));
  printRowStart(client, F("LittleFS")); client.print(storageReady ? F("利用可能") : F("異常")); printRowEnd(client);
  printRowStart(client, F("未送信件数")); client.print(pendingCount); printRowEnd(client);
  printRowStart(client, F("隔離件数")); client.print(failedCount); printRowEnd(client);
  printRowStart(client, F("容量超過による削除")); client.print(droppedCount); printRowEnd(client);
  printRowStart(client, F("直近Webhook試行")); printHtmlEscaped(client, diagnosticTimestamp(lastWebhookAttemptAt_)); printRowEnd(client);
  printRowStart(client, F("直近Webhook結果"));
  if (hasWebhookAttempt_) {
    printHtmlEscaped(client, lastWebhookResult_);
    client.print(F("（status=")); client.print(lastWebhookStatus_); client.print(')');
  } else { client.print(F("まだありません")); }
  printRowEnd(client);
  printRowStart(client, F("連続失敗回数")); client.print(consecutiveWebhookFailures_); printRowEnd(client);
  printRowStart(client, F("直近成功")); printHtmlEscaped(client, diagnosticTimestamp(lastWebhookSuccessAt_)); printRowEnd(client);
  printRowStart(client, F("直近イベントID"));
  if (lastWebhookEventId_.length() > 0) { printHtmlEscaped(client, lastWebhookEventId_); } else { client.print(F("なし")); }
  printRowEnd(client);
  printRowStart(client, F("次回再送"));
  if (nextRetryAt_ != 0 && static_cast<int32_t>(nextRetryAt_ - now) > 0) {
    client.print(static_cast<uint32_t>(nextRetryAt_ - now) / 1000UL); client.print(F(" 秒後"));
  } else { client.print(F("予定なし")); }
  printRowEnd(client);
  client.print(F("</table></section>"));

  printDiagnosticAdvice(client, measurement, wifiConnected, timeSynced, pendingCount, failedCount);
  client.print(F("<p class=\"note\">スマートフォンに「インターネット接続なし」と表示されても、この診断用Wi-Fiへの接続を維持してください。パスワードやWebhook URLはこの画面へ表示しません。</p></main></body></html>"));
}

void CatScaleNetworkManager::processQueue(uint32_t now) {
  if (store_ == nullptr || timeService_ == nullptr || !store_->ready()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  // Sending an event with no trusted clock would produce an ambiguous record.
  // It remains in LittleFS until NTP is available.
  if (!timeService_->isSynced()) return;
  if (nextRetryAt_ != 0 && static_cast<int32_t>(now - nextRetryAt_) < 0) return;

  PendingEvent pending;
  if (!store_->nextPending(pending)) {
    resetRetry();
    return;
  }

  if (pending.path != retryPath_) {
    retryPath_ = pending.path;
    retryExponent_ = 0;
    nextRetryAt_ = 0;
  }

  const bool hadStartedAt = pending.event.hasStartedAt;
  const bool hadEndedAt = pending.event.hasEndedAt;
  const String oldTimeQuality = pending.event.timeQuality;
  timeService_->fillEventTimestamps(pending.event);
  if (hadStartedAt != pending.event.hasStartedAt || hadEndedAt != pending.event.hasEndedAt ||
      oldTimeQuality != pending.event.timeQuality) {
    if (!store_->update(pending.path, pending.event)) {
      Serial.println(F("network=deferred reason=event_update_failed"));
      scheduleRetry(pending.path, now);
      return;
    }
    // Re-read after the atomic update so the POST body includes the timestamps.
    PendingEvent refreshed;
    if (!store_->nextPending(refreshed) || refreshed.path != pending.path) {
      scheduleRetry(pending.path, now);
      return;
    }
    pending = refreshed;
  }

  // The LittleFS envelope also contains private monotonic recovery fields.
  // Rebuild the public v2 body so those storage-only keys are never posted.
  pending.json = store_->webhookJson(pending.event);

  int statusCode = 0;
  const bool posted = postEvent(pending, statusCode);
  if (posted) {
    if (store_->markSent(pending.path)) {
      Serial.print(F("webhook=sent event_id="));
      Serial.println(pending.event.eventId);
      resetRetry();
    } else {
      // The receiver may already have accepted the event. Keeping the file
      // makes the retry idempotent through the event_id header.
      Serial.println(F("webhook=accepted storage=delete_failed; will_retry_idempotently"));
      lastWebhookResult_ = F("HTTP送信成功・送信済みファイルの削除失敗");
      ++consecutiveWebhookFailures_;
      scheduleRetry(pending.path, now);
    }
    return;
  }

  const bool retryable = statusCode <= 0 || statusCode < 200 || statusCode == 408 ||
                         statusCode == 429 || statusCode >= 500;
  if (!retryable) {
    // Redirects are disabled deliberately. Quarantine 3xx and permanent 4xx
    // responses so one bad endpoint cannot hammer the server or block the queue.
    if (store_->quarantine(pending.path, "permanent_http_status")) {
      Serial.print(F("webhook=quarantined status="));
      Serial.println(statusCode);
      lastWebhookResult_ += F("・恒久エラーとして隔離");
      resetRetry();
    } else {
      Serial.println(F("webhook=quarantine_failed; retrying_later"));
      scheduleRetry(pending.path, now);
    }
    return;
  }
  handleFailure(pending.path, statusCode, retryable, now);
}

bool CatScaleNetworkManager::postEvent(const PendingEvent& pending, int& statusCode) {
  statusCode = 0;
  hasWebhookAttempt_ = true;
  lastWebhookAttemptAt_ = millis();
  lastWebhookEventId_ = pending.event.eventId;
  lastWebhookStatus_ = 0;
  if (strlen(CatScaleSecrets::WEBHOOK_URL) == 0 ||
      strlen(CatScaleSecrets::WEBHOOK_CA_CERT) == 0) {
    Serial.println(F("webhook=disabled reason=url_or_ca_missing"));
    lastWebhookResult_ = F("Webhook URLまたはルートCAが未設定");
    ++consecutiveWebhookFailures_;
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(CatScaleSecrets::WEBHOOK_CA_CERT);
  HTTPClient http;
  http.setConnectTimeout(CatScaleConfig::HTTP_TIMEOUT_MS);
  http.setTimeout(CatScaleConfig::HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, CatScaleSecrets::WEBHOOK_URL)) {
    Serial.println(F("webhook=begin_failed"));
    lastWebhookResult_ = F("HTTPクライアント開始失敗");
    ++consecutiveWebhookFailures_;
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Idempotency-Key", pending.event.eventId);
  if (strlen(CatScaleSecrets::WEBHOOK_BEARER_TOKEN) > 0) {
    String authorization = "Bearer ";
    authorization += CatScaleSecrets::WEBHOOK_BEARER_TOKEN;
    http.addHeader("Authorization", authorization);
  }
  statusCode = http.POST(pending.json);
  lastWebhookStatus_ = statusCode;
  const bool success = statusCode >= 200 && statusCode < 300;
  if (success) {
    lastWebhookSuccessAt_ = millis();
    consecutiveWebhookFailures_ = 0;
    lastWebhookResult_ = F("送信成功");
  } else {
    ++consecutiveWebhookFailures_;
    if (statusCode < 0) {
      lastWebhookResult_ = HTTPClient::errorToString(statusCode);
    } else {
      lastWebhookResult_ = F("HTTPエラー");
    }
    Serial.print(F("webhook=failed status="));
    Serial.println(statusCode);
  }
  http.end();
  return success;
}

void CatScaleNetworkManager::handleFailure(const String& path, int statusCode, bool retryable,
                                           uint32_t now) {
  if (!retryable) {
    Serial.print(F("webhook=failed_nonretryable status="));
    Serial.println(statusCode);
    return;
  }
  scheduleRetry(path, now);
}

void CatScaleNetworkManager::scheduleRetry(const String& path, uint32_t now) {
  if (path != retryPath_) {
    retryPath_ = path;
    retryExponent_ = 0;
  }
  static constexpr uint32_t BACKOFF_SECONDS[] = {5, 15, 60, 300, 900, 3600};
  const uint8_t backoffIndex = min(retryExponent_, CatScaleConfig::MAX_EVENT_RETRY_EXPONENT);
  const uint32_t baseSeconds = BACKOFF_SECONDS[backoffIndex];
  const uint32_t baseMs = min(baseSeconds * 1000UL, CatScaleConfig::MAX_BACKOFF_MS);
  // Add deterministic-enough ±20% jitter without requiring another RNG API.
  const uint32_t jitterSpan = max(1UL, baseMs / 5UL);
  const int32_t jitter = static_cast<int32_t>(esp_random() % (jitterSpan * 2UL + 1UL)) -
                         static_cast<int32_t>(jitterSpan);
  nextRetryAt_ = now + baseMs + jitter;
  if (retryExponent_ < CatScaleConfig::MAX_EVENT_RETRY_EXPONENT) ++retryExponent_;
  Serial.print(F("webhook=retry_scheduled delay_ms="));
  Serial.println(static_cast<int32_t>(nextRetryAt_ - now));
}

void CatScaleNetworkManager::resetRetry() {
  retryPath_ = String();
  retryExponent_ = 0;
  nextRetryAt_ = 0;
}
