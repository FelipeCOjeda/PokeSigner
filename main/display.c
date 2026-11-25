// Minimal text renderer for the ST7789 panel using esp_lcd.
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

#define W CONFIG_PS_LCD_WIDTH
#define H CONFIG_PS_LCD_HEIGHT
#define BAND_MAX 48  // tallest line (scale 3)

extern const uint8_t FONT8X16[95][16];

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_done, s_lock;
static uint16_t s_band[2][W * BAND_MAX];
static int s_cur;

static bool on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

int disp_cols(int scale) { return W / (8 * scale); }

int disp_init(void) {
    s_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    xSemaphoreGive(s_done);

    gpio_config_t bl = {.pin_bit_mask = 1ULL << CONFIG_PS_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(CONFIG_PS_LCD_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_PS_LCD_SCLK, .mosi_io_num = CONFIG_PS_LCD_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = W * BAND_MAX * 2,
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
    esp_lcd_panel_set_gap(s_panel, CONFIG_PS_LCD_X_GAP, CONFIG_PS_LCD_Y_GAP);
    esp_lcd_panel_disp_on_off(s_panel, true);
    disp_page(NULL, 0);
    gpio_set_level(CONFIG_PS_LCD_BL, 1);
    return 0;
}

static inline uint16_t sw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }  // panel is big-endian

static void push_band(int y, int h) {
    xSemaphoreTake(s_done, portMAX_DELAY);  // previous transfer finished
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, W, y + h, s_band[s_cur]);
    s_cur ^= 1;
}

static void draw_line(int y, const disp_line_t *l) {
    int sc = l->scale ? l->scale : 1, h = 16 * sc;
    if (y + h > H) h = H - y;
    uint16_t *b = s_band[s_cur], fg = sw(l->fg), bg = sw(l->bg);
    for (int i = 0; i < W * h; i++) b[i] = bg;
    size_t len = l->text ? strlen(l->text) : 0;
    int cols = disp_cols(sc);
    if ((int)len > cols) len = cols;
    int x0 = l->center ? (W - (int)len * 8 * sc) / 2 : 2;
    for (size_t k = 0; k < len; k++) {
        unsigned char ch = (unsigned char)l->text[k];
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = FONT8X16[ch - 32];
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx))
                    for (int dy = 0; dy < sc; dy++)
                        for (int dx = 0; dx < sc; dx++) {
                            int px = x0 + (int)k * 8 * sc + gx * sc + dx, py = gy * sc + dy;
                            if (px >= 0 && px < W && py < h) b[py * W + px] = fg;
                        }
    }
    push_band(y, h);
}

void disp_page(const disp_line_t *lines, size_t n) {
    if (!s_panel) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int y = 0;
    for (size_t i = 0; i < n && y < H; i++) {
        draw_line(y, &lines[i]);
        y += 16 * (lines[i].scale ? lines[i].scale : 1);
    }
    disp_line_t blank = {"", C_WHITE, C_BLACK, 3, 0};
    while (y < H) { draw_line(y, &blank); y += 48; }
    xSemaphoreTake(s_done, portMAX_DELAY);  // wait last band, keep buffers consistent
    xSemaphoreGive(s_done);
    xSemaphoreGive(s_lock);
}
#endif
