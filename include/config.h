// Board wiring for the ESP32-3248S035 ("CYD 3.5", ST7796 + XPT2046).
#pragma once
#define VOLTOL_FW "1.0.0"

// VOTOL programming plug (UART, 3.3 V logic) -> CYD extension connector.
//   VOTOL TX -> IO35 (input only)   VOTOL RX <- IO22   GND - GND
#define VOTOL_RX_PIN 35
#define VOTOL_TX_PIN 22
// Optional GPS module (NMEA): GPS TX -> IO21. 3.3 V + GND from the same plug.
#define GPS_RX_PIN 21
#define GPS_TX_PIN -1
// microSD slot on the board (VSPI)
#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

// Web page / CSV download over the board's own Wi-Fi hotspot.
#define AP_PASSWORD "voltol1234"
