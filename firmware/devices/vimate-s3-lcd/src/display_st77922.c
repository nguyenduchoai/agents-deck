// The S3 3.5" board's ST77922 over QSPI. A register write is command 0x02 with the register in
// the address phase; pixels go as command 0x32 with address 0x002C00 (RAMWR)
// on all four lines (as Espressif's esp_lcd_st77922). The bus is SPI2 on its
// own IO_MUX pins; the panel has no reset line, so it gets a software reset.
// The init sequence is the one Vimate's own firmware sends to this panel.
#include "display.h"

#ifndef BOARD_VIMATE_25

#include <assert.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PIN_D0 11
#define PIN_D1 13
#define PIN_D2 14
#define PIN_D3 9
#define PIN_SCK 12
#define PIN_CS 10
#define PIN_BL 41 // backlight, PWM, active high
#define SPI_HZ (40 * 1000 * 1000)
#define CHUNK 32768  // bytes per SPI transaction
#define MAX_CHUNKS 4 // per write: 320 x 204 panel pixels
#define STRIP 40     // panel rows per flush write

static const char *TAG = "display";
static spi_device_handle_t spi;
static spi_transaction_ext_t chunks[MAX_CHUNKS];
static int queued; // chunks sent but not collected
static void (*done_fn)(void *);
static void *done_arg;
static uint16_t *strip; // panel rows on their way out, big-endian
static SemaphoreHandle_t sent;

typedef struct {
	uint8_t reg, len;
	uint16_t delay_ms;
	uint8_t data[16];
} init_cmd_t;

static const init_cmd_t INIT[] = {
	{0xF1, 1, 0, {0x00}},
	{0x60, 3, 0, {0x00, 0x00, 0x00}},
	{0x65, 1, 0, {0x80}},
	{0x79, 1, 0, {0x06}},
	{0x7B, 3, 0, {0x00, 0x08, 0x08}},
	{0x80, 11, 0, {0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA}},
	{0x81, 4, 0, {0x26, 0x52, 0x72, 0x27}},
	{0x84, 2, 0, {0x92, 0x25}},
	{0x87, 6, 0, {0x10, 0x10, 0x58, 0x00, 0x02, 0x3A}},
	{0x88, 15, 0, {0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06}},
	{0x89, 3, 0, {0x00, 0x00, 0x00}},
	{0x8A, 11, 0, {0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19}},
	{0x8B, 9, 0, {0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E}},
	{0x8C, 13, 0, {0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09}},
	{0x8D, 1, 0, {0x0C}},
	{0x8E, 6, 0, {0x33, 0x01, 0x0C, 0x13, 0x01, 0x01}},
	{0xB3, 2, 0, {0x00, 0x30}},
	{0xF1, 1, 0, {0x00}},
	{0x71, 1, 0, {0xD0}},
	{0x66, 2, 0, {0x02, 0x3F}},
	{0xBE, 3, 0, {0x26, 0x00, 0x9D}},
	{0x70, 12, 0, {0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A}},
	{0x90, 9, 0, {0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27}},
	{0x91, 9, 0, {0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27}},
	{0x92, 10, 0, {0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27}},
	{0x93, 10, 0, {0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27}},
	{0x94, 6, 0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x95, 5, 0, {0x96, 0x16, 0x00, 0x00, 0xFF}},
	{0x96, 12, 0, {0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44}},
	{0x97, 12, 0, {0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44}},
	{0xBA, 5, 0, {0x55, 0x94, 0x2D, 0x94, 0x27}},
	{0x9A, 7, 0, {0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}},
	{0x9B, 7, 0, {0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}},
	{0x9C, 13, 0, {0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00}},
	{0x9D, 8, 0, {0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}},
	{0x9E, 7, 0, {0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}},
	{0xB4, 12, 0, {0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10}},
	{0xB5, 12, 0, {0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12}},
	{0xB6, 7, 0, {0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF}},
	{0x86, 14, 0, {0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5}},
	{0xB7, 16, 0, {0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D}},
	{0xB8, 16, 0, {0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D}},
	{0xB9, 2, 0, {0x23, 0x23}},
	{0xBF, 6, 0, {0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B}},
	{0xF2, 1, 0, {0x00}},
	{0x73, 5, 0, {0x04, 0xDA, 0x12, 0x54, 0x47}},
	{0x77, 5, 0, {0x6B, 0x5B, 0xFD, 0xC3, 0xC5}},
	{0x7A, 2, 0, {0x15, 0x27}},
	{0x7B, 2, 0, {0x04, 0x57}},
	{0x7E, 2, 0, {0x01, 0x0E}},
	{0xBF, 1, 0, {0x36}},
	{0xE3, 2, 0, {0x40, 0x40}},
	{0xF0, 1, 0, {0x00}},
	{0xD0, 1, 0, {0x00}},
	{0x2A, 4, 0, {0x00, 0x00, 0x01, 0x3F}}, // columns 0-319
	{0x2B, 4, 0, {0x00, 0x00, 0x01, 0xDF}}, // rows 0-479
	{0x21, 0, 0, {0}},                      // inversion on
	{0x11, 0, 120, {0}},                    // sleep out
	{0x29, 0, 0, {0}},                      // display on
	{0x2C, 0, 0, {0}},
	{0x3A, 1, 0, {0x01}}, // 16 bits per pixel, as this panel reads it
	{0x36, 1, 0, {0x00}}, // portrait, RGB
	{0x35, 1, 20, {0x01}}, // tearing effect on
};

// After each chunk (in the SPI interrupt): the last one ends the write.
static void IRAM_ATTR chunk_sent(spi_transaction_t *t)
{
	if ((intptr_t)t->user == 1) {
		gpio_set_level(PIN_CS, 1);
		if (done_fn)
			done_fn(done_arg);
	}
}

// Collects the chunks of the last write, so the queue is free again.
static void collect(void)
{
	spi_transaction_t *t;
	while (queued > 0 && spi_device_get_trans_result(spi, &t, portMAX_DELAY) == ESP_OK)
		queued--;
}

// One register write: command 0x02, register in the address phase.
static void reg(uint8_t r, const uint8_t *data, size_t len)
{
	collect();
	spi_transaction_t t = {
		.cmd = 0x02,
		.addr = (uint32_t)r << 8,
		.length = len * 8,
		.tx_buffer = len ? data : NULL,
	};
	gpio_set_level(PIN_CS, 0);
	ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
	gpio_set_level(PIN_CS, 1);
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
	strip = heap_caps_malloc(PANEL_W * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	sent = xSemaphoreCreateBinary();
	assert(strip && sent);
	backlight_init();
	gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_CS, .mode = GPIO_MODE_OUTPUT};
	ESP_ERROR_CHECK(gpio_config(&out));
	gpio_set_level(PIN_CS, 1);

	spi_bus_config_t bus = {
		.data0_io_num = PIN_D0,
		.data1_io_num = PIN_D1,
		.sclk_io_num = PIN_SCK,
		.data2_io_num = PIN_D2,
		.data3_io_num = PIN_D3,
		.max_transfer_sz = CHUNK,
		.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
	spi_device_interface_config_t dev = {
		.command_bits = 8,
		.address_bits = 24,
		.mode = 0,
		.clock_speed_hz = SPI_HZ,
		.spics_io_num = -1, // CS by hand: a frame is several transactions
		.flags = SPI_DEVICE_HALFDUPLEX,
		.queue_size = MAX_CHUNKS,
		.post_cb = chunk_sent,
	};
	ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));
	int khz = 0;
	spi_device_get_actual_freq(spi, &khz);
	ESP_LOGI(TAG, "QSPI at %d kHz", khz);

	reg(0x01, NULL, 0); // software reset
	vTaskDelay(pdMS_TO_TICKS(120));
	reg(0x36, (const uint8_t[]){0x00}, 1);
	reg(0x3A, (const uint8_t[]){0x55}, 1);
	for (size_t i = 0; i < sizeof INIT / sizeof INIT[0]; i++) {
		reg(INIT[i].reg, INIT[i].data, INIT[i].len);
		if (INIT[i].delay_ms)
			vTaskDelay(pdMS_TO_TICKS(INIT[i].delay_ms));
	}
	ESP_LOGI(TAG, "ST77922 ready");
}

void display_brightness(uint8_t level)
{
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Writes panel pixels x1..x2, y1..y2 by DMA; done(arg) runs (from an
// interrupt) once they're out.
static void display_draw(int x1, int y1, int x2, int y2, const uint16_t *pixels, void (*done)(void *), void *arg)
{
	collect();
	uint8_t caset[4] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
	uint8_t raset[4] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
	reg(0x2A, caset, 4);
	reg(0x2B, raset, 4);

	size_t left = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * 2;
	const uint8_t *p = (const uint8_t *)pixels;
	done_fn = done;
	done_arg = arg;
	gpio_set_level(PIN_CS, 0);
	for (int i = 0; left > 0 && i < MAX_CHUNKS; i++) {
		size_t n = left > CHUNK ? CHUNK : left;
		spi_transaction_ext_t *t = &chunks[i];
		memset(t, 0, sizeof *t);
		t->base.flags = SPI_TRANS_MODE_QIO;
		if (i == 0) {
			t->base.cmd = 0x32;
			t->base.addr = 0x002C00; // RAMWR
		} else { // the rest of the same write: data only
			t->base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
		}
		t->base.tx_buffer = p;
		t->base.length = n * 8;
		p += n;
		left -= n;
		t->base.user = (void *)(intptr_t)(left == 0);
		ESP_ERROR_CHECK(spi_device_queue_trans(spi, &t->base, portMAX_DELAY));
		queued++;
	}
}

static void IRAM_ATTR strip_sent(void *arg)
{
	(void)arg;
	BaseType_t woken = pdFALSE;
	xSemaphoreGiveFromISR(sent, &woken);
	portYIELD_FROM_ISR(woken);
}

// The screen is landscape, the panel portrait: screen column x is panel
// row x and screen row y is panel column PANEL_W-1-y (both mirrored with
// FLIP). The panel garbles windows narrower than a full row, so whatever
// changed goes out as whole panel rows, read from the framebuffer.
void display_flush(const uint16_t *fb, int x1, int y1, int x2, int y2)
{
	(void)y1, (void)y2;
	for (int c0 = x1; c0 <= x2; c0 += STRIP) {
		int c1 = c0 + STRIP - 1 > x2 ? x2 : c0 + STRIP - 1;
		uint16_t *o = strip;
		for (int x = c0; x <= c1; x++)
			for (int c = 0; c < PANEL_W; c++) {
				uint16_t v = FLIP ? fb[c * DISPLAY_W + DISPLAY_W - 1 - x] : fb[(PANEL_W - 1 - c) * DISPLAY_W + x];
				*o++ = (uint16_t)(v << 8 | v >> 8);
			}
		int r0 = FLIP ? PANEL_H - 1 - c1 : c0;
		if (FLIP) { // rows went out bottom-up: reverse them
			int n = c1 - c0 + 1;
			for (int i = 0; i < n / 2; i++)
				for (int c = 0; c < PANEL_W; c++) {
					uint16_t t = strip[i * PANEL_W + c];
					strip[i * PANEL_W + c] = strip[(n - 1 - i) * PANEL_W + c];
					strip[(n - 1 - i) * PANEL_W + c] = t;
				}
		}
		display_draw(0, r0, PANEL_W - 1, r0 + (c1 - c0), strip, strip_sent, NULL);
		xSemaphoreTake(sent, portMAX_DELAY);
	}
}

#endif
