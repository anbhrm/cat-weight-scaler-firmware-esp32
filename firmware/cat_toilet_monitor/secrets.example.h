#pragma once

// Copy this file to secrets.h and replace the placeholders locally.
// secrets.h is excluded by .gitignore. Do not paste real credentials here.
namespace CatScaleSecrets {

constexpr bool CONFIGURED = false;

constexpr char WIFI_SSID[] = "REPLACE_WITH_WIFI_SSID";
constexpr char WIFI_PASSWORD[] = "REPLACE_WITH_WIFI_PASSWORD";
constexpr char WEBHOOK_URL[] = "https://example.invalid/webhook";
constexpr char WEBHOOK_BEARER_TOKEN[] = "";
constexpr char DIAGNOSTIC_AP_PASSWORD[] = "REPLACE_WITH_8_TO_63_CHAR_PASSWORD";

// Use the CA certificate that signs the webhook host. Never disable TLS verification.
// This placeholder intentionally prevents a successful TLS connection until
// the certificate is configured for the actual endpoint.
constexpr char WEBHOOK_CA_CERT[] = "";

}  // namespace CatScaleSecrets
