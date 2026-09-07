#pragma once

#include <Arduino.h>
#include "secrets.h"

namespace CatScaleConfig {

static_assert(CatScaleSecrets::CONFIGURED,
              "Copy secrets.example.h to secrets.h and configure it before building.");

constexpr char FIRMWARE_VERSION[] = "0.3.1";
constexpr char DEVICE_ID[] = "toilet-1";
constexpr char WIFI_HOSTNAME[] = "cat-toilet-toilet-1";

// XIAO ESP32C3: D1/GPIO3, D2/GPIO4, D6/GPIO21.
constexpr uint8_t PIN_HX711_DOUT = 3;
constexpr uint8_t PIN_HX711_SCK = 4;
constexpr uint8_t PIN_MAINTENANCE_BUTTON = 21;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t LOG_INTERVAL_MS = 2000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;

// The calibration factor was measured with the completed platform.
constexpr float CALIBRATION_FACTOR = 25.658228f;

constexpr float ENTRY_THRESHOLD_G = 1000.0f;
constexpr uint32_t ENTRY_CONFIRM_MS = 10000;
constexpr float EXIT_THRESHOLD_G = 200.0f;
constexpr uint32_t EXIT_CONFIRM_MS = 5000;
constexpr uint32_t POST_EXIT_SETTLE_MS = 10000;
constexpr uint32_t EVENT_TIMEOUT_MS = 30UL * 60UL * 1000UL;

constexpr uint32_t STARTUP_STABLE_MS = 30000;
constexpr uint32_t WARMUP_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t AUTO_TARE_WINDOW_MS = 5UL * 60UL * 1000UL;
constexpr float AUTO_TARE_BAND_G = 20.0f;       // central 95% width
constexpr float IDLE_AUTO_TARE_MAX_OFFSET_G = 200.0f;

constexpr uint32_t FILTER_WINDOW_SAMPLES = 5;
constexpr uint32_t STABLE_WINDOW_SAMPLES = 20;  // 2 seconds at 10 Hz
constexpr float STABLE_WINDOW_RANGE_G = 80.0f;
constexpr uint32_t TARE_SAMPLES = 15;

constexpr float EXCRETION_MIN_G = 10.0f;
constexpr float EXCRETION_GOOD_MIN_G = 20.0f;
constexpr float EXCRETION_MAX_G = 300.0f;
constexpr float EXCRETION_STABLE_RANGE_G = 20.0f;
// Change to true only after the documented 20g central/corner acceptance test passes.
constexpr bool EXCRETION_GOOD_ENABLED = false;

constexpr uint32_t MANUAL_TARE_MIN_MS = 3000;
constexpr uint32_t MAINTENANCE_LONG_PRESS_MS = 8000;

constexpr uint32_t NTP_REFRESH_INTERVAL_MS = 10000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t WIFI_RADIO_RESET_AFTER_MS = 60000;
constexpr uint32_t WIFI_RADIO_RESET_INTERVAL_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t NETWORK_POLL_INTERVAL_MS = 500;
constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t HTTP_RESPONSE_TIMEOUT_MS = 30000;
constexpr uint16_t DIAGNOSTIC_HTTP_PORT = 80;
constexpr uint32_t DIAGNOSTIC_POLL_INTERVAL_MS = 25;
constexpr uint32_t DIAGNOSTIC_REQUEST_TIMEOUT_MS = 500;
constexpr size_t DIAGNOSTIC_REQUEST_MAX_BYTES = 1024;
constexpr uint8_t MAX_WEBHOOK_RETRIES = 3;
constexpr uint8_t MAX_WEBHOOK_ATTEMPTS = 1 + MAX_WEBHOOK_RETRIES;

constexpr size_t MAX_QUEUE_EVENTS = 500;
constexpr uint8_t MAX_QUEUE_PERCENT = 70;

constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.nist.gov";

}  // namespace CatScaleConfig
