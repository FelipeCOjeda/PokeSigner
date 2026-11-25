#pragma once
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(int n,int s){(void)n;(void)s;return 0;}
static inline int xQueueSend(QueueHandle_t q,const void*v,int t){(void)q;(void)v;(void)t;return 0;}
static inline int xQueueReceive(QueueHandle_t q,void*v,int t){(void)q;(void)v;(void)t;return 0;}
static inline void xQueueReset(QueueHandle_t q){(void)q;}
