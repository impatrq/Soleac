idf_component_register(
    SRCS "mems_test.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_gpio
)
