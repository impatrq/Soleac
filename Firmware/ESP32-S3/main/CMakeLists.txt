set(soleac_requires)

if(EXISTS "${IDF_PATH}/components/esp_driver_i2s")
    list(APPEND soleac_requires esp_driver_i2s)
elseif(NOT "driver" IN_LIST soleac_requires)
    list(APPEND soleac_requires driver)
endif()

if(EXISTS "${IDF_PATH}/components/esp_driver_gpio")
    list(APPEND soleac_requires esp_driver_gpio)
elseif(NOT "driver" IN_LIST soleac_requires)
    list(APPEND soleac_requires driver)
endif()

if(EXISTS "${IDF_PATH}/components/esp_driver_i2c")
    list(APPEND soleac_requires esp_driver_i2c)
elseif(NOT "driver" IN_LIST soleac_requires)
    list(APPEND soleac_requires driver)
endif()

if(EXISTS "${IDF_PATH}/components/esp_driver_spi")
    list(APPEND soleac_requires esp_driver_spi)
elseif(NOT "driver" IN_LIST soleac_requires)
    list(APPEND soleac_requires driver)
endif()

if(EXISTS "${IDF_PATH}/components/esp_driver_sdmmc")
    list(APPEND soleac_requires esp_driver_sdmmc)
elseif(NOT "driver" IN_LIST soleac_requires)
    list(APPEND soleac_requires driver)
endif()

list(APPEND soleac_requires
    bt
    fatfs
    sdmmc
    esp_lcd
    esp_lcd_axs15231b
    esp_lcd_touch
    espressif__esp-dsp
    nvs_flash
)

idf_component_register(
    SRCS
        "app_main.c"
        "ble_comm.c"
        "fft_processing.c"
        "lcd_control.c"
        "sd_logger.c"
   PRIV_REQUIRES
    esp_adc
        INCLUDE_DIRS "."
    REQUIRES ${soleac_requires}
    )
