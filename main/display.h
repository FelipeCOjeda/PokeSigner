#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"

// RGB565 colors
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GRAY   0x8410
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_YELLOW 0xFFE0
#define C_CYAN   0x07FF
#define C_ORANGE 0xFD20

typedef struct {
    const char *text;
    uint16_t fg, bg;
    uint8_t scale;      // 1 = 8x16, 2 = 16x32, 3 = 24x48
    uint8_t center;
} disp_line_t;

#if CONFIG_PS_DISPLAY_ST7789
int  disp_init(void);
// Draws lines top to bottom; the rest of the screen is cleared.
void disp_page(const disp_line_t *lines, size_t n);
int  disp_cols(int scale);
#else
static inline int  disp_init(void) { return -1; }
static inline void disp_page(const disp_line_t *l, size_t n) { (void)l; (void)n; }
static inline int  disp_cols(int scale) { return 21 / (scale ? scale : 1); }
#endif
