idf_component_register(
    SRCS "app_status.c"
         "hello_world.c"
         "network.c"
         "uart_bridge.c"
         "stm32_protocol.c"
         "sd_logger.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES espressif__mqtt fatfs esp_driver_sdspi esp_driver_spi esp_driver_uart esp_event esp_netif esp_wifi nvs_flash
)
