#pragma once
#include "esp_lcd_panel_io.h"
#include "board.h"
extern uint16_t FB[LCD_H][LCD_W];
static inline int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p,int x0,int y0,int x1,int y1,const void*d){(void)p;const uint16_t*s=d;for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){uint16_t c=*s++;FB[y][x]=(uint16_t)((c>>8)|(c<<8));}return 0;}
static inline int esp_lcd_panel_reset(esp_lcd_panel_handle_t p){(void)p;return 0;}
static inline int esp_lcd_panel_init(esp_lcd_panel_handle_t p){(void)p;return 0;}
static inline int esp_lcd_panel_invert_color(esp_lcd_panel_handle_t p,bool b){(void)p;(void)b;return 0;}
static inline int esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t p,bool b){(void)p;(void)b;return 0;}
static inline int esp_lcd_panel_mirror(esp_lcd_panel_handle_t p,bool a,bool b){(void)p;(void)a;(void)b;return 0;}
static inline int esp_lcd_panel_set_gap(esp_lcd_panel_handle_t p,int a,int b){(void)p;(void)a;(void)b;return 0;}
static inline int esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p,bool b){(void)p;(void)b;return 0;}
