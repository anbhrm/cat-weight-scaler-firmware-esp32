/*
 * 猫トイレ計量台・実運用ファームウェア
 *
 * 構成: XIAO ESP32C3 + HX711 1個 + 20kgフルブリッジロードセル4個並列
 * 配線: HX711 DOUT -> GPIO3/D1, SCK -> GPIO4/D2
 *       NOボタン -> GPIO21/D6 と GND
 *
 * Arduino IDEでこのフォルダーをスケッチとして開き、secrets.example.hを
 * secrets.hへコピーしてローカル設定を入力してから書き込んでください。
 * secrets.hはGit管理対象外です。
 */

#include "app.h"

CatToiletApplication application;

void setup() {
  application.begin();
}

void loop() {
  application.loop();
}
