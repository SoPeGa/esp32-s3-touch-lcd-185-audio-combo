#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define LCD_H_RES 360
#define LCD_V_RES 360

esp_err_t board_display_init(void);
esp_err_t board_lvgl_init(void);
esp_lcd_panel_handle_t board_lcd_panel(void);

esp_err_t board_touch_init(void);
bool board_touch_read(int *x, int *y);
esp_err_t board_lvgl_indev_init(void);  /* register CST816 as LVGL pointer device */

esp_err_t board_audio_init(void);
esp_err_t board_audio_set_output_format(uint32_t sample_rate, uint32_t bits_per_sample, uint32_t channels);
esp_err_t board_audio_set_input_format(uint32_t sample_rate, uint32_t bits_per_sample, uint32_t channels);
esp_err_t board_audio_write(void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);
esp_err_t board_audio_read(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);
void board_audio_set_volume(uint8_t volume);
uint8_t board_audio_get_volume(void);

esp_err_t board_sd_mount(void);
FILE *board_open_file(const char *path);
int board_find_mp3_files(char names[][96], size_t max_files);

bool board_boot_button_pressed(void);

esp_err_t board_battery_init(void);
uint8_t   board_battery_read_pct(void);  /* returns 0 if not available */

