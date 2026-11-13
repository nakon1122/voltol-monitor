// Board wiring for the ESP32-3248S035 ("CYD 3.5", ST7796 + XPT2046).
#pragma once
#define VOTOL_FW "1.9.0"

#if defined(VOTOL_P4C5)
// ---- KSDIY ESP32-P4C5 4.3" MIPI board (ST7102 480x800 + ST7123 touch, C5 Wi-Fi, ML307C 4G) ----
// 2x10 header (looking at the board, header at the top):
//   top row:    DAC  GND  IO50 IO48 IO46 IO45 IO27 IO29 GND  RS485-B
//   bottom row: 5V   3V3  IO49 IO47 GND  IO26 IO28 IO30 GND  RS485-A
// VOTOL TX -> IO48, VOTOL RX <- IO47 (+ GPS RX), GND; GPS TX -> IO46, GPS VCC 3V3
#define VOTOL_BOARD "p4c5"
#define VOTOL_RX_PIN 48
#define VOTOL_TX_PIN 47
#define GPS_RX_PIN 46
#define GPS_TX_PIN -1
#define HAS_SD 2  // TF card on SDMMC slot 0 (IO_MUX pins)
#define SDMMC_CLK 43
#define SDMMC_CMD 44
#define SDMMC_D0 39
#define SDMMC_D1 40
#define SDMMC_D2 41
#define SDMMC_D3 42
#elif defined(VOTOL_S340)
// ---- ESP32-S3 4.0" 480x320 IPS (LCDwiki ES3C40P: ST7796 SPI + FT6336 touch) ----
// VOTOL programming plug -> the 4-pin "UART" connector on the back (3.3 V logic):
//   VOTOL TX -> RXD (GPIO44)   VOTOL RX <- TXD (GPIO43)   GND - GND
//   (USB-C is the board's own USB port, so the UART connector is free for VOTOL)
// Optional GPS on the IO connector: GPS TX -> IO2, GPS RX <- TXD (same wire as VOTOL RX)
#define VOTOL_BOARD "s340"
#define VOTOL_RX_PIN 44
#define VOTOL_TX_PIN 43
#define GPS_RX_PIN 2
#define GPS_TX_PIN -1
#define HAS_SD 2  // microSD on SDMMC 4-bit
#define SDMMC_CLK 38
#define SDMMC_CMD 40
#define SDMMC_D0 39
#define SDMMC_D1 41
#define SDMMC_D2 48
#define SDMMC_D3 47
#elif defined(VOTOL_HEADLESS)
// ---- ESP32-S3 DevKitC-1 (no screen, everything on the web page) ----
//   VOTOL TX -> GPIO18   VOTOL RX <- GPIO17   GND - GND
//   GPS TX   -> GPIO16   GPS RX   <- GPIO17 (same wire as VOTOL RX)
#define VOTOL_BOARD "s3"
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
#define VOTOL_BOARD "cyd35"
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
#define AP_PASSWORD "votol1234"
