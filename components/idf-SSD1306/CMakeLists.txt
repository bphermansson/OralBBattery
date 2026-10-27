set(srcs "ssd1306.c")

idf_component_register(
    SRCS "${srcs}"
    REQUIRES esp_driver_i2c
    INCLUDE_DIRS "."
)