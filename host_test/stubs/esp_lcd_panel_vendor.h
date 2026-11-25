#pragma once
#include "esp_lcd_panel_io.h"
typedef struct { int reset_gpio_num, rgb_ele_order, bits_per_pixel; } esp_lcd_panel_dev_config_t;
#define LCD_RGB_ELEMENT_ORDER_RGB 0
#define LCD_RGB_ELEMENT_ORDER_BGR 1
static inline int esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io, esp_lcd_panel_dev_config_t*c, esp_lcd_panel_handle_t*p){(void)io;(void)c;*p=(void*)1;return 0;}
