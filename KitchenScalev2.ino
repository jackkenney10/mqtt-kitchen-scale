#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "HX711.h"

// =====================================================
// User configuration
// =====================================================
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";

const char* MQTT_BROKER = "";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = "kitchen-scale-01";

const char* TOPIC_TELEMETRY = "home/scale01/telemetry";
const char* TOPIC_STATUS    = "home/scale01/status";
const char* TOPIC_COMMAND   = "home/scale01/command/#";

// HX711 pins
const int HX711_DOUT_PIN = 4;
const int HX711_SCK_PIN  = 5;

// Calibration math:
// grams = (raw - offset) / scaleFactor
long offset = 0;
float scaleFactor = 1000.0f;

// Behavior
const unsigned long TELEMETRY_INTERVAL_MS = 500;
const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long MQTT_RETRY_MS = 5000;

const int MOVING_AVG_WINDOW = 10;
const int STABILITY_WINDOW = 10;
const float STABILITY_THRESHOLD_G = 0.5f;

const bool AUTO_TARE_ON_BOOT = false;   // I recommend false once calibrated
const bool SAVE_TARE_TO_FLASH = true;   // save tare offset when tare command is run

// =====================================================
// Globals
// =====================================================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
HX711 scale;
Preferences prefs;

unsigned long bootMs = 0;
unsigned long lastTelemetryMs = 0;
unsigned long lastWifiReconnectAttempt = 0;
unsigned long lastMqttReconnectAttempt = 0;

// Sample smoothing
float sampleBuffer[MOVING_AVG_WINDOW] = {0};
int sampleIndex = 0;
bool sampleBufferFilled = false;

// Stability detection
float stabilityBuffer[STABILITY_WINDOW] = {0};
int stabilityIndex = 0;
bool stabilityBufferFilled = false;

// Last published values
float lastRawAverage = 0.0f;
float lastGrams = 0.0f;
bool lastStable = false;

// =====================================================
// Utility helpers
// =====================================================
void addSample(float value) {
  sampleBuffer[sampleIndex] = value;
  sampleIndex = (sampleIndex + 1) % MOVING_AVG_WINDOW;
  if (sampleIndex == 0) {
    sampleBufferFilled = true;
  }
}

float getSampleAverage() {
  int count = sampleBufferFilled ? MOVING_AVG_WINDOW : sampleIndex;
  if (count <= 0) return 0.0f;

  float sum = 0.0f;
  for (int i = 0; i < count; i++) {
    sum += sampleBuffer[i];
  }
  return sum / count;
}

void addStabilitySample(float grams) {
  stabilityBuffer[stabilityIndex] = grams;
  stabilityIndex = (stabilityIndex + 1) % STABILITY_WINDOW;
  if (stabilityIndex == 0) {
    stabilityBufferFilled = true;
  }
}

bool isStable() {
  int count = stabilityBufferFilled ? STABILITY_WINDOW : stabilityIndex;
  if (count < 3) return false;

  float minVal = stabilityBuffer[0];
  float maxVal = stabilityBuffer[0];

  for (int i = 1; i < count; i++) {
    if (stabilityBuffer[i] < minVal) minVal = stabilityBuffer[i];
    if (stabilityBuffer[i] > maxVal) maxVal = stabilityBuffer[i];
  }

  return (maxVal - minVal) < STABILITY_THRESHOLD_G;
}

float rawToGrams(float raw) {
  return (raw - offset) / scaleFactor;
}

// =====================================================
// Persistence
// =====================================================
bool loadCalibration() {
  prefs.begin("scale", true);  // read-only

  bool hasOffset = prefs.isKey("offset");
  bool hasScale  = prefs.isKey("scale");

  if (hasOffset) {
    offset = prefs.getLong("offset", 0);
  }

  if (hasScale) {
    scaleFactor = prefs.getFloat("scale", 1000.0f);
  }

  prefs.end();

  return hasOffset && hasScale;
}

void saveCalibration() {
  prefs.begin("scale", false);
  prefs.putLong("offset", offset);
  prefs.putFloat("scale", scaleFactor);
  prefs.end();

  Serial.println("Calibration saved.");
}

void clearCalibration() {
  prefs.begin("scale", false);
  prefs.clear();
  prefs.end();

  offset = 0;
  scaleFactor = 1000.0f;

  Serial.println("Calibration cleared.");
}

// =====================================================
// Scale functions
// =====================================================
bool waitForScaleReady(unsigned long timeoutMs = 1000) {
  unsigned long start = millis();
  while (!scale.is_ready()) {
    delay(5);
    if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

bool tareScale(int samples = 20, bool persist = true) {
  long sum = 0;
  int validReads = 0;

  for (int i = 0; i < samples; i++) {
    if (waitForScaleReady(1000)) {
      sum += scale.read();
      validReads++;
    }
    delay(50);
  }

  if (validReads > 0) {
    offset = sum / validReads;

    if (persist && SAVE_TARE_TO_FLASH) {
      saveCalibration();
    }

    Serial.print("New tare offset: ");
    Serial.println(offset);
    return true;
  } else {
    Serial.println("Tare failed: no valid HX711 reads.");
    return false;
  }
}

bool calibrateScale(float knownGrams, int samples = 20) {
  if (knownGrams <= 0.0f) {
    Serial.println("Calibration failed: knownGrams must be > 0.");
    return false;
  }

  long sum = 0;
  int validReads = 0;

  for (int i = 0; i < samples; i++) {
    if (waitForScaleReady(1000)) {
      sum += scale.read();
      validReads++;
    }
    delay(50);
  }

  if (validReads <= 0) {
    Serial.println("Calibration failed: no valid HX711 reads.");
    return false;
  }

  long rawLoaded = sum / validReads;
  float newScaleFactor = (rawLoaded - offset) / knownGrams;

  if (newScaleFactor == 0.0f) {
    Serial.println("Calibration failed: computed scaleFactor is zero.");
    return false;
  }

  scaleFactor = newScaleFactor;
  saveCalibration();

  Serial.print("Calibration successful. New scaleFactor: ");
  Serial.println(scaleFactor, 6);
  return true;
}

// =====================================================
// Wi-Fi / MQTT
// =====================================================
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Starting Wi-Fi...");
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  unsigned long now = millis();
  if (now - lastWifiReconnectAttempt >= WIFI_RETRY_MS) {
    lastWifiReconnectAttempt = now;
    Serial.println("Wi-Fi not connected, retrying...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  return false;
}

bool ensureMQTT() {
  if (mqttClient.connected()) {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  unsigned long now = millis();
  if (now - lastMqttReconnectAttempt < MQTT_RETRY_MS) {
    return false;
  }

  lastMqttReconnectAttempt = now;

  Serial.print("Attempting MQTT connect... ");

  bool ok = mqttClient.connect(
    MQTT_CLIENT_ID,
    nullptr,
    nullptr,
    TOPIC_STATUS,
    1,
    true,
    "offline"
  );

  if (ok) {
    Serial.println("connected");
    mqttClient.subscribe(TOPIC_COMMAND);
    mqttClient.publish(TOPIC_STATUS, "online", true);
    return true;
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }
}

// =====================================================
// Telemetry / commands
// =====================================================
void publishTelemetry(float rawAverage, float grams, bool stable) {
  StaticJsonDocument<320> doc;

  doc["raw_avg"] = rawAverage;
  doc["grams"] = grams;
  doc["stable"] = stable;
  doc["offset"] = offset;
  doc["scale_factor"] = scaleFactor;
  doc["uptime_s"] = (millis() - bootMs) / 1000;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["heap_free"] = ESP.getFreeHeap();

  char payload[320];
  size_t len = serializeJson(doc, payload);

  mqttClient.publish(TOPIC_TELEMETRY, payload, len);
}

void handleCommandTopic(const String& topic, const String& message) {
  Serial.print("MQTT command topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(message);

  if (topic == "home/scale01/command/tare") {
    bool ok = tareScale(20, true);
    mqttClient.publish(TOPIC_STATUS, ok ? "tare_ok" : "tare_failed", true);
    return;
  }

  if (topic == "home/scale01/command/calibrate") {
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, message);

    if (err) {
      Serial.println("Calibration JSON parse failed.");
      mqttClient.publish(TOPIC_STATUS, "calibration_json_failed", true);
      return;
    }

    if (!doc["known_grams"].is<float>() && !doc["known_grams"].is<int>()) {
      Serial.println("Calibration payload missing known_grams.");
      mqttClient.publish(TOPIC_STATUS, "calibration_missing_known_grams", true);
      return;
    }

    float knownGrams = doc["known_grams"];
    bool ok = calibrateScale(knownGrams, 20);
    mqttClient.publish(TOPIC_STATUS, ok ? "calibration_ok" : "calibration_failed", true);
    return;
  }

  if (topic == "home/scale01/command/reboot") {
    mqttClient.publish(TOPIC_STATUS, "rebooting", true);
    delay(250);
    ESP.restart();
    return;
  }

  if (topic == "home/scale01/command/clear_calibration") {
    clearCalibration();
    mqttClient.publish(TOPIC_STATUS, "calibration_cleared", true);
    return;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  handleCommandTopic(String(topic), message);
}

// =====================================================
// Setup / loop
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Kitchen scale starting...");

  bootMs = millis();

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

  bool calibLoaded = loadCalibration();
  Serial.println(calibLoaded ? "Loaded saved calibration." : "No saved calibration found.");

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);

  startWiFi();

  if (AUTO_TARE_ON_BOOT) {
    Serial.println("Performing startup tare...");
    tareScale(20, true);
  }
}

void loop() {
  ensureWiFi();
  ensureMQTT();

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  if (scale.is_ready()) {
    long raw = scale.read();

    addSample((float)raw);
    float rawAverage = getSampleAverage();
    float grams = rawToGrams(rawAverage);

    addStabilitySample(grams);
    bool stable = isStable();

    lastRawAverage = rawAverage;
    lastGrams = grams;
    lastStable = stable;
  }

  if (mqttClient.connected() && millis() - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = millis();
    publishTelemetry(lastRawAverage, lastGrams, lastStable);

    Serial.print("Raw avg: ");
    Serial.print(lastRawAverage, 1);
    Serial.print(" | Grams: ");
    Serial.print(lastGrams, 2);
    Serial.print(" | Stable: ");
    Serial.println(lastStable ? "true" : "false");
  }

  delay(10);
}