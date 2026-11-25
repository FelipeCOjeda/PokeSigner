#pragma once
typedef struct { int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3
#define ESP_OK 0
static inline int spi_bus_initialize(int h, spi_bus_config_t*c,int d){(void)h;(void)c;(void)d;return 0;}
