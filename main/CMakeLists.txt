idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." 
    PRIV_REQUIRES usb esp_driver_uart esp_driver_gpio esp_timer esp_wifi nvs_flash esp_netif esp_http_server esp_http_client esp_driver_rmt spi_flash app_update
    )


#littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)