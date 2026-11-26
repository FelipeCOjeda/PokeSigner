// Tile renderer over esp_lcd SPI. Two panels:
//  - C6 Waveshare 1.47: ST7789 via the esp_lcd vendor driver, 320x172 landscape, tiles at 2x
//  - C3 Spotpear 1.44: ST7735S with a small built-in init sequence, 128x128, tiles at 1x
#include "display.h"
#if CONFIG_PS_DISPLAY_ST7789
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include <stdlib.h>

#if CONFIG_PS_LCD_BGR
#define BGR_BIT 0x08
#else
#define BGR_BIT 0x00
#endif
#if CONFIG_PS_LCD_INVERT
#define INV_CMD 0x21
#else
#define INV_CMD 0x20
#endif

#define W LCD_W
#define H LCD_H
#define ROWPX TPX                  // pixel height of one grid row

extern const uint8_t FONT8X8[95][8];

// RGB565, palette inspired by the lavender tones of 1990s handheld color screens
static const uint16_t PAL[P_COUNT] = {
    0xFFDF,  // white   F8F8F8
    0xCE1E,  // lav     C8C0F0
    0x7318,  // purple  7060C0
    0x2108,  // ink     202040
    0xD986,  // red     D83030
    0x3508,  // green   30A040
    0xFE40,  // yellow  F8C800
    0x3298,  // blue    3050C0
};

static const uint8_t TILES[32][8] = {
    [0x01] = {0xE0, 0xFF, 0xE0, 0x5F, 0x50, 0x50, 0x50, 0x50},  // TL
    [0x02] = {0x07, 0xFF, 0x07, 0xFA, 0x0A, 0x0A, 0x0A, 0x0A},  // TR
    [0x03] = {0x50, 0x50, 0x50, 0x50, 0x5F, 0xE0, 0xFF, 0xE0},  // BL
    [0x04] = {0x0A, 0x0A, 0x0A, 0x0A, 0xFA, 0x07, 0xFF, 0x07},  // BR
    [0x05] = {0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00},  // top edge
    [0x06] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00},  // bottom edge
    [0x07] = {0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50},  // left edge
    [0x0e] = {0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A},  // right edge
    [0x10] = {0x00, 0x60, 0x70, 0x78, 0x7C, 0x78, 0x70, 0x60},  // arrow
    [0x11] = {0x00, 0x00, 0xFE, 0x7C, 0x38, 0x10, 0x00, 0x00},  // down
    [0x12] = {0x00, 0x22, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00},  // dots
    [0x13] = {0x28, 0x7C, 0x62, 0x7C, 0x62, 0x62, 0x7C, 0x28},  // bitcoin sign
};

static esp_lcd_panel_io_handle_t s_io;
#if !LCD_ST7735
static esp_lcd_panel_handle_t s_panel;   // ST7789 only
#endif
static SemaphoreHandle_t s_done, s_lock;
static uint16_t s_band[2][W * ROWPX];
static int s_cur, s_ready;
#if LCD_ST7735
static int s_xoff, s_yoff;               // ST7735 RAM offsets for the current orientation
#endif

static bool on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

static inline uint16_t sw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static const uint8_t *glyph(unsigned char c) {
    static const uint8_t blank[8];
    if (c < 32) return TILES[c];
    if (c > 126) return blank;
    return FONT8X8[c - 32];
}

/* ------------------------------------------------------------ panel access */

#if LCD_ST7735
static void tx(uint8_t cmd, const uint8_t *d, size_t n) { esp_lcd_panel_io_tx_param(s_io, cmd, d, n); }

static void st7735_window(int x0, int y0, int x1, int y1) {   // inclusive
    uint8_t ca[4] = {0, (uint8_t)(x0 + s_xoff), 0, (uint8_t)(x1 + s_xoff)};
    uint8_t ra[4] = {0, (uint8_t)(y0 + s_yoff), 0, (uint8_t)(y1 + s_yoff)};
    tx(0x2A, ca, 4);
    tx(0x2B, ra, 4);
}
#endif

static void push(int y, int h) {
    xSemaphoreTake(s_done, portMAX_DELAY);  // previous transfer finished
#if LCD_ST7735
    st7735_window(0, y, W - 1, y + h - 1);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, s_band[s_cur], (size_t)W * h * 2);
#else
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, W, y + h, s_band[s_cur]);
#endif
    s_cur ^= 1;
}

/* ------------------------------------------------------------ rendering */

// Draws `rows` pixel rows of a tile scaled to sz x sz pixels (sz = 8 * scale; any
// size, e.g. 12 = 1.5x) into band b at x, starting at tile pixel row ysub.
static void put_tile(uint16_t *b, int x, unsigned char c, int sz, uint16_t fg, uint16_t bg, int ysub, int rows) {
    const uint8_t *g = glyph(c);
    for (int py = 0; py < rows; py++) {
        int gy = (py + ysub) * 8 / sz;
        uint8_t bits = gy < 8 ? g[gy] : 0;
        uint16_t *row = b + py * W;
        for (int px = 0; px < sz && x + px < W; px++)
            row[x + px] = (bits & (0x80 >> (px * 8 / sz))) ? fg : bg;
    }
}

static void fill_margin(int y, int h) {
    if (h <= 0) return;
    while (h > 0) {
        int n = h > ROWPX ? ROWPX : h;
        uint16_t *b = s_band[s_cur], c = sw(PAL[P_LAV]);
        for (int i = 0; i < W * n; i++) b[i] = c;
        push(y, n);
        y += n; h -= n;
    }
}

void disp_grid(const grid_t *g) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fill_margin(0, TOPM);
    for (int r = 0; r < GH; r++) {
        int big = g->big[r] && r + 1 < GH;
        for (int sub = 0; sub < (big ? 2 : 1); sub++) {
            uint16_t *b = s_band[s_cur];
            int rr = r + sub;
            for (int c = 0; c < GW; c++) {
                if (big && c > 0 && c < GW - 1) continue;
                put_tile(b, c * ROWPX, (unsigned char)g->ch[rr][c], TPX,
                         sw(PAL[g->fg[rr][c]]), sw(PAL[g->bg[rr][c]]), 0, ROWPX);
            }
            if (big) {
                // interior: text at BIGSC, centered between the two border columns
                uint16_t bg = sw(PAL[g->bg[r][1]]), fg = sw(PAL[g->fg[r][1]]);
                int inner = W - 2 * ROWPX, cw = BIGPX;
                for (int y = 0; y < ROWPX; y++)
                    for (int x = ROWPX; x < W - ROWPX; x++) b[y * W + x] = bg;
                char t[GW + 1]; int n = 0;
                for (int c = 1; c < GW - 1; c++) t[n++] = g->ch[r][c];
                while (n && t[n - 1] == ' ') n--;
                int s = 0; while (s < n && t[s] == ' ') s++;
                int len = n - s; if (len > inner / cw) len = inner / cw;
                int x0 = ROWPX + (inner - len * cw) / 2;
                for (int k = 0; k < len; k++)
                    put_tile(b, x0 + k * cw, (unsigned char)t[s + k], BIGPX, fg, bg, sub * ROWPX, ROWPX);
            }
            push(TOPM + rr * ROWPX, ROWPX);
        }
        if (big) r++;
    }
    fill_margin(TOPM + GH * ROWPX, H - TOPM - GH * ROWPX);
    xSemaphoreTake(s_done, portMAX_DELAY);
    xSemaphoreGive(s_done);
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------ sprites */

static uint16_t sprite_color(char c) {
    switch (c) {
    case 'K': return 0x2108;   // outline (ink)
    case 'O': return 0xF4A3;   // bitcoin orange F7931A
    case 'L': return 0xFE0E;   // highlight FFC070
    case 'W': return 0xFFFF;   // eye white
    case 'Y': return 0xFE40;   // spark yellow
    case 'R': return 0xDA06;   // red D84030
    case 'S': return 0xCE59;   // silver C8C8D0
    case 'D': return 0x630E;   // dark grey 606070
    case 'T': return 0x8E98;   // screen teal 8BD3C7
    default:  return PAL[P_LAV];
    }
}

void disp_sprite(const char *const rows[16], int bx, int by, int bw, int bh,
                 int dx, int dy, int sc, int sparkle, int bg_pal) {
    if (!s_ready || bw <= 0 || bh <= 0) return;
    uint16_t *buf = malloc((size_t)bw * bh * 2);
    if (!buf) return;
    uint16_t bg = sw(PAL[bg_pal]);
    for (int i = 0; i < bw * bh; i++) buf[i] = bg;
    if (bg_pal == P_LAV) {   // same dotted pattern as the background tiles, aligned to the grid
        uint16_t dot = sw(PAL[P_PURPLE]);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                int ty = ((by + y - TOPM) * 8 / TPX) & 7, tx = ((bx + x) * 8 / TPX) & 7;
                if (TILES[0x12][ty] & (0x80 >> tx)) buf[y * bw + x] = dot;
            }
    }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            char c = rows[y][x];
            if (c == '.') continue;
            uint16_t col = sw(sprite_color(c));
            for (int yy = 0; yy < sc; yy++)
                for (int xx = 0; xx < sc; xx++) {
                    int px = dx + x * sc + xx, py = dy + y * sc + yy;
                    if (px >= 0 && px < bw && py >= 0 && py < bh) buf[py * bw + px] = col;
                }
        }
    if (sparkle) {   // little stars around the sprite
        static const int8_t S[6][2] = {{-3, 2}, {18, 3}, {-2, 12}, {19, 11}, {2, -2}, {14, -1}};
        uint16_t y1 = sw(sprite_color('Y'));
        for (int k = 0; k < 6; k++) {
            if ((k + sparkle) % 2) continue;
            int cx = dx + S[k][0] * sc, cy = dy + S[k][1] * sc;
            for (int d = -sc; d <= sc; d++) {
                int pts[2][2] = {{cx + d, cy}, {cx, cy + d}};
                for (int q = 0; q < 2; q++)
                    if (pts[q][0] >= 0 && pts[q][0] < bw && pts[q][1] >= 0 && pts[q][1] < bh)
                        buf[pts[q][1] * bw + pts[q][0]] = y1;
            }
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
#if LCD_ST7735
    st7735_window(bx, by, bx + bw - 1, by + bh - 1);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, buf, (size_t)bw * bh * 2);
#else
    esp_lcd_panel_draw_bitmap(s_panel, bx, by, bx + bw, by + bh, buf);
#endif
    xSemaphoreTake(s_done, portMAX_DELAY);   // wait for the DMA before freeing
    xSemaphoreGive(s_done);
    xSemaphoreGive(s_lock);
    free(buf);
}

/* ------------------------------------------------------------ orientation */

void disp_set_orientation(uint8_t o) {
    if (!s_ready) return;
    o &= 3;
#if LCD_ST7735
    // MADCTL and RAM offsets for a 128x128 window on the 132x162 ST7735S RAM
    static const uint8_t MAD[4] = {0x00, 0x60, 0xC0, 0xA0};
    static const int8_t XO[4] = {2, 1, 2, 3}, YO[4] = {1, 2, 3, 2};
    uint8_t m = MAD[o] | BGR_BIT;
    xSemaphoreTake(s_done, portMAX_DELAY);
    tx(0x36, &m, 1);
    xSemaphoreGive(s_done);
    s_xoff = XO[o]; s_yoff = YO[o];
#else
    static const bool MX[4] = {true, false, false, true}, MY[4] = {false, true, false, true};
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, MX[o], MY[o]);
#if CONFIG_PS_BOARD_TDISPLAY_114
    // 135 px on a 240 px axis: offset is 52 or 53 depending on the column order
    esp_lcd_panel_set_gap(s_panel, CONFIG_PS_LCD_Y_GAP, CONFIG_PS_LCD_X_GAP + (MX[o] ? 1 : 0));
#else
    esp_lcd_panel_set_gap(s_panel, CONFIG_PS_LCD_Y_GAP, CONFIG_PS_LCD_X_GAP);
#endif
#endif
}

/* ------------------------------------------------------------ init */

#if LCD_ST7735
static void st7735_init(void) {
    static const uint8_t frm[] = {0x01, 0x2C, 0x2D}, frm3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    static const uint8_t gp[] = {0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    static const uint8_t gn[] = {0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
    uint8_t v;
    if (CONFIG_PS_LCD_RST >= 0) {
        gpio_set_level(CONFIG_PS_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(CONFIG_PS_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    }
    tx(0x01, NULL, 0); vTaskDelay(pdMS_TO_TICKS(150));        // SWRESET
    tx(0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(255));        // SLPOUT
    tx(0xB1, frm, 3); tx(0xB2, frm, 3); tx(0xB3, frm3, 6);    // frame rate
    v = 0x07; tx(0xB4, &v, 1);                                 // INVCTR
    tx(0xC0, (const uint8_t[]){0xA2, 0x02, 0x84}, 3);          // power
    v = 0xC5; tx(0xC1, &v, 1);
    tx(0xC2, (const uint8_t[]){0x0A, 0x00}, 2);
    tx(0xC3, (const uint8_t[]){0x8A, 0x2A}, 2);
    tx(0xC4, (const uint8_t[]){0x8A, 0xEE}, 2);
    v = 0x0E; tx(0xC5, &v, 1);                                 // VCOM
    tx(INV_CMD, NULL, 0);                                      // inversion
    v = 0x05; tx(0x3A, &v, 1);                                 // 16-bit color
    tx(0xE0, gp, 16); tx(0xE1, gn, 16);                        // gamma
    tx(0x13, NULL, 0); vTaskDelay(pdMS_TO_TICKS(10));          // NORON
    tx(0x29, NULL, 0); vTaskDelay(pdMS_TO_TICKS(100));         // DISPON
}
#endif

int disp_init(uint8_t orientation) {
    s_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    xSemaphoreGive(s_done);
    if (CONFIG_PS_LCD_BL >= 0) {
        gpio_config_t bl = {.pin_bit_mask = 1ULL << (CONFIG_PS_LCD_BL & 63), .mode = GPIO_MODE_OUTPUT};
        gpio_config(&bl);
        gpio_set_level(CONFIG_PS_LCD_BL, 0);
    }
    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_PS_LCD_SCLK, .mosi_io_num = CONFIG_PS_LCD_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = W * ROWPX * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return -1;
    esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = CONFIG_PS_LCD_DC, .cs_gpio_num = CONFIG_PS_LCD_CS,
        .pclk_hz = (LCD_ST7735 ? 27 : 40) * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 4, .on_color_trans_done = on_done,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &s_io) != ESP_OK) return -1;
#if LCD_ST7735
    if (CONFIG_PS_LCD_RST >= 0) {
        gpio_config_t rst = {.pin_bit_mask = 1ULL << (CONFIG_PS_LCD_RST & 63), .mode = GPIO_MODE_OUTPUT};
        gpio_config(&rst);
    }
    st7735_init();
    s_ready = 1;
#else
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = CONFIG_PS_LCD_RST,
#if CONFIG_PS_LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(s_io, &pc, &s_panel) != ESP_OK) return -1;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
#if CONFIG_PS_LCD_INVERT
    esp_lcd_panel_invert_color(s_panel, true);
#endif
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_ready = 1;
#endif
    disp_set_orientation(orientation);
    if (CONFIG_PS_LCD_BL >= 0) gpio_set_level(CONFIG_PS_LCD_BL, 1);
    return 0;
}
#endif
