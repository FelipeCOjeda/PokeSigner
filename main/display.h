#pragma once
// Tile-based display in the spirit of 8-bit handhelds: a grid of 8x8 tiles
// (20x10 at 2x on the C6 320x172 panel, 16x16 at 1x on the C3 128x128 panel).
#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "board.h"

// palette indices
enum { P_WHITE, P_LAV, P_PURPLE, P_INK, P_RED, P_GREEN, P_YELLOW, P_BLUE, P_COUNT };

// special tiles (control codes inside the text grid)
#define T_TL   "\x01"
#define T_TR   "\x02"
#define T_BL   "\x03"
#define T_BR   "\x04"
#define T_HT   "\x05"   // top edge
#define T_HB   "\x06"   // bottom edge
#define T_VL   "\x07"
#define T_VR   "\x0e"
#define T_ARROW "\x10"  // right-pointing cursor
#define T_DOWN  "\x11"  // "more" prompt
#define T_DOTS  "\x12"  // background pattern
#define T_BTC   "\x13"  // bitcoin sign

typedef struct {
    char    ch[GH][GW];
    uint8_t fg[GH][GW];
    uint8_t bg[GH][GW];
    uint8_t big[GH];    // 1 = render this row's interior at 4x, spanning 2 rows
} grid_t;

// Draws a 16x16 sprite (rows of palette letters, '.' = background) at pixel x,y with
// integer scale, inside a box of w x h pixels that is first filled with the background.
// sparkle != 0 adds little stars around it (alternating sets for odd/even values).
#if CONFIG_PS_DISPLAY_ST7789
void disp_sprite(const char *const rows[16], int box_x, int box_y, int box_w, int box_h,
                 int dx, int dy, int scale, int sparkle, int bg_pal);
int  disp_init(uint8_t orientation);
void disp_grid(const grid_t *g);
void disp_set_orientation(uint8_t o);   // 0..3, landscape variants
#else
static inline int  disp_init(uint8_t o) { (void)o; return -1; }
static inline void disp_grid(const grid_t *g) { (void)g; }
static inline void disp_set_orientation(uint8_t o) { (void)o; }
static inline void disp_sprite(const char *const r[16], int a, int b, int c, int d, int e, int f, int g, int h, int i)
{ (void)r; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; }
#endif
