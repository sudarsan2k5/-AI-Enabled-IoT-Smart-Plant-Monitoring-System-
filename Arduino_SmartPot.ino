/**
 * Smart plant monitor (Arduino IDE) — same logic as SmartPot platformio project.
 *
 * ARDUINO IDE SETUP
 * 1) Install "esp32" by Espressif Systems: Board manager URL
 *    https://espressif.github.io/arduino-esp32/package_esp32_index.json
 *    then Tools → Board → esp32 → "ESP32 Dev Module" (or your exact board)
 * 2) Sketch → Include Library → Manage Libraries, install:
 *    - "DHTesp" (beegee-tokyo)
 *    - "ArduinoJson" (Benoit Blanchon) v7.x
 * 3) ModbusMaster (not always in the manager): download ZIP from
 *    https://github.com/4-20mA/ModbusMaster
 *    then Sketch → Include Library → Add .ZIP Library
 *
 * Edit WIFI_SSID, WIFI_PASS, SENSOR_DATA_URL and NPK_* below, then upload.
 */
#include <DHTesp.h>
#include <ModbusMaster.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cmath>
#include <time.h>

// —————— WiFi & backend ——————
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
static const char *SENSOR_DATA_URL = "http://192.168.0.10:8000/sensor-data";

// —————— Pins ——————
static const int PIN_DHT = 4;
static const int PIN_SOIL_ADC = 36;
static const int PIN_LDR = 5;
static const int PIN_RS485_DE_RE = 18;
static const int PIN_UART_RX2 = 16;
static const int PIN_UART_TX2 = 17;

static const uint32_t POST_INTERVAL_MS = 30 * 1000;
static const uint32_t DHT_READ_DELAY_MS = 2000;

static const uint32_t NPK_BAUD = 9600;
static const uint8_t NPK_SLAVE_ID = 1;
static const uint16_t NPK_START_ADDRESS = 0x0000;
static const uint8_t NPK_REGISTER_COUNT = 3;
static const int NPK_USE_HOLDING = 1;

DHTesp dht;
ModbusMaster npk;

void rs485_to_tx() { digitalWrite(PIN_RS485_DE_RE, HIGH); }
void rs485_to_rx() { digitalWrite(PIN_RS485_DE_RE, LOW); }

bool iso8601_utc(char *buf, size_t len) {
  struct tm t;
  if (!getLocalTime(&t, 0))
    return false;
  strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &t);
  return true;
}

void ntp_start() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  int retries = 0;
  while (!getLocalTime(&t) && retries++ < 30) {
    delay(500);
  }
}

int soil_moisture_percent(int raw12) {
  if (raw12 < 0) raw12 = 0;
  if (raw12 > 4095) raw12 = 4095;
  return (int)map(raw12, 0, 4095, 0, 100);
}

int light_value_from_digital(bool lit) { return lit ? 800 : 200; }

bool read_npk(int &n, int &p, int &k) {
  n = p = k = 0;
  uint8_t r;
  if (NPK_USE_HOLDING)
    r = npk.readHoldingRegisters(NPK_START_ADDRESS, NPK_REGISTER_COUNT);
  else
    r = npk.readInputRegisters(NPK_START_ADDRESS, NPK_REGISTER_COUNT);
  if (r != 0) {
    Serial.printf("NPK Modbus err: 0x%02x\n", (unsigned)r);
    return false;
  }
  n = (int)npk.getResponseBuffer(0);
  p = (int)npk.getResponseBuffer(1);
  k = (int)npk.getResponseBuffer(2);
  return true;
}

bool post_sensor_json(float temp, float hum, int soilPct, int lightVal, int n,
                      int p, int k) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected; skip POST");
    return false;
  }
  char ts[32];
  if (!iso8601_utc(ts, sizeof(ts)))
    snprintf(ts, sizeof(ts), "1970-01-01T00:00:00Z");

  JsonDocument doc;
  doc["temperature"] = (int)std::lround(temp);
  doc["humidity"] = (int)std::lround(hum);
  doc["soil_moisture"] = soilPct;
  doc["light"] = lightVal;
  doc["nitrogen"] = n;
  doc["phosphorus"] = p;
  doc["potassium"] = k;
  doc["timestamp"] = ts;

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(SENSOR_DATA_URL)) {
    Serial.println("http.begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  Serial.printf("POST %d %s\n", code, body.c_str());
  http.end();
  return code >= 200 && code < 300;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_RS485_DE_RE, OUTPUT);
  rs485_to_rx();

  dht.setup(PIN_DHT, DHTesp::DHT22);
  delay(2000);
  analogReadResolution(12);
  analogSetPinAttenuation((gpio_num_t)PIN_SOIL_ADC, ADC_11db);

  Serial2.begin(NPK_BAUD, SERIAL_8N1, PIN_UART_RX2, PIN_UART_TX2);
  npk.begin(NPK_SLAVE_ID, Serial2);
  npk.preTransmission(rs485_to_tx);
  npk.postTransmission(rs485_to_rx);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  int w = 0;
  while (WiFi.status() != WL_CONNECTED && w++ < 40) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP ");
    Serial.println(WiFi.localIP());
    ntp_start();
  } else
    Serial.println("WiFi connect failed; will retry in loop (no NTP).");

  Serial.println("Ready. DHT=4, soil=36, LDR=5, UART2=16/17, DE+RE=18");
}

void loop() {
  static uint32_t lastPost = 0;
  if (millis() - lastPost < POST_INTERVAL_MS) {
    delay(200);
    return;
  }
  lastPost = millis();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(2000);
  }

  float temp = NAN, hum = NAN;
  for (int try_dht = 0; try_dht < 2; try_dht++) {
    TempAndHumidity th = dht.getTempAndHumidity();
    if (dht.getStatus() == 0) {
      temp = th.temperature;
      hum = th.humidity;
      break;
    }
    Serial.printf("DHT err (try %d) %d\n", try_dht, (int)dht.getStatus());
    delay(DHT_READ_DELAY_MS);
  }

  int raw = analogRead(PIN_SOIL_ADC);
  int soil = soil_moisture_percent(raw);
  int lightV = light_value_from_digital(digitalRead(PIN_LDR) == HIGH);

  int n = 0, p = 0, k = 0;
  if (!read_npk(n, p, k))
    Serial.println("NPK: using zeros (Modbus read failed).");

  Serial.printf("T=%.1f H=%.1f soil%%=%d raw=%d L=%d N=%d P=%d K=%d\n", temp, hum, soil, raw, lightV, n, p, k);

  if (!isnan(temp) && !isnan(hum))
    post_sensor_json(temp, hum, soil, lightV, n, p, k);
  else
    Serial.println("Skip POST: invalid DHT");
}
