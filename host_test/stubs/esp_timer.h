#pragma once
#include <stdint.h>
static int64_t g_fake_t;
static inline int64_t esp_timer_get_time(void){ g_fake_t += 100000; return g_fake_t; }
