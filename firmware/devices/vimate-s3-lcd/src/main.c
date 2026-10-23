// vimate-s3-lcd: the agents deck on Vimate's S3 LCD boards (see display.h), over
// Deck Link on its USB port. Two FreeRTOS tasks: Deck Link (link.c) and the
// UI (LVGL, here and in ui.c); they share the agents state (agents.c).
#include <assert.h>

#include "agents.h"
#include "debug.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "lvgl.h"
#include "touch.h"
#include "ui.h"

#define BRIGHTNESS 200
static const char *TAG = "panel";
static bool touch_ok;
static uint16_t *fb; // the whole screen, LVGL draws into it

static uint32_t tick(void) { return now_ms(); }

// LVGL draws into the whole-screen framebuffer (direct mode); the board's
// driver sends what changed.
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
	(void)px;
#ifdef PANEL_DEBUG
	if (lv_display_flush_is_last(disp))
		debug_frame();
#endif
	display_flush(fb, area->x1, area->y1, area->x2, area->y2);
	lv_display_flush_ready(disp);
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	int x, y;
	bool down = touch_ok && touch_read(&x, &y);
#ifdef PANEL_DEBUG
	down = down || debug_touch(&x, &y);
#endif
	if (down) {
		data->point.x = x;
		data->point.y = y;
		data->state = LV_INDEV_STATE_PRESSED;
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
}

#ifdef PANEL_DEBUG
#include "esp_timer.h"

static void indev_note(lv_event_t *e)
{
	lv_event_code_t c = lv_event_get_code(e);
	if (c != LV_EVENT_PRESSED && c != LV_EVENT_RELEASED && c != LV_EVENT_SCROLL_BEGIN && c != LV_EVENT_GESTURE &&
	    c != LV_EVENT_CLICKED)
		return;
	lv_indev_t *in = lv_event_get_target(e);
	lv_point_t p;
	lv_indev_get_point(in, &p);
	debug_note("indev ev%d obj=%p scr=%p at %d,%d", (int)c, (void *)lv_indev_get_active_obj(), (void *)lv_screen_active(),
		   (int)p.x, (int)p.y);
}
#endif

static void ui_task(void *arg)
{
	(void)arg;
	bool lit = false;
	for (;;) {
		lv_lock(); // LVGL's draw threads run alongside
		ui_update();
		lv_unlock();
#ifdef PANEL_DEBUG
		int64_t t0 = esp_timer_get_time();
#endif
		uint32_t wait = lv_timer_handler();
#ifdef PANEL_DEBUG
		debug_busy((uint32_t)(esp_timer_get_time() - t0));
#endif
#ifdef PANEL_DEBUG
		debug_poll();
#endif
		if (!lit) { // light the panel once the first frame is on it
			lv_lock(); // the panel's bus is idle between frames
			display_brightness(BRIGHTNESS);
			lv_unlock();
			lit = true;
		}
		vTaskDelay(pdMS_TO_TICKS(wait < 2 ? 2 : wait > 30 ? 30 : wait));
	}
}

void app_main(void)
{
	agents_init();
	display_init();
	touch_ok = touch_init();

	lv_init();
	lv_tick_set_cb(tick);
	lv_display_t *disp = lv_display_create(DISPLAY_W, DISPLAY_H);
	// Direct mode: LVGL redraws only what changed, in place; flush() sends it.
	size_t size = DISPLAY_W * DISPLAY_H * 2;
	fb = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
	assert(fb);
	lv_display_set_buffers(disp, fb, NULL, size, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush);
	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, read_touch);
#ifdef PANEL_DEBUG
	lv_indev_add_event_cb(indev, indev_note, LV_EVENT_ALL, NULL);
#endif

	ui_init();
	link_start();
	xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 4, NULL, 1);
	ESP_LOGI(TAG, "running (touch %s)", touch_ok ? "ok" : "missing");
}
