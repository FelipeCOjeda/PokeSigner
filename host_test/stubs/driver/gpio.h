#pragma once
#include <stdint.h>
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en; } gpio_config_t;
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
static int g_fake_n;
static inline int gpio_config(gpio_config_t *c){(void)c;return 0;}
// simulated short presses: pressed for 2 polls every 6 polls
static inline int gpio_get_level(int p){(void)p; return (++g_fake_n % 6) < 2 ? 0 : 1;}
