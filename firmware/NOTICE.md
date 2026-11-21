# Firmware notice

The firmware in `BirdhouseCam/` is based on the `CameraWebServer` example
(`libraries/ESP32/examples/Camera/CameraWebServer`) of the Arduino core for the ESP32,
arduino-esp32 3.1.3, https://github.com/espressif/arduino-esp32. Licence texts: `../LICENSES/`.

| File | Origin | Licence |
|---|---|---|
| `BirdhouseCam/app_httpd.cpp` | unchanged from the example | © 2015-2016 Espressif Systems (Shanghai) PTE LTD, Apache-2.0 (see file header) |
| `BirdhouseCam/camera_index.h` | unchanged from the example | as arduino-esp32: LGPL-2.1 |
| `BirdhouseCam/camera_pins.h` | unchanged from the example | as arduino-esp32: LGPL-2.1 |
| `BirdhouseCam/BirdhouseCam.ino` | derived from `CameraWebServer.ino` of the example; changed in November 2026: Wi-Fi-first start, OTA updates, diagnostics server, sensor retry, Wi-Fi watchdog | © 2026 Bertlab, LGPL-2.1-only |
| `BirdhouseCam/partitions.csv`, `BirdhouseCam/wifi_secret.example.h` | written for this project, November 2026 | © 2026 Bertlab, LGPL-2.1-only |
