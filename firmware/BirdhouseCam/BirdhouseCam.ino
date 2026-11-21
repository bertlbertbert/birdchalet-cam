// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (c) 2026 Bertlab. Derived from the CameraWebServer example of arduino-esp32 3.1.3
// (Espressif Systems), modified in November 2026.
//
// BirdhouseCam 1.0.0 - Seeed Studio XIAO ESP32S3 Sense as a Wi-Fi camera that ALWAYS stays reachable.
//
// Based on the CameraWebServer example of the Arduino core for the ESP32 (arduino-esp32 3.1.3).
// Differences to that example:
//  1. Wi-Fi first. If the image sensor does not answer at boot, the board still joins the network
//     (the example returns from setup() before WiFi.begin(), which leaves the camera unreachable).
//  2. Over-the-air updates (ArduinoOTA, port 3232, password from wifi_secret.h). partitions.csv
//     provides two app slots; after the first USB flash every further update works without a cable.
//     That matters here: the board sits inside a sealed capsule inside the camera tower.
//  3. Diagnostics on port 8080 (/diag, JSON): sensor state, error code, init attempts, uptime,
//     reset reason, signal strength, chip temperature, free memory. Works without a sensor, too.
//  4. The sensor init is retried every 30 seconds until it answers; then the camera server starts
//     as usual (port 80: web UI, /capture, /status; port 81: /stream).
//  5. Wi-Fi watchdog: if the connection is gone for 30 seconds, the board reconnects.
//
// wifi_secret.h is NOT part of this repository. Copy wifi_secret.example.h to wifi_secret.h and
// fill in your own values: WIFI_SSID_SECRET, WIFI_PASS_SECRET, OTA_PASS_SECRET and, optionally,
// CAMERA_NAME (default "birdhouse-cam"; plain hostname: letters, digits and hyphens only).
// Never commit that file.

#include "esp_camera.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include "esp_system.h"

#define CAMERA_MODEL_XIAO_ESP32S3  // Has PSRAM
#include "camera_pins.h"
#include "wifi_secret.h"

#ifndef CAMERA_NAME
#define CAMERA_NAME "birdhouse-cam"
#endif
#define FW_VERSION "BirdhouseCam 1.0.0"

void startCameraServer();
void setupLedFlash(int pin);

static WebServer diag(8080);
static bool camera_ok = false;
static bool server_running = false;
static esp_err_t camera_error = ESP_OK;
static uint32_t camera_attempts = 0;
static uint32_t last_attempt_ms = 0;
static uint32_t wifi_lost_since_ms = 0;
static uint32_t ota_runs = 0;

static camera_config_t make_camera_config() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }
  return config;
}

static void try_camera() {
  camera_attempts++;
  last_attempt_ms = millis();
  if (camera_attempts > 1) esp_camera_deinit();  // clean restart after a failed attempt
  camera_config_t config = make_camera_config();
  camera_error = esp_camera_init(&config);
  if (camera_error != ESP_OK) {
    Serial.printf("Camera init %u failed: 0x%x\n", camera_attempts, camera_error);
    camera_ok = false;
    return;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    camera_error = ESP_FAIL;
    camera_ok = false;
    return;
  }
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, FRAMESIZE_QVGA);
  camera_ok = true;
  Serial.printf("Camera ready after %u attempt(s), PID 0x%x\n", camera_attempts, s->id.PID);
  if (!server_running) {
#if defined(LED_GPIO_NUM)
    setupLedFlash(LED_GPIO_NUM);
#endif
    startCameraServer();
    server_running = true;
  }
}

static const char *reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "watchdog_int";
    case ESP_RST_TASK_WDT: return "watchdog_task";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    default: return "unknown";
  }
}

static void send_diag() {
  sensor_t *s = camera_ok ? esp_camera_sensor_get() : NULL;
  char buf[640];
  snprintf(buf, sizeof(buf),
           "{\"name\":\"%s\",\"version\":\"%s\",\"uptime_s\":%lu,\"reset_reason\":\"%s\","
           "\"ip\":\"%s\",\"rssi\":%d,\"camera_ok\":%s,\"camera_error\":\"0x%x\",\"camera_attempts\":%u,"
           "\"sensor_pid\":\"0x%x\",\"camera_server\":%s,\"chip_temp_c\":%.1f,\"heap_free\":%u,"
           "\"psram_free\":%u,\"ota_runs\":%u}",
           CAMERA_NAME, FW_VERSION, (unsigned long)(millis() / 1000), reset_reason(),
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), camera_ok ? "true" : "false",
           (unsigned)camera_error, camera_attempts, s ? s->id.PID : 0, server_running ? "true" : "false",
           temperatureRead(), ESP.getFreeHeap(), ESP.getFreePsram(), ota_runs);
  diag.send(200, "application/json", buf);
}

static void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(CAMERA_NAME);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID_SECRET, WIFI_PASS_SECRET);
  Serial.print("Wi-Fi connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi %s, IP %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "not connected yet",
                WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println(FW_VERSION);

  connect_wifi();  // 1. Wi-Fi first

  ArduinoOTA.setHostname(CAMERA_NAME);  // 2. over-the-air updates
  ArduinoOTA.setPassword(OTA_PASS_SECRET);
  ArduinoOTA.onStart([]() {
    ota_runs++;
    if (camera_ok) esp_camera_deinit();  // free memory and the bus for the update
    camera_ok = false;
  });
  ArduinoOTA.begin();

  diag.on("/diag", send_diag);  // 3. diagnostics, even without a sensor
  diag.on("/", send_diag);
  diag.begin();

  try_camera();  // 4. sensor; on failure retried every 30 s
}

void loop() {
  ArduinoOTA.handle();
  diag.handleClient();

  if (!camera_ok && millis() - last_attempt_ms > 30000) try_camera();

  if (WiFi.status() != WL_CONNECTED) {  // 5. Wi-Fi watchdog
    if (wifi_lost_since_ms == 0) wifi_lost_since_ms = millis();
    if (millis() - wifi_lost_since_ms > 30000) {
      Serial.println("Wi-Fi lost for 30 s, reconnecting");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID_SECRET, WIFI_PASS_SECRET);
      wifi_lost_since_ms = millis();
    }
  } else {
    wifi_lost_since_ms = 0;
  }
  delay(5);
}
