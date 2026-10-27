idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES idf-SSD1306 oralb_proto oralb_ble
                       PRIV_REQUIRES spi_flash nvs_flash)
