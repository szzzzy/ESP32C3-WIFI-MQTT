idf_component_register(
    SRCS "app_status.c"
         "esp_status_report.c"
         "app_main.c"
         "line_reader.c"
         "network.c"
         "uart_bridge.c"
         "usb_transport.c"
         "stm32_protocol.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES cjson espressif__mqtt esp_driver_spi esp_driver_uart esp_driver_usb_serial_jtag esp_event esp_netif esp_timer esp_wifi nvs_flash
)
