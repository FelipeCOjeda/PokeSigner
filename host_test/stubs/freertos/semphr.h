#pragma once
typedef int *SemaphoreHandle_t; typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY 0
static int g_sem;
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void){return &g_sem;}
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return &g_sem;}
static inline int xSemaphoreTake(SemaphoreHandle_t s,int t){(void)s;(void)t;return 1;}
static inline int xSemaphoreGive(SemaphoreHandle_t s){(void)s;return 1;}
static inline int xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t*h){(void)s;(void)h;return 1;}
