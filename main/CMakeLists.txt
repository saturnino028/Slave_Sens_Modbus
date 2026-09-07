idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp-modbus driver esp_driver_uart esp_driver_i2c
                       PRIV_REQUIRES spi_flash)