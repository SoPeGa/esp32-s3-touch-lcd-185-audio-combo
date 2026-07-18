#include "board_spotpear.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "BAT_Driver.h"
#include "I2C_Driver.h"
#include "LVGL_Driver.h"
#include "ST77916.h"
#include "TCA9554PWR.h"

static const char *TAG = "board_185";
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static uint8_t s_volume = 55;
static bool s_sd_mounted;

/* Onboard I2S microphone (pins and conversion as in the Waveshare demo). */
#define MIC_I2S_BCLK GPIO_NUM_15
#define MIC_I2S_WS   GPIO_NUM_2
#define MIC_I2S_DIN  GPIO_NUM_39
#define MIC_CHUNK_FRAMES 512

esp_err_t board_display_init(void) { I2C_Init(); ESP_RETURN_ON_ERROR(EXIO_Init(), TAG, "EXIO init"); LCD_Init(); return panel_handle ? ESP_OK : ESP_FAIL; }
esp_err_t board_lvgl_init(void) { LVGL_Init(); return ESP_OK; }
esp_lcd_panel_handle_t board_lcd_panel(void) { return panel_handle; }
esp_err_t board_touch_init(void) { return ESP_OK; }
bool board_touch_read(int *x, int *y) { uint16_t tx=0,ty=0; uint8_t n=0; if (!tp || esp_lcd_touch_read_data(tp)!=ESP_OK || !esp_lcd_touch_get_coordinates(tp,&tx,&ty,NULL,&n,1) || !n) return false; if(x)*x=tx; if(y)*y=ty; return true; }
esp_err_t board_lvgl_indev_init(void) { return ESP_OK; }

static esp_err_t audio_format(uint32_t rate, uint32_t bits, uint32_t channels) { ESP_RETURN_ON_FALSE(s_i2s_tx, ESP_ERR_INVALID_STATE, TAG, "I2S unavailable"); i2s_std_clk_config_t clk=I2S_STD_CLK_DEFAULT_CONFIG(rate); i2s_std_slot_config_t slot=I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)bits, channels==1?I2S_SLOT_MODE_MONO:I2S_SLOT_MODE_STEREO); ESP_RETURN_ON_ERROR(i2s_channel_disable(s_i2s_tx),TAG,"disable I2S"); ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s_tx,&clk),TAG,"clock"); ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_i2s_tx,&slot),TAG,"slot"); return i2s_channel_enable(s_i2s_tx); }
esp_err_t board_audio_init(void) { i2s_chan_config_t cc=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER); cc.auto_clear=true; cc.dma_desc_num=12; cc.dma_frame_num=512; ESP_RETURN_ON_ERROR(i2s_new_channel(&cc,&s_i2s_tx,NULL),TAG,"new I2S"); i2s_std_config_t cfg={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(44100),.slot_cfg=I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),.gpio_cfg={.mclk=GPIO_NUM_NC,.bclk=GPIO_NUM_48,.ws=GPIO_NUM_38,.dout=GPIO_NUM_47,.din=GPIO_NUM_NC}}; ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx,&cfg),TAG,"I2S mode"); return i2s_channel_enable(s_i2s_tx); }
esp_err_t board_audio_set_output_format(uint32_t r,uint32_t b,uint32_t c){return audio_format(r,b,c);}
esp_err_t board_audio_set_input_format(uint32_t r, uint32_t b, uint32_t c)
{
    (void)b; (void)c;  /* output is always 16-bit mono, converted from the 32-bit stereo mic */
    if (s_i2s_rx) return ESP_OK;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, NULL, &s_i2s_rx), TAG, "mic chan");
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(r),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_NC, .bclk = MIC_I2S_BCLK, .ws = MIC_I2S_WS,
                     .dout = GPIO_NUM_NC, .din = MIC_I2S_DIN},
    };
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    esp_err_t ret = i2s_channel_init_std_mode(s_i2s_rx, &cfg);
    if (ret == ESP_OK) ret = i2s_channel_enable(s_i2s_rx);
    if (ret != ESP_OK) { i2s_del_channel(s_i2s_rx); s_i2s_rx = NULL; }
    return ret;
}
esp_err_t board_audio_write(void *data,size_t len,size_t *written,uint32_t timeout){if(s_volume<100&&data){int16_t *p=data;for(size_t i=0;i<len/2;i++)p[i]=(int16_t)(((int32_t)p[i]*s_volume)/100);}return i2s_channel_write(s_i2s_tx,data,len,written,timeout);}
/* Fills data with 16-bit mono samples converted from the 32-bit stereo mic stream. */
esp_err_t board_audio_read(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_i2s_rx, ESP_ERR_INVALID_STATE, TAG, "mic not started");
    static int32_t raw[MIC_CHUNK_FRAMES * 2];
    size_t frames = len / 2;
    if (frames > MIC_CHUNK_FRAMES) frames = MIC_CHUNK_FRAMES;
    size_t raw_read = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_read(s_i2s_rx, raw, frames * 2 * sizeof(int32_t), &raw_read, timeout_ms),
                        TAG, "mic read");
    size_t got = raw_read / (2 * sizeof(int32_t));
    int16_t *out = data;
    for (size_t i = 0; i < got; i++) {
        int32_t left = raw[i * 2] >> 14;
        int32_t right = raw[i * 2 + 1] >> 14;
        int32_t s = (abs(right) > abs(left)) ? right : left;
        s *= 3;
        if (s > INT16_MAX) s = INT16_MAX;
        if (s < INT16_MIN) s = INT16_MIN;
        out[i] = (int16_t)s;
    }
    if (bytes_read) *bytes_read = got * 2;
    return ESP_OK;
}
void board_audio_set_volume(uint8_t v){s_volume=v>100?100:v;} uint8_t board_audio_get_volume(void){return s_volume;}

esp_err_t board_sd_mount(void){if(s_sd_mounted)return ESP_OK;esp_vfs_fat_sdmmc_mount_config_t mc={.format_if_mount_failed=false,.max_files=8,.allocation_unit_size=16384};sdmmc_host_t host=SDMMC_HOST_DEFAULT();sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();slot.width=1;slot.clk=GPIO_NUM_14;slot.cmd=GPIO_NUM_17;slot.d0=GPIO_NUM_16;slot.d1=slot.d2=slot.d3=GPIO_NUM_NC;slot.flags|=SDMMC_SLOT_FLAG_INTERNAL_PULLUP;sdmmc_card_t *card=NULL;ESP_RETURN_ON_ERROR(esp_vfs_fat_sdmmc_mount("/sdcard",&host,&slot,&mc,&card),TAG,"mount SD");s_sd_mounted=true;sdmmc_card_print_info(stdout,card);return ESP_OK;}
FILE *board_open_file(const char *path){return fopen(path,"rb");}
static void scan(const char *dir,char names[][96],size_t max,int *count){DIR *d=opendir(dir);if(!d)return;struct dirent *e;while(*count<(int)max&&(e=readdir(d))){const char *dot=strrchr(e->d_name,'.');if(!dot||strcasecmp(dot,".mp3"))continue;strlcpy(names[*count],dir,96);strlcat(names[*count],"/",96);strlcat(names[*count],e->d_name,96);(*count)++;}closedir(d);}
int board_find_mp3_files(char names[][96],size_t max){int n=0;scan("/sdcard",names,max,&n);scan("/sdcard/music",names,max,&n);return n;}
bool board_boot_button_pressed(void){return false;}

static bool s_bat_ready;
esp_err_t board_battery_init(void){ BAT_Init(); s_bat_ready = true; return ESP_OK; }

/* LiPo voltage to percent, piecewise linear. 255 = no battery detected. */
uint8_t board_battery_read_pct(void)
{
    if (!s_bat_ready) return 255;
    float v = BAT_Get_Volts();
    if (v < 2.8f) return 255;           /* USB only, no battery attached */
    static const struct { float v; uint8_t pct; } curve[] = {
        {4.15f, 100}, {4.00f, 85}, {3.90f, 70}, {3.80f, 55},
        {3.70f, 40}, {3.60f, 25}, {3.50f, 12}, {3.30f, 0},
    };
    if (v >= curve[0].v) return 100;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (v >= curve[i].v) {
            float span = curve[i - 1].v - curve[i].v;
            float frac = (v - curve[i].v) / span;
            return curve[i].pct + (uint8_t)(frac * (curve[i - 1].pct - curve[i].pct));
        }
    }
    return 0;
}



