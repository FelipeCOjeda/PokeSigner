#pragma once
// Per-board geometry for the tile renderer.
#include "sdkconfig.h"

#if CONFIG_PS_BOARD_C3_SPOTPEAR_144
// 128x128 ST7735: 16x16 tiles of 8x8 drawn at 1x
#define LCD_W   128
#define LCD_H   128
#define GW      16
#define GH      16
#define TSC     1      // tile scale
#define BIGSC   2      // scale of "big" rows (spans 2 grid rows)
#define LCD_ST7735 1
#define BOARD_NAME "ESP32-C3 1.44"
// mascot placement: idle (inside the box, white bg), splash, celebration area
#define IDLE_SPR_SC 2
#define IDLE_SPR_X  48
#define IDLE_SPR_Y  84
#define SPLASH_SC   3
#define SPLASH_Y    40
#define CELEB_SC    3
#define MASCOT_ANIM 0          // static mascot on the C3
#else
// 320x172 ST7789 landscape: 20x10 tiles of 8x8 drawn at 2x
#define LCD_W   320
#define LCD_H   172
#define GW      20
#define GH      10
#define TSC     2
#define BIGSC   4
#define LCD_ST7735 0
#define BOARD_NAME "ESP32-C6 1.47"
#define IDLE_SPR_SC 4
#define IDLE_SPR_X  228
#define IDLE_SPR_Y  (TOPM + 40)
#define SPLASH_SC   4
#define SPLASH_Y    (TOPM + 62)
#define CELEB_SC    4
#define MASCOT_ANIM 1
#endif

#define TW      (GW - 2)                 // text columns inside a box
#define TOPM    ((LCD_H - GH * 8 * TSC) / 2)
