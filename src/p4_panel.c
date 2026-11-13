// KSDIY ESP32-P4C5 4.3" panel bring-up (plain C: the esp_lcd config macros use C initialisers).
// Sequence and values follow the board vendor's BSP (kevincoooool/ksdiy_p4c5_bsp 1.3.3):
// LDO3 2.5 V for the DSI PHY, I2C0 on GPIO7/8 shared by AXP2101 + ST7123, ST7102 over
// 2-lane MIPI DSI at 820 Mbps, LCD/touch reset on GPIO22, touch INT GPIO23, backlight GPIO6.
#ifdef VOTOL_P4C5
#include <string.h>
#include "p4_panel.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7102.h"
#include "esp_lcd_touch_st7123.h"
#include "axp2101.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "p4panel";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static i2c_master_bus_handle_t s_bus;

static const st7102_lcd_init_cmd_t s_init[] = {
    {0x99, (uint8_t[]){0x71, 0x02, 0xa2}, 3, 0},
    {0x99, (uint8_t[]){0x71, 0x02, 0xa3}, 3, 0},
    {0x99, (uint8_t[]){0x71, 0x02, 0xa4}, 3, 0},
    {0xB0, (uint8_t[]){0x22, 0x57, 0x1E, 0x61, 0x2F, 0x57, 0x61}, 7, 0},
    {0xB7, (uint8_t[]){0x64, 0x64}, 2, 0},
    {0xBF, (uint8_t[]){0xB4, 0xB4}, 2, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x13, 0x24, 0x44, 0x00, 0x74, 0x03, 0xB8, 0x04, 0x11, 0x16, 0x08, 0x86, 0x04, 0x21, 0xD3, 0x02, 0x10,
                       0x0F, 0x22, 0x4D, 0x0E, 0x90, 0x09, 0x32, 0xF0, 0x0B, 0x40, 0x0E, 0xF3, 0x7D, 0x0E, 0xA9, 0xBF, 0x03, 0xC4}, 37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x13, 0x24, 0x44, 0x00, 0x74, 0x03, 0xB8, 0x04, 0x11, 0x16, 0x08, 0x86, 0x04, 0x21, 0xD3, 0x02, 0x10,
                       0x0F, 0x22, 0x4D, 0x0E, 0x90, 0x09, 0x32, 0xF0, 0x0B, 0x40, 0x0E, 0xF3, 0x7D, 0x0E, 0xA9, 0xBF, 0x03, 0xC4}, 37, 0},
    {0xD7, (uint8_t[]){0x10, 0x0C, 0x36, 0x19, 0x90, 0x90}, 6, 0},
    {0xA3, (uint8_t[]){0x51, 0x03, 0x80, 0xCF, 0x44, 0x00, 0x00, 0x00, 0x00, 0x04, 0x78, 0x78, 0x00, 0x1A, 0x00, 0x45,
                       0x05, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x02, 0x20, 0x52, 0x00, 0x05, 0x00, 0x00, 0xFF}, 32, 0},
    {0xA6, (uint8_t[]){0x02, 0x00, 0x24, 0x55, 0x35, 0x00, 0x38, 0x00, 0x78, 0x78, 0x00, 0x24, 0x55, 0x36, 0x00, 0x37, 0x00, 0x78, 0x78, 0x02, 0xAC, 0x51,
                       0x3A, 0x00, 0x00, 0x00, 0x78, 0x78, 0x03, 0xAC, 0x21, 0x00, 0x04, 0x00, 0x00, 0x78, 0x78, 0x3e, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 44, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x00, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x04, 0x03, 0x78, 0x78, 0x00, 0x64, 0x40, 0x25, 0x34, 0x00, 0x00, 0x02, 0x01, 0x78, 0x78,
                       0x00, 0x64, 0x40, 0x4B, 0x5A, 0x00, 0x00, 0x02, 0x01, 0x78, 0x78, 0x00, 0x24, 0x40, 0x69, 0x78, 0x00, 0x00, 0x00, 0x00, 0x78, 0x78, 0x00, 0x44}, 48, 0},
    {0xAC, (uint8_t[]){0x08, 0x0A, 0x11, 0x00, 0x13, 0x03, 0x1B, 0x18, 0x06, 0x1A, 0x19, 0x1B, 0x1B, 0x1B, 0x18, 0x1B, 0x09, 0x0B, 0x10,
                       0x02, 0x12, 0x01, 0x1B, 0x18, 0x06, 0x1A, 0x19, 0x1B, 0x1B, 0x1B, 0x18, 0x1B, 0xFF, 0x67, 0xFF, 0x67, 0x00}, 37, 0},
    {0xAD, (uint8_t[]){0xCC, 0x40, 0x46, 0x11, 0x04, 0x78, 0x78}, 7, 0},
    {0xE8, (uint8_t[]){0x30, 0x07, 0x00, 0x94, 0x94, 0x9C, 0x00, 0xE2, 0x04, 0x00, 0x00, 0x00, 0x00, 0xEF}, 14, 0},
    {0xE7, (uint8_t[]){0x8B, 0x3C, 0x00, 0x0C, 0xF0, 0x5D, 0x00, 0x5D, 0x00, 0x5D, 0x00, 0x5D, 0x00, 0xFF, 0x00, 0x08, 0x7B,
                       0x00, 0x00, 0xC8, 0x6A, 0x5A, 0x08, 0x1A, 0x3C, 0x00, 0x81, 0x01, 0xCC, 0x01, 0x7F, 0xF0, 0x22}, 33, 0},
    {0x11, (uint8_t[]){0x00}, 0, 600},
    {0x29, (uint8_t[]){0x00}, 0, 120},
};

static void pmic_rails(void) {
  if (axp2101_init(s_bus) != ESP_OK || axp2101_check_chip_id() != ESP_OK) { ESP_LOGW(TAG, "AXP2101 not found"); return; }
  for (int attempt = 0; attempt < 3; attempt++) {
    int failed = 0;
    failed += axp2101_set_dcdc_voltage(AXP2101_DCDC1, 3.3f) != ESP_OK;
    failed += axp2101_set_dcdc_enabled(AXP2101_DCDC1, true) != ESP_OK;
    failed += axp2101_set_ldo_voltage(AXP2101_LDO_ALDO1, 1.8f) != ESP_OK;
    failed += axp2101_set_ldo_enabled(AXP2101_LDO_ALDO1, true) != ESP_OK;
    failed += axp2101_set_ldo_voltage(AXP2101_LDO_ALDO3, 3.3f) != ESP_OK;
    failed += axp2101_set_ldo_enabled(AXP2101_LDO_ALDO3, true) != ESP_OK;
    failed += axp2101_set_ldo_voltage(AXP2101_LDO_ALDO4, 2.9f) != ESP_OK;
    failed += axp2101_set_ldo_enabled(AXP2101_LDO_ALDO4, true) != ESP_OK;
    if (!failed) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}

int p4_panel_init(void) {
  gpio_config_t rst = {.pin_bit_mask = 1ULL << 22, .mode = GPIO_MODE_OUTPUT};
  gpio_config(&rst);
  gpio_set_level(22, 1);

  static esp_ldo_channel_handle_t ldo;
  esp_ldo_channel_config_t ldo_cfg = {.chan_id = 3, .voltage_mv = 2500};
  if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) return -1;

  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0, .sda_io_num = 7, .scl_io_num = 8, .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) return -2;
  pmic_rails();

  esp_lcd_dsi_bus_handle_t dsi = NULL;
  esp_lcd_dsi_bus_config_t dsi_cfg = {.bus_id = 0, .num_data_lanes = 2, .phy_clk_src = 0, .lane_bit_rate_mbps = 820};
  if (esp_lcd_new_dsi_bus(&dsi_cfg, &dsi) != ESP_OK) return -3;
  esp_lcd_panel_io_handle_t io = NULL;
  esp_lcd_dbi_io_config_t dbi = ST7102_MIPI_PANEL_IO_DBI_CONFIG();
  if (esp_lcd_new_panel_io_dbi(dsi, &dbi, &io) != ESP_OK) return -4;
  esp_lcd_dpi_panel_config_t dpi = ST7102_MIPI_480_800_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB565);
  dpi.num_fbs = 1;
  st7102_vendor_config_t vendor = {
      .init_cmds = s_init,
      .init_cmds_size = sizeof s_init / sizeof s_init[0],
      .flags.use_mipi_interface = 1,
      .mipi_config = {.dsi_bus = dsi, .dpi_config = &dpi},
  };
  esp_lcd_panel_dev_config_t dev = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16, .vendor_config = &vendor};
  if (esp_lcd_new_panel_st7102(io, &dev, &s_panel) != ESP_OK) return -5;
  esp_lcd_panel_reset(s_panel);
  if (esp_lcd_panel_init(s_panel) != ESP_OK) return -6;
  esp_lcd_panel_disp_on_off(s_panel, true);

  esp_lcd_panel_io_handle_t tio = NULL;
  esp_lcd_panel_io_i2c_config_t tcfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
  tcfg.scl_speed_hz = 400000;
  if (esp_lcd_new_panel_io_i2c(s_bus, &tcfg, &tio) == ESP_OK) {
    esp_lcd_touch_config_t tc = {.x_max = 480, .y_max = 800, .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_23};
    if (esp_lcd_touch_new_i2c_st7123(tio, &tc, &s_touch) != ESP_OK) s_touch = NULL;
  }
  ESP_LOGI(TAG, "panel ok, touch %s", s_touch ? "ok" : "missing");
  return 0;
}

int p4_panel_draw(int x0, int y0, int x1, int y1, const void *rgb565) {
  return s_panel ? (int)esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, rgb565) : -1;
}

int p4_touch_read(int *x, int *y) {
  if (!s_touch) return 0;
  uint8_t n = 0;
  if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return 0;
  uint16_t tx[1], ty[1], st[1];
  if (!esp_lcd_touch_get_coordinates(s_touch, tx, ty, st, &n, 1) || !n) return 0;
  *x = tx[0];
  *y = ty[0];
  return 1;
}
#endif
