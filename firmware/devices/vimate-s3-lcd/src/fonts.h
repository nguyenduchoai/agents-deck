// Montserrat Medium with Vietnamese (and LVGL's symbols), made by lv_font_conv
// from the files in LVGL's scripts/built_in_font: LVGL's own Montserrat is
// ASCII only. Ranges: Basic Latin, Latin-1, the Vietnamese letters (Ăă Đđ Ĩĩ
// Ũũ Ơơ Ưư, U+1EA0-1EF9) and a few punctuation marks.
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(font_vn_12);
LV_FONT_DECLARE(font_vn_14);
LV_FONT_DECLARE(font_vn_16);
LV_FONT_DECLARE(font_vn_20);
LV_FONT_DECLARE(font_vn_24);
LV_FONT_DECLARE(font_vn_28);

// The sizes the UI asks for; the 320x240 screen gets each one a step smaller.
#ifdef BOARD_VIMATE_25
#define FONT_28 font_vn_20
#define FONT_24 font_vn_16
#define FONT_20 font_vn_14
#define FONT_16 font_vn_12
#define FONT_14 font_vn_12
#else
#define FONT_28 font_vn_28
#define FONT_24 font_vn_24
#define FONT_20 font_vn_20
#define FONT_16 font_vn_16
#define FONT_14 font_vn_14
#endif
