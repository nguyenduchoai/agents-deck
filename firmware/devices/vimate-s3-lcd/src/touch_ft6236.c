// The S3 2.5" board's FT6236 at 0x38 (the bus is shared with its ES8311
// codec, left alone). Register 0x02 holds the number of points; point 0 is 4
// bytes from 0x03: x in the low 4 bits of byte 0 and byte 1, y in bytes 2-3,
// in the screen's coordinates (the panel is mirrored by MADCTL MX, the touch
// isn't).
#include "display.h"
#include "touch.h"

#ifdef BOARD_VIMATE_25

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SDA 16
#define PIN_SCL 15
#define PIN_RST 18
#define ADDR 0x38

static const char *TAG = "touch";
static i2c_master_dev_handle_t dev;

static esp_err_t read_reg(uint8_t r, uint8_t *buf, size_t len) { return i2c_master_transmit_receive(dev, &r, 1, buf, len, 20); }

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
	vTaskDelay(pdMS_TO_TICKS(20));
	gpio_set_level(PIN_RST, 1);
	vTaskDelay(pdMS_TO_TICKS(300));

	uint8_t chip = 0; // register 0xA3: the chip's id
	if (read_reg(0xA3, &chip, 1) != ESP_OK) {
		ESP_LOGW(TAG, "no FT6236 at 0x%02x", ADDR);
		return false;
	}
	ESP_LOGI(TAG, "FT6x36 ready (chip 0x%02x)", chip);
	return true;
}

bool touch_read(int *x, int *y)
{
	uint8_t n = 0, b[4];
	if (read_reg(0x02, &n, 1) != ESP_OK || (n & 0x0F) == 0 || (n & 0x0F) > 2)
		return false;
	if (read_reg(0x03, b, sizeof b) != ESP_OK)
		return false;
	int px = ((b[0] & 0x0F) << 8) | b[1], py = ((b[2] & 0x0F) << 8) | b[3];
	*x = px < DISPLAY_W ? px : DISPLAY_W - 1;
	*y = py < DISPLAY_H ? py : DISPLAY_H - 1;
	return true;
}

#endif
