// ST7789 tile renderer (esp_lcd). Landscape 320x172, 20x10 tiles of 16x16.
#include "display.h"
#if CONFIG_PS_DISPLAY_ST7789
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#define W 320
#define H 172
#define TOP 6          // (172 - 160) / 2
#define BAND 32

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

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_done, s_lock;
static uint16_t s_band[2][W * BAND];
static int s_cur;

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

static void push(int y, int h) {
    xSemaphoreTake(s_done, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, W, y + h, s_band[s_cur]);
    s_cur ^= 1;
}

// Draws one tile at scale `sc` into the current band at pixel x, row offset oy.
static void put_tile(uint16_t *b, int x, int oy, int bh, unsigned char c, int sc, uint16_t fg, uint16_t bg, int ysub) {
    const uint8_t *g = glyph(c);
    for (int py = 0; py < 8 * 2 && oy + py < bh; py++) {      // 16 px per band row
        int gy = (py + ysub) / sc;
        uint8_t bits = gy < 8 ? g[gy] : 0;
        uint16_t *row = b + (oy + py) * W;
        for (int px = 0; px < 8 * sc && x + px < W; px++) {
            int gx = px / sc;
            row[x + px] = (bits & (0x80 >> gx)) ? fg : bg;
        }
    }
}

static void fill_margin(int y, int h) {
    uint16_t *b = s_band[s_cur], c = sw(PAL[P_LAV]);
    for (int i = 0; i < W * h; i++) b[i] = c;
    push(y, h);
}

void disp_grid(const grid_t *g) {
    if (!s_panel) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fill_margin(0, TOP);
    for (int r = 0; r < GH; r++) {
        uint16_t *b = s_band[s_cur];
        int big = g->big[r] && r + 1 < GH;
        for (int sub = 0; sub < (big ? 2 : 1); sub++) {
            b = s_band[s_cur];
            int rr = r + sub;
            // border / normal cells
            for (int c = 0; c < GW; c++) {
                if (big && c > 0 && c < GW - 1) continue;
                put_tile(b, c * 16, 0, 16, (unsigned char)g->ch[rr][c], 2,
                         sw(PAL[g->fg[rr][c]]), sw(PAL[g->bg[rr][c]]), 0);
            }
            if (big) {
                // interior: up to 9 chars at 4x, centered in 288 px
                uint16_t bg = sw(PAL[g->bg[r][1]]), fg = sw(PAL[g->fg[r][1]]);
                for (int i = 0; i < 16 * W; i++) if ((i % W) >= 16 && (i % W) < W - 16) b[i] = bg;
                char t[GW + 1]; int n = 0;
                for (int c = 1; c < GW - 1; c++) t[n++] = g->ch[r][c];
                while (n && t[n - 1] == ' ') n--;
                int s = 0; while (s < n && t[s] == ' ') s++;
                int len = n - s; if (len > 9) len = 9;
                int x0 = 16 + (288 - len * 32) / 2;
                for (int k = 0; k < len; k++)
                    put_tile(b, x0 + k * 32, 0, 16, (unsigned char)t[s + k], 4, fg, bg, sub * 16);
            }
            push(TOP + rr * 16, 16);
        }
        if (big) r++;
    }
    fill_margin(TOP + GH * 16, H - TOP - GH * 16);
    xSemaphoreTake(s_done, portMAX_DELAY);
    xSemaphoreGive(s_done);
    xSemaphoreGive(s_lock);
}

void disp_set_orientation(uint8_t o) {
    if (!s_panel) return;
    static const bool MX[4] = {true, false, false, true}, MY[4] = {false, true, false, true};
    o &= 3;
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, MX[o], MY[o]);
    esp_lcd_panel_set_gap(s_panel, CONFIG_PS_LCD_Y_GAP, CONFIG_PS_LCD_X_GAP);
}

int disp_init(uint8_t orientation) {
    s_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    xSemaphoreGive(s_done);
    gpio_config_t bl = {.pin_bit_mask = 1ULL << CONFIG_PS_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(CONFIG_PS_LCD_BL, 0);
    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_PS_LCD_SCLK, .mosi_io_num = CONFIG_PS_LCD_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = W * BAND * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return -1;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = CONFIG_PS_LCD_DC, .cs_gpio_num = CONFIG_PS_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 4, .on_color_trans_done = on_done,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io) != ESP_OK) return -1;
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = CONFIG_PS_LCD_RST,
#if CONFIG_PS_LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &pc, &s_panel) != ESP_OK) return -1;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
#if CONFIG_PS_LCD_INVERT
    esp_lcd_panel_invert_color(s_panel, true);
#endif
    disp_set_orientation(orientation);
    esp_lcd_panel_disp_on_off(s_panel, true);
    gpio_set_level(CONFIG_PS_LCD_BL, 1);
    return 0;
}
#endif
