#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "time.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WebServer.h>
#include <vector>
#include <WiFiManager.h>

#define BUTTON_PIN 25
bool buttonWasPressed = false;
unsigned long pressStartTime = 0;
const unsigned long RESET_PRESS_TIME = 10000;

const char* unihikerServer = "http://192.168.107.13:5000/log";
const char* unihikerResetURL = "http://192.168.107.13:5000/reset";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 0;

#define ONE_WIRE_BUS 27
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

unsigned long previousMillis = 0;
const long interval = 60000;

WebServer fallbackServer(80);
bool fallbackMode = false;

struct Reading {
  String timestamp;
  float temperature;
};
std::vector<Reading> readingBuffer;

void setupTime();
void readAndBufferData();
bool tryFlushBuffer();
void startFallbackServer();
String getFormattedTimestamp();

bool resetDataOnServer() {
  HTTPClient http;
  http.begin(unihikerResetURL);
  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.printf("[RESET] /reset returned HTTP %d\n", httpCode);
    http.end();
    return (httpCode == 200);
  } else {
    Serial.printf("[RESET] Error on HTTP request: %d\n", httpCode);
    http.end();
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting...");

  sensors.begin();

  WiFiManager wifiManager;
  if (!wifiManager.autoConnect("ESP32_AP")) {
    Serial.println("Failed to connect - restarting...");
    ESP.restart();
    delay(1000);
  }
  Serial.println("WiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  setupTime();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loop() {
  bool currentlyPressed = (digitalRead(BUTTON_PIN) == LOW);

  if (currentlyPressed && !buttonWasPressed) {
    buttonWasPressed = true;
    pressStartTime = millis();
  }
  else if (!currentlyPressed && buttonWasPressed) {
    buttonWasPressed = false;
    unsigned long pressDuration = millis() - pressStartTime;
    Serial.printf("[BUTTON] Held for %lu ms\n", pressDuration);
    
    if (pressDuration >= RESET_PRESS_TIME) {
      Serial.println("[BUTTON] Held for 10 seconds or more -> Resetting data on server...");
      if (resetDataOnServer()) {
        Serial.println("[BUTTON] Data reset successfully!");
      } else {
        Serial.println("[BUTTON] Failed to reset data!");
      }
    } else {
      Serial.println("[BUTTON] Press too short, no action taken.");
    }
  }

  if (fallbackMode) {
    fallbackServer.handleClient();
    static unsigned long lastRetry = 0;
    unsigned long now = millis();
    if (now - lastRetry >= 30000) {
      lastRetry = now;
      Serial.println("Fallback: Attempting to flush buffer to server...");
      if (tryFlushBuffer()) {
        fallbackMode = false;
        fallbackServer.stop();
        Serial.println("Exited fallback mode, resuming normal operation.");
      }
    }
    if (now - previousMillis >= interval) {
      previousMillis = now;
      readAndBufferData();
    }
    return;
  }

  unsigned long now = millis();
  if (now - previousMillis >= interval) {
    previousMillis = now;
    readAndBufferData();
    if (!tryFlushBuffer()) {
      startFallbackServer();
    }
  }
}

void setupTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Synchronizing time with NTP...");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nTime synchronized!");
}

String getFormattedTimestamp() {
  time_t now = time(nullptr);
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);
  timeInfo.tm_sec = 0;
  char buffer[50];
  strftime(buffer, sizeof(buffer), "%A %d/%m - %H:%M:%S", &timeInfo);
  return String(buffer);
}

void readAndBufferData() {
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  Reading r;
  r.timestamp = getFormattedTimestamp();
  r.temperature = tempC;
  readingBuffer.push_back(r);
  Serial.printf("Buffered reading: %s, %.2f°C (buffer size=%d)\n",
                r.timestamp.c_str(), tempC, readingBuffer.size());
}

bool tryFlushBuffer() {
  if (readingBuffer.empty()) {
    Serial.println("Buffer is empty, nothing to flush.");
    return true;
  }
  Serial.println("Flushing buffer to UNIHIKER server...");
  HTTPClient http;
  http.begin(unihikerServer);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  for (size_t i = 0; i < readingBuffer.size(); i++) {
    Reading &r = readingBuffer[i];
    String postData = "timestamp=" + r.timestamp + "&temperature=" + String(r.temperature, 2);
    int httpCode = http.POST(postData);
    if (httpCode <= 0) {
      Serial.printf("Error posting reading %d: code=%d\n", i, httpCode);
      http.end();
      return false;
    } else {
      Serial.printf("Reading %d posted -> HTTP %d\n", i, httpCode);
    }
  }
  http.end();
  readingBuffer.clear();
  Serial.println("All buffered readings flushed successfully!");
  return true;
}

void startFallbackServer() {
  if (fallbackMode) return;
  fallbackMode = true;
  Serial.println("Starting fallback server on port 80...");

  fallbackServer.on("/", HTTP_GET, []() {
    String html = "<html><head><title>Fallback Mode</title></head><body>";
    html += "<h1>Fallback Mode</h1>";
    html += "<p>Unable to reach UNIHIKER server. Buffer size: " + String(readingBuffer.size()) + "</p>";
    html += "<p>Please wait or try /flush.</p>";
    html += "</body></html>";
    fallbackServer.send(200, "text/html", html);
  });

  fallbackServer.on("/flush", HTTP_GET, []() {
    bool ok = tryFlushBuffer();
    if (ok) {
      fallbackServer.send(200, "text/plain", "Flush succeeded, exiting fallback mode.");
      fallbackMode = false;
      fallbackServer.stop();
    } else {
      fallbackServer.send(200, "text/plain", "Flush failed, still in fallback mode.");
    }
  });
  fallbackServer.begin();
  Serial.println("Fallback server started. Visit http://" + WiFi.localIP().toString() + "/");
}