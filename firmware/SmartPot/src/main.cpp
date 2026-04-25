/**
 * Smart plant monitor — matches project.md JSON over POST /sensor-data
 * (Same firmware for Arduino IDE: firmware/Arduino_SmartPot/Arduino_SmartPot.ino)
 *
 * Pin map (your wiring):
 *   DHT22   Data     -> D4  (GPIO 4)
 *   YL-69   AO       -> VP  (GPIO 36)
 *   LDR     DO       -> D5  (GPIO 5)   (digital: bright vs dark)
 *   MAX485  RO, DI   -> RX2, TX2 (16, 17)   + RE/DE -> D18 (18)
 *   NPK     A/B      -> blue/yellow
 *
 * NPK: Modbus RTU. Register addresses, baud, slave ID MUST match the sensor
 * manual (or seller PDF). Defaults below are common guesses — change if no reply.
 */
#include <Arduino.h>
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
// Host PC on same LAN, FastAPI, e.g. http://192.168.0.10:8000
static const char *SENSOR_DATA_URL = "http://192.168.0.10:8000/sensor-data";

// —————— Pins ——————
static const int PIN_DHT = 4;
static const int PIN_SOIL_ADC = 36; // VP
static const int PIN_LDR = 5;
static const int PIN_RS485_DE_RE = 18;
static const int PIN_UART_RX2 = 16; // to MAX485 RO
static const int PIN_UART_TX2 = 17; // to MAX485 DI

// —————— Timing ——————
static const uint32_t POST_INTERVAL_MS = 30 * 1000; // how often to upload
static const uint32_t DHT_READ_DELAY_MS = 2000;       // DHT22 needs time between reads

// —————— Modbus (NPK) — edit from datasheet ——————
static const uint32_t NPK_BAUD = 9600; // try 4800 if no response
static const uint8_t NPK_SLAVE_ID = 1;
// Many probes expose N, P, K as three consecutive 16-bit registers.
static const uint16_t NPK_START_ADDRESS = 0x0000;
static const uint8_t NPK_REGISTER_COUNT = 3;
// 1 = use readHoldingRegisters (0x03). 0 = readInputRegisters (0x04)
static const int NPK_USE_HOLDING = 1;

DHTesp dht;
ModbusMaster npk;

// ESP32: UART "flush" can return before the last bit is on the wire; a short
// delay before DE/RE=RX gives the slave a chance to see a clean bus (fixes many 0xE2)
void rs485_to_tx() {
  digitalWrite(PIN_RS485_DE_RE, HIGH);
  delayMicroseconds(50);
}
void rs485_to_rx() {
  delayMicroseconds(1200);
  digitalWrite(PIN_RS485_DE_RE, LOW);
}

// ISO8601 UTC, e.g. 2026-04-22T10:00:00Z
bool iso8601_utc(char *buf, size_t len) {
  struct tm t;
  if (!getLocalTime(&t, 0))
    return false;
  // WiFi: after sync, treat as UTC for simplicity (college / demo)
  strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &t);
  return true;
}

void ntp_start() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  for (int i = 0; i < 30; i++) {
    if (getLocalTime(&t))
      return;
    delay(500);
  }
  Serial.println("NTP: time not set — timestamp will be 1970 until sync works (WiFi/UDP allowed?)");
}

// Map 12-bit ADC to 0–100 (dry = higher raw on YL-69 often — invert in UI if needed)
int soil_moisture_percent(int raw12) {
  if (raw12 < 0) raw12 = 0;
  if (raw12 > 4095) raw12 = 4095;
  return (int)map(raw12, 0, 4095, 0, 100);
}

// 3‑pin LDR board: use a number for JSON (your example used 300)
int light_value_from_digital(bool lit) { return lit ? 800 : 200; }

bool read_npk(int &n, int &p, int &k) {
  n = p = k = 0;
  uint8_t r;
  if (NPK_USE_HOLDING)
    r = npk.readHoldingRegisters(NPK_START_ADDRESS, NPK_REGISTER_COUNT);
  else
    r = npk.readInputRegisters(NPK_START_ADDRESS, NPK_REGISTER_COUNT);

  // success code is 0x00 in ModbusMaster
  if (r != 0) {
    if (r == ModbusMaster::ku8MBResponseTimedOut) {
      Serial.println("NPK Modbus err: 0xE2 = response TIMEOUT (no answer). "
                     "Try: swap A<->B, check NPK power & baud, slave ID, registers (manual).");
    } else
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
    Serial.println("http.begin failed (bad URL?)");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  // code -1 = could not connect (server down, wrong IP, firewall, not 0.0.0.0 on PC)
  if (code < 0)
    Serial.println("POST failed: connection error (-1). Run FastAPI on this PC, "
                   "use correct LAN IP, uvicorn --host 0.0.0.0 --port 8000, open firewall.");
  else
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
  delay(2000); // DHT22 needs a moment after power-up
  analogReadResolution(12);
  // 0…~3.1 V full scale for soil AO on VP(36) when using 11 dB
  analogSetPinAttenuation((gpio_num_t)PIN_SOIL_ADC, ADC_11db);

  Serial2.setRxBufferSize(512);
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

  Serial.println("Ready. DHT=4, soil=36, LDR=5, UART2=16/17, DE+RE=18 NPK/Modbus");
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

  // DHT (one read + one retry)
  float temp = NAN, hum = NAN;
  for (int try_dht = 0; try_dht < 2; try_dht++) {
    TempAndHumidity th = dht.getTempAndHumidity();
    if (dht.getStatus() == 0) { // DHT_ERROR_NONE
      temp = th.temperature;
      hum = th.humidity;
      break;
    }
    Serial.printf("DHT err (try %d) %d\n", try_dht, (int)dht.getStatus());
    delay(DHT_READ_DELAY_MS);
  }

  // Soil
  int raw = analogRead(PIN_SOIL_ADC);
  int soil = soil_moisture_percent(raw);
  // LDR digital -> numeric for JSON
  int lightV = light_value_from_digital(digitalRead(PIN_LDR) == HIGH);

  // NPK
  int n = 0, p = 0, k = 0;
  if (!read_npk(n, p, k))
    Serial.println("NPK: using zeros (Modbus read failed; check A/B, baud, registers).");

  Serial.printf("T=%.1f H=%.1f soil%%=%d raw=%d L=%d N=%d P=%d K=%d\n", temp, hum, soil, raw, lightV, n, p, k);

  if (!isnan(temp) && !isnan(hum))
    post_sensor_json(temp, hum, soil, lightV, n, p, k);
  else
    Serial.println("Skip POST: invalid DHT");
}
