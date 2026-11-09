// Board wiring for the ESP32-3248S035 ("CYD 3.5", ST7796 + XPT2046).
#pragma once
#define VOLTOL_FW "1.7.1"

#ifdef VOLTOL_HEADLESS
// ---- ESP32-S3 DevKitC-1 (no screen, everything on the web page) ----
//   VOTOL TX -> GPIO18   VOTOL RX <- GPIO17   GND - GND
//   GPS TX   -> GPIO16   GPS RX   <- GPIO17 (same wire as VOTOL RX)
#define VOTOL_RX_PIN 18
#define VOTOL_TX_PIN 17
#define GPS_RX_PIN 16
#define GPS_TX_PIN -1
#define HAS_SD 0
#else
// ---- ESP32-3248S035 "CYD 3.5" ----
// VOTOL programming plug (UART, 3.3 V logic) -> CYD extension connector.
//   VOTOL TX -> IO35 (input only)   VOTOL RX <- IO22   GND - GND
// Optional GPS: GPS TX -> IO21, GPS RX <- IO22 (same wire as VOTOL RX: the
// firmware sends the u-blox setup on it), 3.3 V + GND.
#define VOTOL_RX_PIN 35
#define VOTOL_TX_PIN 22
#define GPS_RX_PIN 21
#define GPS_TX_PIN -1
#define HAS_SD 1
// microSD slot on the board (VSPI)
#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#endif

// Web page / CSV download over the board's own Wi-Fi hotspot.
#define AP_PASSWORD "voltol1234"
