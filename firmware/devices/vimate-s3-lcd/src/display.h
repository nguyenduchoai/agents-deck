// The screen, as LVGL and the deck see it. Two Vimate boards, picked at build
// time (platformio.ini):
// - S3 3.5" (default): an ST77922 320x480 portrait panel that the case holds
//   sideways (display_st77922.c, touch_st77922.c);
// - S3 2.5" (-DBOARD_VIMATE_25): an ILI9341 240x320 panel, used upright
//   (display_ili9341.c, touch_ft6236.c).
#pragma once

#include <stdint.h>

#ifdef BOARD_VIMATE_25
#define BOARD_TYPE "vimate-s3-25lcd"
#define DISPLAY_W 240
#define DISPLAY_H 320
#else
#define BOARD_TYPE "vimate-s3-35lcd"
#define DISPLAY_W 480
#define DISPLAY_H 320
#define PANEL_W 320
#define PANEL_H 480
// Which way up: 0 has the USB port on the right, 1 on the left.
#define FLIP 0
#endif

// Starts the panel (backlight off) and runs its init sequence.
void display_init(void);

// Sets the backlight's brightness (0-255).
void display_brightness(uint8_t level);

// Sends the screen rectangle x1..x2, y1..y2 (inclusive) of fb, the whole
// screen as LVGL draws it (RGB565, DISPLAY_W wide), to the panel; returns once
// it's out.
void display_flush(const uint16_t *fb, int x1, int y1, int x2, int y2);
