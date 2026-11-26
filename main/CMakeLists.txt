idf_component_register(
    SRCS main.c ble.c ui.c display.c font8x8.c
    INCLUDE_DIRS .
    REQUIRES btcsigner qrcodegen bt nvs_flash esp_lcd esp_driver_spi esp_driver_gpio esp_timer esp_driver_usb_serial_jtag esp_driver_uart bootloader_support esp_hw_support)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
