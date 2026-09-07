# Cat Weight Scaler Firmware for ESP32

猫用トイレ計量台のための、Seeed Studio XIAO ESP32C3向けファームウェアです。HX711へ並列接続した4個のロードセルから重量を読み、猫の利用開始・退出、体重、利用前後の重量差を検出してWebhookへ送信します。

このリポジトリにはファームウェアだけを収録しています。筐体、計量台、配線加工などの設計資料は含みません。

## 主な機能

- 約10 Hzの重量測定、外れ値除去、5点移動中央値
- 重量変化による利用開始・退出の状態管理
- 安定区間からの猫体重と利用前後差（排泄量）の算出
- イベントをLittleFSへ先に保存し、別タスクからHTTPSで送信
- `event_id` と `Idempotency-Key` による重複送信対策
- NTP同期後のJST（`+09:00`）時刻付与
- 自宅LANから開ける読み取り専用の診断画面
- 長押しボタンによる手動風袋引きとメンテナンスモード

## ディレクトリ構成

| パス | 内容 |
| --- | --- |
| `firmware/cat_toilet_monitor/` | 実運用ファームウェア |
| `firmware/cat_toilet_monitor/secrets.example.h` | 秘密設定のテンプレート |
| `firmware/hx711_weight_test/` | 配線確認、風袋引き、既知重量による校正用スケッチ |

## 対象ハードウェア

- Seeed Studio XIAO ESP32C3
- HX711 1個（Aチャンネルを使用）
- 4線式フルブリッジロードセル4個（同名端子を並列接続）
- LEDなし・Normally Openのモーメンタリーボタン
- USB常時給電

XIAOとHX711の接続は次の設定です。

| 信号 | XIAO ESP32C3 | 備考 |
| --- | --- | --- |
| HX711 VCC | 3V3 | HX711も3.3 Vで動作させる |
| HX711 GND | GND | 共通GND |
| HX711 DOUT | D1 / GPIO3 | `PIN_HX711_DOUT` |
| HX711 SCK | D2 / GPIO4 | `PIN_HX711_SCK` |
| ボタン | D6 / GPIO21 と GND | 内蔵プルアップを使用 |

ロードセルの線色には統一規格がありません。手元の製品資料で `E+`、`E-`、`A+`、`A-` を確認してから接続してください。4個を並列化する前に、1個ずつ符号とゼロ復帰を確認することを推奨します。

## 開発環境

Arduino IDEを基準にしています。確認済みの組み合わせは次のとおりです。

- ESP32 Arduino core 3.3.11
- ボード: `XIAO_ESP32C3`
- [bogde/HX711](https://github.com/bogde/HX711) 0.7.5
- [ArduinoJson](https://arduinojson.org/) 7系（7.4.3で確認）

Wi-Fi、HTTPS、Preferences、LittleFS、FreeRTOS関連の機能はESP32 Arduino coreに含まれます。

## 初期設定

1. Arduino IDEのボードマネージャーでESP32 Arduino core 3.3.11を導入します。
2. ライブラリマネージャーで `HX711 Arduino Library` と `ArduinoJson` を導入します。
3. `firmware/cat_toilet_monitor/secrets.example.h` を同じフォルダーへ複製し、ファイル名を `secrets.h` にします。
4. `secrets.h` のプレースホルダーを実際の値へ置き換え、`CONFIGURED` を `true` にします。

設定項目は次のとおりです。

```cpp
namespace CatScaleSecrets {

constexpr bool CONFIGURED = true;
constexpr char WIFI_SSID[] = "your-2.4GHz-wifi";
constexpr char WIFI_PASSWORD[] = "your-wifi-password";
constexpr char WEBHOOK_URL[] = "https://example.com/webhook";
constexpr char WEBHOOK_BEARER_TOKEN[] = "optional-token";
constexpr char WEBHOOK_CA_CERT[] = R"EOF(-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)EOF";

}
```

`WEBHOOK_CA_CERT` にはWebhookホストの証明書を検証できるルートCA証明書をPEM形式で設定します。TLS検証を無効にする実装にはしていません。

`secrets.h` は `.gitignore` で除外されています。Wi-Fiパスワード、Bearer token、Webhook URL、証明書の秘密鍵などをコミットしないでください。テンプレートの `secrets.example.h` へ実値を書き込むことも避けてください。

## 校正

実運用前に、使用するロードセル、HX711、台を組み上げた状態で校正してください。

1. Arduino IDEで `firmware/hx711_weight_test/hx711_weight_test.ino` を開きます。
2. ボードに `XIAO_ESP32C3`、通信速度に115200 bpsを選び、書き込みます。
3. 台を空荷重の通常状態にして起動時の風袋引きを待ちます。
4. 3〜5 kg程度の既知重量を中央へ載せます。
5. シリアルモニターで `c` を送信し、続けて既知重量をグラム単位で送信します（例: `5000`）。
6. 表示された `factor` を `firmware/cat_toilet_monitor/config.h` の `CALIBRATION_FACTOR` へ反映します。
7. 同じ重量を中央と四隅へ置き、位置による差と再現性を確認します。

重量確認用スケッチがPreferencesへ保存する校正値は、実運用ファームウェアへ自動では引き継がれません。`CALIBRATION_FACTOR` の更新が必要です。

シリアルコマンドは `c`（校正）、`t`（風袋引き）、`r`（保存済み校正値の消去）、`h`（ヘルプ）です。未校正時に `weight_g=NA` と表示されるのは正常です。

## 実運用ファームウェアの書き込み

1. `secrets.h` と `CALIBRATION_FACTOR` を設定します。
2. Arduino IDEで `firmware/cat_toilet_monitor/cat_toilet_monitor.ino` を開きます。
3. ボードに `XIAO_ESP32C3`、接続したポートを選びます。
4. 検証後、XIAO ESP32C3へ書き込みます。
5. 115200 bpsのシリアルモニターで起動状態を確認します。

起動後は、トイレと通常量の砂を載せ、猫や追加物を載せずに30秒間静止させます。安定すると現在の状態を0 gとして保存し、利用検知を開始します。猫や物を載せたまま30秒以上安定させると、その荷重も0 gへ含まれるため、降ろして再起動してください。

機器ごとに変更する値は `firmware/cat_toilet_monitor/config.h` に集約されています。特に次を確認してください。

- `DEVICE_ID`: Webhook上で一意になるデバイスID
- `CALIBRATION_FACTOR`: 実機で求めた校正係数
- 入退場、安定判定、タイムアウトの各閾値
- `EXCRETION_GOOD_ENABLED`: 20 g追加試験と位置誤差試験に合格した後だけ `true`

## ボタン操作

| 操作 | 動作 |
| --- | --- |
| 3秒以上8秒未満押し、離す | IDLE中または起動時に手動風袋引き |
| 8秒押し続ける | メンテナンスモードを開始 |
| メンテナンス中に8秒押す | メンテナンスを終了し、安定後に風袋引き |

利用中や退出処理中の手動風袋操作は、記録を壊さないため無視されます。

## 診断画面

実運用ファームウェアは、自宅Wi-Fiから割り当てられたIPアドレスで診断画面を提供します。独自の診断用アクセスポイントは起動しません。

1. 書き込み後、115200 bpsのシリアルモニターを開きます。
2. `diagnostic=ready url=http://.../` と表示されたURLを確認します。
3. スマートフォンやPCをXIAOと同じ家庭内LANへ接続します。
4. 表示されたURLをブラウザーで開きます。

DHCPホスト名は `cat-toilet-toilet-1` です。IPアドレスはルーターによって変わることがあり、変更時は新しいURLがシリアルへ表示されます。ゲストWi-Fiや端末間通信を禁止するAP隔離が有効なネットワークでは開けません。Wi-Fi切断中も測定とLittleFSへの保存は続きますが、診断画面は復旧まで利用できません。

画面は読み取り専用で、Wi-Fiパスワード、Bearer token、Webhook URLは表示しません。Webhookが応答待ちの間も、診断画面は別タスクで応答します。

## Webhook

HTTP 2xxを送信成功として扱います。408、429、5xxを含むすべての非2xx応答は再送せず、LittleFS内の隔離領域へ移します。タイムアウト、TLS失敗、切断などHTTPステータスを受け取れなかった通信障害だけを、約5秒、15秒、1分の間隔で最大3回再送します。初回を含むHTTPリクエスト数は最大4回です。

接続確立のタイムアウトは5秒、HTTP応答待ちはCloud Functionsのコールドスタートを考慮して30秒です。2xx受信後にローカルファイルの削除が失敗した場合も、同じイベントをHTTPで再送せず送信対象外へ隔離します。Wi-Fi未接続またはNTP同期前は送信を開始せず、イベントを端末内へ保持します。

送信例:

```json
{
  "schema_version": 2,
  "device_id": "toilet-1",
  "event_id": "toilet-1-a1b2c3-0012-00000042",
  "started_at": "2026-09-06T12:34:56+09:00",
  "ended_at": "2026-09-06T12:35:48+09:00",
  "duration_sec": 52,
  "weight_g": 4280,
  "quality": "good",
  "baseline_before_g": 0,
  "baseline_after_g": 42,
  "excretion_g": 42,
  "excretion_quality": "good",
  "time_quality": "synced",
  "termination_reason": "normal",
  "queue_dropped_count": 0,
  "firmware_version": "0.3.0"
}
```

時刻や測定値を確定できない場合、対応する値は `null` になります。受信側は `event_id` に一意制約を設け、同じイベントの再送を安全に受け入れられるようにしてください。猫の識別はこのファームウェアでは行わず、Webhook受信側で `weight_g` を使って判定します。

## トラブルシューティング

- `Copy secrets.example.h to secrets.h...` でコンパイルが止まる: `secrets.h` を作成し、設定後に `CONFIGURED = true` へ変更します。
- `HX711 not ready` または `SENSOR_FAULT`: 3V3、GND、DOUT、SCKとロードセル側の4端子を確認します。
- `webhook=disabled reason=url_or_ca_missing`: `WEBHOOK_URL` と `WEBHOOK_CA_CERT` を設定します。
- 診断URLが表示されない: 自宅Wi-FiのSSIDとパスワード、2.4 GHzの電波状態を確認します。
- 診断URLを開けない: スマートフォンやPCが同じ家庭内LANにあり、ゲストWi-FiやAP隔離を使用していないことを確認します。
- USBポートが表示されない: データ通信対応USBケーブル、USBポート、XIAOのブート操作を確認します。
- 重量の符号が逆、または位置で大きく変わる: ロードセルを1個ずつ確認し、4個の向き、同名端子の並列接続、受圧位置を見直します。

## 注意事項

本ファームウェアと自作計量台は、医療機器や取引用のはかりではありません。健康管理では単発の測定値だけで判断せず、継続的な傾向と猫の状態を合わせて確認してください。
