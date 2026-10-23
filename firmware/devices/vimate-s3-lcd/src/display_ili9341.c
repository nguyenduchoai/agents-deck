// The S3 2.5" board's ILI9341 on SPI3, D/C on its own pin, set up as Vimate's
// firmware does: Espressif's esp_lcd_ili9341 init sequence, BGR and inversion, but
// upright: the panel's own 240x320 portrait (Vimate turns it to landscape). There's no
// reset line, so it gets a software reset.
#include "display.h"

#ifdef BOARD_VIMATE_25

#include <assert.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_MOSI 11
#define PIN_SCK 12
#define PIN_CS 10
#define PIN_DC 46
#define PIN_BL 45 // backlight, PWM, active high
#define SPI_HZ (40 * 1000 * 1000)
#define STRIP 40 // screen rows per write

static const char *TAG = "display";
static spi_device_handle_t spi;
static uint16_t *strip; // rows on their way out, big-endian

typedef struct {
	uint8_t reg, len;
	uint16_t delay_ms;
	uint8_t data[15];
} init_cmd_t;

static const init_cmd_t INIT[] = {
	{0x11, 0, 100, {0}},   // sleep out
	{0x36, 1, 0, {0x48}},  // portrait (MX, else it's mirrored), BGR
	{0x3A, 1, 0, {0x55}},  // 16 bits per pixel
	{0xCF, 3, 0, {0x00, 0xAA, 0xE0}},
	{0xED, 4, 0, {0x67, 0x03, 0x12, 0x81}},
	{0xE8, 3, 0, {0x8A, 0x01, 0x78}},
	{0xCB, 5, 0, {0x39, 0x2C, 0x00, 0x34, 0x02}},
	{0xF7, 1, 0, {0x20}},
	{0xEA, 2, 0, {0x00, 0x00}},
	{0xC0, 1, 0, {0x23}},
	{0xC1, 1, 0, {0x11}},
	{0xC5, 2, 0, {0x43, 0x4C}},
	{0xC7, 1, 0, {0xA0}},
	{0xB1, 2, 0, {0x00, 0x1B}},
	{0xF2, 1, 0, {0x00}},
	{0x26, 1, 0, {0x01}},
	{0xE0, 15, 0, {0x1F, 0x36, 0x36, 0x3A, 0x0C, 0x05, 0x4F, 0x87, 0x3C, 0x08, 0x11, 0x35, 0x19, 0x13, 0x00}},
	{0xE1, 15, 0, {0x00, 0x09, 0x09, 0x05, 0x13, 0x0A, 0x30, 0x78, 0x43, 0x07, 0x0E, 0x0A, 0x26, 0x2C, 0x1F}},
	{0xB7, 1, 0, {0x07}},
	{0xB6, 3, 0, {0x08, 0x82, 0x27}},
	{0x21, 0, 0, {0}}, // inversion on: an IPS panel, colours come out negative without it
	{0x29, 0, 0, {0}}, // display on
};

// D/C before each transaction: low for a command byte, high for its data.
static void IRAM_ATTR set_dc(spi_transaction_t *t) { gpio_set_level(PIN_DC, (int)(intptr_t)t->user); }

static void send(int dc, const void *data, size_t len)
{
	if (!len)
		return;
	spi_transaction_t t = {.length = len * 8, .tx_buffer = data, .user = (void *)(intptr_t)dc};
	ESP_ERROR_CHECK(spi_device_transmit(spi, &t));
}

static void reg(uint8_t r, const uint8_t *data, size_t len)
{
	send(0, &r, 1);
	send(1, data, len);
}

static void backlight_init(void)
{
	ledc_timer_config_t timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = LEDC_TIMER_8_BIT,
		.timer_num = LEDC_TIMER_0,
		.freq_hz = 20000,
		.clk_cfg = LEDC_AUTO_CLK,
	};
	ESP_ERROR_CHECK(ledc_timer_config(&timer));
	ledc_channel_config_t ch = {
		.gpio_num = PIN_BL,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = LEDC_CHANNEL_0,
		.timer_sel = LEDC_TIMER_0,
		.duty = 0, // dark until the first frame
	};
	ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void display_init(void)
{
	backlight_init();
	strip = heap_caps_malloc(DISPLAY_W * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	assert(strip);
	gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_DC, .mode = GPIO_MODE_OUTPUT};
	ESP_ERROR_CHECK(gpio_config(&out));

	spi_bus_config_t bus = {
		.mosi_io_num = PIN_MOSI,
		.miso_io_num = -1,
		.sclk_io_num = PIN_SCK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = DISPLAY_W * STRIP * 2,
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
	spi_device_interface_config_t dev = {
		.mode = 0,
		.clock_speed_hz = SPI_HZ,
		.spics_io_num = PIN_CS,
		.queue_size = 2,
		.pre_cb = set_dc,
	};
	ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &spi));

	reg(0x01, NULL, 0); // software reset
	vTaskDelay(pdMS_TO_TICKS(120));
	for (size_t i = 0; i < sizeof INIT / sizeof INIT[0]; i++) {
		reg(INIT[i].reg, INIT[i].data, INIT[i].len);
		if (INIT[i].delay_ms)
			vTaskDelay(pdMS_TO_TICKS(INIT[i].delay_ms));
	}
	ESP_LOGI(TAG, "ILI9341 ready");
}

void display_brightness(uint8_t level)
{
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_flush(const uint16_t *fb, int x1, int y1, int x2, int y2)
{
	int w = x2 - x1 + 1;
	reg(0x2A, (const uint8_t[]){x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF}, 4);
	reg(0x2B, (const uint8_t[]){y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF}, 4);
	send(0, (const uint8_t[]){0x2C}, 1); // RAMWR, then the rows
	for (int r0 = y1; r0 <= y2; r0 += STRIP) {
		int rows = y2 - r0 + 1 < STRIP ? y2 - r0 + 1 : STRIP;
		uint16_t *o = strip;
		for (int y = r0; y < r0 + rows; y++)
			for (int x = x1; x <= x2; x++) {
				uint16_t v = fb[y * DISPLAY_W + x];
				*o++ = (uint16_t)(v << 8 | v >> 8);
			}
		send(1, strip, (size_t)w * rows * 2);
	}
}

#endif
