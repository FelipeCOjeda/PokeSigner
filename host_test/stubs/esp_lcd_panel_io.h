#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef void *esp_lcd_panel_io_handle_t; typedef void *esp_lcd_panel_handle_t; typedef int esp_lcd_spi_bus_handle_t;
typedef struct { int x; } esp_lcd_panel_io_event_data_t;
typedef struct { int dc_gpio_num, cs_gpio_num, pclk_hz, lcd_cmd_bits, lcd_param_bits, spi_mode, trans_queue_depth; void *on_color_trans_done; } esp_lcd_panel_io_spi_config_t;
static inline int esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t b, esp_lcd_panel_io_spi_config_t*c, esp_lcd_panel_io_handle_t*io){(void)b;(void)c;*io=(void*)1;return 0;}
