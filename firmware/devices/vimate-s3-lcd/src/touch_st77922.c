// Sitronix touch at 0x55, 16-bit register addresses (as Espressif's
// esp_lcd_touch_st7123). Register 0x0010 bit 3 says a report has
// coordinates; the points are 7 bytes each from 0x0014: bit 7 of byte 0 is
// "valid", x is 14 bits in bytes 0-1 (the panel's columns), y in
// bytes 2-3. The whole report has to be read, as Vimate's firmware does, or
// the controller keeps handing back the last one. The bus is shared with the
// board's ES8311 codec (0x18), which this firmware leaves alone.
#include "display.h"
#include "touch.h"

#ifndef BOARD_VIMATE_25

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SDA 38
#define PIN_SCL 39
#define PIN_RST 48
#define ADDR 0x55

static const char *TAG = "touch";
static i2c_master_dev_handle_t dev;
static uint8_t points; // in a report

static esp_err_t read_reg(uint16_t r, uint8_t *buf, size_t len)
{
	uint8_t a[2] = {r >> 8, r & 0xFF};
	return i2c_master_transmit_receive(dev, a, 2, buf, len, 20);
}

bool touch_init(void)
{
	i2c_master_bus_config_t bus_cfg = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = PIN_SDA,
		.scl_io_num = PIN_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t bus;
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = ADDR,
		.scl_speed_hz = 400000,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

	gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT};
	ESP_ERROR_CHECK(gpio_config(&out));
	gpio_set_level(PIN_RST, 0);
	vTaskDelay(pdMS_TO_TICKS(100));
	gpio_set_level(PIN_RST, 1);
	vTaskDelay(pdMS_TO_TICKS(100));

	// register 0x0009: how many points it reports
	if (read_reg(0x0009, &points, 1) != ESP_OK || points == 0 || points > 10) {
		ESP_LOGW(TAG, "no touch at 0x%02x (max points %d)", ADDR, points);
		return false;
	}
	ESP_LOGI(TAG, "ready, %d points", points);
	return true;
}

bool touch_read(int *x, int *y)
{
	uint8_t info = 0, b[7 * 10];
	if (read_reg(0x0010, &info, 1) != ESP_OK || !(info & 0x08))
		return false;
	if (read_reg(0x0014, b, 7 * points) != ESP_OK || !(b[0] & 0x80))
		return false;
	int px = ((b[0] & 0x3F) << 8) | b[1], py = ((b[2] & 0x3F) << 8) | b[3];
	px = px < PANEL_W ? px : PANEL_W - 1;
	py = py < PANEL_H ? py : PANEL_H - 1;
	*x = FLIP ? PANEL_H - 1 - py : py; // panel to screen, see display_flush()
	*y = FLIP ? px : PANEL_W - 1 - px;
	return true;
}

#endif
