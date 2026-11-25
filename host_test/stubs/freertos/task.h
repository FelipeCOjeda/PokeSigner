#pragma once
void on_delay(int ms);
static inline void vTaskDelay(int x){ on_delay(x); }
static inline int xTaskCreate(void (*f)(void*), const char*n, int s, void*a, int p, void*h){(void)f;(void)n;(void)s;(void)a;(void)p;(void)h;return 1;}
