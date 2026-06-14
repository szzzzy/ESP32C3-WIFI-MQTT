idf_component_register(
    SRCS "app_status.c"
         "hello_world.c"
         "network.c"
         "uart_bridge.c"
         "usb_transport.c"
         "stm32_protocol.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES espressif__mqtt esp_driver_spi esp_driver_uart esp_driver_usb_serial_jtag esp_event esp_netif esp_wifi nvs_flash
)
