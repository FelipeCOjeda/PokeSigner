#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef void *esp_lcd_panel_io_handle_t; typedef void *esp_lcd_panel_handle_t; typedef int esp_lcd_spi_bus_handle_t;
typedef struct { int x; } esp_lcd_panel_io_event_data_t;
typedef struct { int dc_gpio_num, cs_gpio_num, pclk_hz, lcd_cmd_bits, lcd_param_bits, spi_mode, trans_queue_depth; void *on_color_trans_done; } esp_lcd_panel_io_spi_config_t;
static inline int esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t b, esp_lcd_panel_io_spi_config_t*c, esp_lcd_panel_io_handle_t*io){(void)b;(void)c;*io=(void*)1;return 0;}

#include "board.h"
extern uint16_t FB[LCD_H][LCD_W];
static int g_wx0,g_wy0,g_wx1,g_wy1;
static inline int esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io,int cmd,const void*d,unsigned long n){(void)io;const unsigned char*p=d;
  if(cmd==0x2A&&n==4){g_wx0=p[1]-2;g_wx1=p[3]-2;} if(cmd==0x2B&&n==4){g_wy0=p[1]-1;g_wy1=p[3]-1;} return 0;}
static inline int esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io,int cmd,const void*d,unsigned long n){(void)io;(void)cmd;(void)n;const uint16_t*s=d;
  for(int y=g_wy0;y<=g_wy1;y++)for(int x=g_wx0;x<=g_wx1;x++){uint16_t c=*s++;FB[y][x]=(uint16_t)((c>>8)|(c<<8));}return 0;}
