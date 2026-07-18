idf_component_register(
    SRCS "app_main.c" "board_spotpear.c" "ui_round.c"
         "board_185/I2C_Driver/I2C_Driver.c" "board_185/EXIO/TCA9554PWR.c"
         "board_185/LCD_Driver/esp_lcd_st77916/esp_lcd_st77916.c" "board_185/LCD_Driver/ST77916.c"
         "board_185/Touch_Driver/esp_lcd_touch/esp_lcd_touch.c" "board_185/Touch_Driver/CST816.c"
         "board_185/LVGL_Driver/LVGL_Driver.c"
         "board_185/PCF85063/PCF85063.c" "board_185/BAT_Driver/BAT_Driver.c"
    INCLUDE_DIRS "." "board_185/I2C_Driver" "board_185/EXIO" "board_185/LCD_Driver/esp_lcd_st77916"
                 "board_185/LCD_Driver" "board_185/Touch_Driver/esp_lcd_touch" "board_185/Touch_Driver" "board_185/LVGL_Driver"
                 "board_185/PCF85063" "board_185/BAT_Driver"
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_sdmmc esp_driver_spi
             esp_http_client esp_http_server esp_netif esp_wifi esp_event esp-tls fatfs json nvs_flash lvgl esp-audio-player esp_adc)
