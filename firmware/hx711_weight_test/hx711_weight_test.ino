/*
 * 猫トイレ計量台：組み立て後の重量確認用
 *
 * 対象：Seeed Studio XIAO ESP32C3 + HX711 1個 + 20kgロードセル4個並列
 * 配線：HX711 DOUT -> XIAO GPIO3 (D1)
 *       HX711 SCK  -> XIAO GPIO4 (D2)
 *       スイッチ -> XIAO GPIO21 (D6) と GND
 *
 * 使い方の概要：
 * 1. USBを接続し、シリアルモニターを115200bpsで開く。
 * 2. 猫や既知重量を載せず、台・トイレ・砂を通常の風袋状態にする。
 *    起動時にその状態を自動で風袋引きする。
 * 3. 校正済みの値があれば weight_g にグラム値が表示される。
 * 4. 初回は、3〜5kg程度の既知重量を載せてシリアルモニターへ c、
 *    続けて重量をグラムで入力する（例：5000）。係数はXIAOへ保存される。
 * 5. スイッチを押すと、その時点のトイレ・砂・台を0gとして風袋引きする。
 *
 * bogde/HX711ライブラリをArduino IDEのライブラリマネージャーから導入する。
 * このスケッチは本番用の猫判別・Webhook送信ではなく、機械組立と計量の確認用。
 */

#include <Arduino.h>
#include <HX711.h>
#include <Preferences.h>
#include <math.h>

namespace {
// XIAO ESP32C3のGPIO番号。基板上の表示ではD1、D2、D6に対応する。
constexpr uint8_t PIN_HX711_DOUT = 3;   // D1 / GPIO3
constexpr uint8_t PIN_HX711_SCK  = 4;   // D2 / GPIO4
constexpr uint8_t PIN_TARE       = 21;  // D6 / GPIO21（スイッチのもう一方はGND）

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t LOG_INTERVAL_MS = 500;
constexpr uint8_t LOG_SAMPLES = 3;
constexpr uint8_t TARE_SAMPLES = 15;
constexpr uint8_t CALIBRATION_SAMPLES = 20;
constexpr float DEFAULT_FACTOR = 1.0f;
constexpr float MIN_FACTOR_ABS = 0.001f;
constexpr float MAX_FACTOR_ABS = 1000000.0f;

HX711 scale;
Preferences preferences;

float calibrationFactor = DEFAULT_FACTOR;
bool calibrated = false;
bool waitingForKnownWeight = false;
bool rawButtonState = false;
bool stableButtonState = false;
uint32_t buttonChangedAt = 0;
uint32_t lastLogAt = 0;
String serialLine;

bool validFactor(float value) {
  return isfinite(value) && fabsf(value) >= MIN_FACTOR_ABS && fabsf(value) <= MAX_FACTOR_ABS;
}

void printHelp() {
  Serial.println();
  Serial.println(F("--- 操作 ---"));
  Serial.println(F("スイッチ押下：風袋引き（台・トイレ・砂を0gにする）"));
  Serial.println(F("t + Enter  ：シリアルから風袋引き"));
  Serial.println(F("c + Enter  ：既知重量を使った校正を開始"));
  Serial.println(F("r + Enter  ：保存済み校正値を消去"));
  Serial.println(F("h + Enter  ：このヘルプを表示"));
  Serial.println(F("校正時は、既知重量を載せたまま c → 重量[g] の順に入力する。例：5000"));
  Serial.println();
}

void loadCalibration() {
  preferences.begin("cat-scale-test", false);
  if (preferences.isKey("factor")) {
    const float saved = preferences.getFloat("factor", DEFAULT_FACTOR);
    if (validFactor(saved)) {
      calibrationFactor = saved;
      calibrated = true;
    }
  }
}

void saveCalibration() {
  preferences.putFloat("factor", calibrationFactor);
}

void tareScale(const __FlashStringHelper* reason) {
  if (!scale.is_ready()) {
    Serial.println(F("event=tare_failed reason=HX711_not_ready"));
    return;
  }

  Serial.print(F("event=tare_start reason="));
  Serial.println(reason);
  Serial.println(F("  風袋引き中。台へ触れず、猫や既知重量を載せないでください。"));
  scale.tare(TARE_SAMPLES);
  Serial.print(F("event=tare_done offset="));
  Serial.println(scale.get_offset());
}

void startCalibration() {
  if (!scale.is_ready()) {
    Serial.println(F("校正開始不可：HX711 not ready"));
    return;
  }
  waitingForKnownWeight = true;
  Serial.println(F("校正：3〜5kg程度の重さを中央へ静かに載せてください。"));
  Serial.println(F("重さを載せたまま、既知重量をグラムで入力してEnterを押してください。例：5000"));
}

void finishCalibration(const String& text) {
  const float knownWeightG = text.toFloat();
  if (!isfinite(knownWeightG) || knownWeightG <= 0.0f) {
    Serial.println(F("校正失敗：0より大きい重量[g]を入力してください。例：5000"));
    waitingForKnownWeight = false;
    return;
  }

  const double rawDelta = scale.get_value(CALIBRATION_SAMPLES);
  if (!isfinite(rawDelta) || fabs(rawDelta) < 1.0) {
    Serial.println(F("校正失敗：生値の変化が小さすぎます。ロードセル配線と既知重量を確認してください。"));
    waitingForKnownWeight = false;
    return;
  }

  const float newFactor = static_cast<float>(rawDelta / knownWeightG);
  if (!validFactor(newFactor)) {
    Serial.println(F("校正失敗：計算された係数が範囲外です。"));
    waitingForKnownWeight = false;
    return;
  }

  calibrationFactor = newFactor;
  calibrated = true;
  saveCalibration();
  waitingForKnownWeight = false;

  Serial.print(F("event=calibration_done known_weight_g="));
  Serial.print(knownWeightG, 1);
  Serial.print(F(" raw_delta="));
  Serial.print(rawDelta, 1);
  Serial.print(F(" factor="));
  Serial.println(calibrationFactor, 6);
  Serial.println(F("校正値を保存しました。以後のweight_gがグラム値になります。"));
}

void resetCalibration() {
  preferences.remove("factor");
  calibrationFactor = DEFAULT_FACTOR;
  calibrated = false;
  Serial.println(F("校正値を消去しました。weight_gは未校正へ戻ります。"));
}

void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }

  if (waitingForKnownWeight) {
    finishCalibration(line);
    return;
  }

  char command = line.charAt(0);
  if (command >= 'A' && command <= 'Z') {
    command = static_cast<char>(command - 'A' + 'a');
  }
  if (command == 't') {
    tareScale(F("serial"));
  } else if (command == 'c') {
    startCalibration();
  } else if (command == 'r') {
    resetCalibration();
  } else if (command == 'h' || command == '?') {
    printHelp();
  } else {
    Serial.println(F("不明な入力です。h + Enterで操作方法を表示します。"));
  }
}

void readSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        handleSerialLine(serialLine);
        serialLine = "";
      }
    } else if (serialLine.length() < 64) {
      serialLine += c;
    }
  }
}

void updateTareButton() {
  const bool reading = (digitalRead(PIN_TARE) == LOW);
  if (reading != rawButtonState) {
    rawButtonState = reading;
    buttonChangedAt = millis();
  }

  if ((millis() - buttonChangedAt) >= 35 && stableButtonState != rawButtonState) {
    stableButtonState = rawButtonState;
    if (stableButtonState) {
      tareScale(F("button"));
    }
  }
}

void logWeight() {
  if (!scale.is_ready()) {
    Serial.println(F("weight_g=NA raw_delta=NA calibrated=0 ready=0"));
    return;
  }

  const double rawDelta = scale.get_value(LOG_SAMPLES);
  Serial.print(F("weight_g="));
  if (calibrated && validFactor(calibrationFactor)) {
    const double weightG = rawDelta / calibrationFactor;
    Serial.print(weightG, 1);
  } else {
    Serial.print(F("NA"));
  }
  Serial.print(F(" raw_delta="));
  Serial.print(rawDelta, 1);
  Serial.print(F(" factor="));
  Serial.print(calibrationFactor, 6);
  Serial.print(F(" calibrated="));
  Serial.println(calibrated ? 1 : 0);
}
}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(800);
  pinMode(PIN_TARE, INPUT_PULLUP);

  scale.begin(PIN_HX711_DOUT, PIN_HX711_SCK);
  loadCalibration();
  scale.set_scale(calibrationFactor);

  Serial.println();
  Serial.println(F("=== HX711 重量テスト ==="));
  Serial.println(F("USB接続：XIAOの電源＋シリアル通信"));
  Serial.println(F("ピン：DOUT=GPIO3(D1), SCK=GPIO4(D2), TARE=GPIO21(D6)"));
  if (calibrated) {
    Serial.print(F("保存済み校正係数 factor="));
    Serial.println(calibrationFactor, 6);
  } else {
    Serial.println(F("校正係数なし。weight_g=NAです。既知重量を使い c で校正してください。"));
  }

  if (scale.is_ready()) {
    tareScale(F("startup"));
  } else {
    Serial.println(F("HX711 not ready：3V3、GND、DOUT、SCKとロードセル配線を確認してください。"));
  }
  printHelp();
}

void loop() {
  readSerialCommands();
  updateTareButton();

  const uint32_t now = millis();
  if (now - lastLogAt >= LOG_INTERVAL_MS) {
    lastLogAt = now;
    logWeight();
  }
}
