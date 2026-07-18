#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <strings.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include "audio_player.h"
#include "board_spotpear.h"
#include "PCF85063.h"
#include "ST77916.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "ui_round.h"

#define TAG "combo"
#define MAX_TRACKS 80
#define MAX_STATIONS 16
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MANAGER_AP_SSID "Spotpear-Radio"
#define WIFI_MANAGER_AP_PASS "12345678"
#define WIFI_MANAGER_NVS_NAMESPACE "wifi_mgr"
#define PLAYER_NVS_NAMESPACE       "player"
#define AI_WAV_PATH "/sdcard/ai_question.wav"
#define AI_TTS_PATH "/sdcard/ai_reply.mp3"
#define AI_RECORD_RATE 16000
#define AI_RECORD_CHANNELS 1
#define AI_RECORD_BITS 16
#define RADIO_STREAM_BUFFER_SIZE (64 * 1024)
#define RADIO_STREAM_PREBUFFER    (24 * 1024)
#define RADIO_STREAM_CHUNK        4096

typedef struct {
    char name[48];
    char url[320];
} station_t;

typedef struct {
    esp_http_client_handle_t client;
    bool opened;
    int icy_metaint;
    int icy_audio_left;
    size_t rewind_len;
    size_t rewind_pos;
    uint8_t rewind_buf[4096];
    StreamBufferHandle_t stream_buffer;
    StaticStreamBuffer_t *stream_buffer_ctrl;
    uint8_t *stream_buffer_storage;
    TaskHandle_t stream_task;
    volatile bool stream_stop;
    volatile bool stream_done;
    bool prebuffered;
    char station_name[96];
    char stream_title[192];
    char url[384];
} http_file_t;

static QueueHandle_t s_action_queue;
static EventGroupHandle_t s_wifi_events;
static char s_tracks[MAX_TRACKS][96];
static int s_track_count;
static int s_track_index;
static station_t s_stations[MAX_STATIONS];
static int s_station_count;
static int s_station_index;
static ui_mode_t s_mode = UI_MODE_RADIO;
static volatile bool s_player_idle;
static volatile bool s_player_unknown;
static bool s_playing;
static bool s_paused;
static volatile uint8_t s_mp3_progress = UI_PROGRESS_HIDE;  /* 0-100 while an SD MP3 plays */
static volatile bool s_radio_buffering;
static bool s_rtc_present;
static volatile bool s_sntp_synced;
static esp_netif_t *s_wifi_sta_netif;
static esp_netif_t *s_wifi_ap_netif;
static httpd_handle_t s_wifi_httpd;
static bool s_wifi_initialized;
static bool s_wifi_handlers_registered;
static bool s_wifi_started;
static bool s_wifi_sta_reconnect;
static bool s_wifi_manager_active;
static bool s_web_server_active;
static TaskHandle_t s_ai_task;
static int s_ai_voice_index;
static volatile int s_web_station_index = -1;
static volatile int s_web_track_index = -1;
static char s_web_test_name[48];
static char s_web_test_url[320];
static portMUX_TYPE s_metadata_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_radio_metadata_dirty;
static char s_radio_station[96];
static char s_radio_title[192];
static volatile int s_web_volume = -1;
static volatile int s_web_delete_index = -1;
static volatile uint8_t s_battery_cache = 255;
static volatile uint32_t s_saver_timeout_ms = 30000;  /* 0 = screensaver disabled */
static char s_sta_ip[20] = "";
static volatile bool s_show_ip;
static char s_now_title[128];      /* mirror of what the LCD shows, for /api/state */
static char s_now_subtitle[192];   /* guarded by s_metadata_lock */

enum {
    APP_ACTION_WEB_RADIO_PLAY = 100,
    APP_ACTION_WEB_MP3_PLAY,
    APP_ACTION_WEB_STOP,
    APP_ACTION_WEB_RADIO_TEST,
    APP_ACTION_WEB_VOLUME,
    APP_ACTION_WEB_MP3_DELETE,
    APP_ACTION_WEB_RESCAN,
};
static const char *const s_ai_voices[] = {"alloy", "nova", "shimmer", "echo", "fable", "onyx"};

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void ai_start_conversation(void);
static void save_player_state(void);
static void save_stations_to_nvs(void);

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static esp_err_t read_req_body(httpd_req_t *req, char *body, size_t body_len)
{
    int remaining = req->content_len;
    int offset = 0;
    if (body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    body[0] = '\0';
    while (remaining > 0 && offset < (int)body_len - 1) {
        int chunk = remaining;
        int space = (int)body_len - 1 - offset;
        if (chunk > space) {
            chunk = space;
        }
        int read_len = httpd_req_recv(req, body + offset, chunk);
        if (read_len <= 0) {
            return ESP_FAIL;
        }
        offset += read_len;
        remaining -= read_len;
    }
    body[offset] = '\0';
    return ESP_OK;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void publish_radio_metadata(const char *station, const char *title)
{
    portENTER_CRITICAL(&s_metadata_lock);
    if (station && station[0]) strlcpy(s_radio_station, station, sizeof(s_radio_station));
    if (title) strlcpy(s_radio_title, title, sizeof(s_radio_title));
    s_radio_metadata_dirty = true;
    portEXIT_CRITICAL(&s_metadata_lock);
}

static void copy_id3_text(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    if (!dst_len) return;
    dst[0] = '\0';
    if (!src || src_len < 2) return;
    uint8_t encoding = src[0];
    size_t out = 0;
    if (encoding == 1 || encoding == 2) {
        bool little = encoding == 1 && src_len >= 3 && src[1] == 0xff && src[2] == 0xfe;
        size_t i = (encoding == 1 && src_len >= 3 && (src[1] == 0xff || src[1] == 0xfe)) ? 3 : 1;
        for (; i + 1 < src_len && out + 1 < dst_len; i += 2) {
            uint8_t c = little ? src[i] : src[i + 1];
            if (!c) break;
            dst[out++] = (char)c;
        }
    } else {
        for (size_t i = 1; i < src_len && out + 1 < dst_len && src[i]; i++) {
            dst[out++] = (char)src[i];
        }
    }
    dst[out] = '\0';
    char *clean = trim(dst);
    if (clean != dst) memmove(dst, clean, strlen(clean) + 1);
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

static void load_mp3_metadata(const char *path, char *title, size_t title_len,
                              char *artist, size_t artist_len)
{
    strlcpy(title, base_name(path), title_len);
    strlcpy(artist, "Artist necunoscut", artist_len);
    FILE *f = fopen(path, "rb");
    if (!f) return;

    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && memcmp(hdr, "ID3", 3) == 0 &&
        (hdr[3] == 3 || hdr[3] == 4)) {
        uint8_t version = hdr[3];
        uint32_t remaining = syncsafe32(hdr + 6);
        while (remaining >= 10 && fread(hdr, 1, 10, f) == 10) {
            uint32_t frame_size = version == 4 ? syncsafe32(hdr + 4) :
                ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) |
                ((uint32_t)hdr[6] << 8) | hdr[7];
            if (!hdr[0] || frame_size == 0 || frame_size > remaining - 10) break;
            bool wanted = memcmp(hdr, "TIT2", 4) == 0 || memcmp(hdr, "TPE1", 4) == 0;
            if (wanted) {
                uint8_t text[256];
                size_t n = frame_size < sizeof(text) ? frame_size : sizeof(text);
                if (fread(text, 1, n, f) != n) break;
                copy_id3_text(memcmp(hdr, "TIT2", 4) == 0 ? title : artist,
                              memcmp(hdr, "TIT2", 4) == 0 ? title_len : artist_len, text, n);
                if (frame_size > n) fseek(f, frame_size - n, SEEK_CUR);
            } else {
                fseek(f, frame_size, SEEK_CUR);
            }
            remaining -= 10 + frame_size;
        }
    }

    uint8_t tag[128];
    if ((strcmp(title, base_name(path)) == 0 || strcmp(artist, "Artist necunoscut") == 0) &&
        fseek(f, -128, SEEK_END) == 0 && fread(tag, 1, sizeof(tag), f) == sizeof(tag) &&
        memcmp(tag, "TAG", 3) == 0) {
        if (strcmp(title, base_name(path)) == 0) {
            size_t n = title_len > 31 ? 30 : title_len - 1;
            memcpy(title, tag + 3, n);
            title[n] = '\0';
        }
        if (strcmp(artist, "Artist necunoscut") == 0) {
            size_t n = artist_len > 31 ? 30 : artist_len - 1;
            memcpy(artist, tag + 33, n);
            artist[n] = '\0';
        }
        char *clean = trim(title);
        if (clean != title) memmove(title, clean, strlen(clean) + 1);
        clean = trim(artist);
        if (clean != artist) memmove(artist, clean, strlen(clean) + 1);
    }
    fclose(f);
}

static void set_now_strings(const char *title, const char *subtitle)
{
    portENTER_CRITICAL(&s_metadata_lock);
    strlcpy(s_now_title, title ? title : "", sizeof(s_now_title));
    strlcpy(s_now_subtitle, subtitle ? subtitle : "", sizeof(s_now_subtitle));
    portEXIT_CRITICAL(&s_metadata_lock);
}

static void set_now_playing(void)
{
    ui_set_mode(s_mode);
    if (s_mode == UI_MODE_AI) {
        ui_set_title("Asistent AI");
        ui_set_subtitle(s_ai_voices[s_ai_voice_index]);
        set_now_strings("Asistent AI", s_ai_voices[s_ai_voice_index]);
    } else if (s_mode == UI_MODE_MP3) {
        if (s_track_count > 0) {
            char title[128], artist[96];
            load_mp3_metadata(s_tracks[s_track_index], title, sizeof(title), artist, sizeof(artist));
            ui_set_mp3_metadata(title, artist);
            set_now_strings(title, artist);
        } else {
            ui_set_mp3_metadata("Fara fisiere MP3", "/sdcard sau /sdcard/music");
            set_now_strings("Fara fisiere MP3", "");
        }
    } else {
        if (s_station_count > 0) {
            ui_set_radio_metadata(s_stations[s_station_index].name, "Astept metadata...");
            set_now_strings(s_stations[s_station_index].name, "");
        } else {
            ui_set_radio_metadata("Internet Radio", "Adauga posturi in radio.txt");
            set_now_strings("Internet Radio", "");
        }
    }
    ui_set_playing(s_playing);
    ui_set_volume(board_audio_get_volume());
}

static esp_err_t audio_mute(AUDIO_PLAYER_MUTE_SETTING setting)
{
    (void)setting;
    return ESP_OK;
}

static esp_err_t audio_clk(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    uint32_t channels = (ch == I2S_SLOT_MODE_MONO) ? 1 : 2;
    return board_audio_set_output_format(rate, bits_cfg, channels);
}

static void audio_cb(audio_player_cb_ctx_t *ctx)
{
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        s_player_idle = true;
    } else if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE) {
        s_player_unknown = true;
    }
}

static esp_err_t radio_http_event(esp_http_client_event_t *evt)
{
    http_file_t *hf = (http_file_t *)evt->user_data;
    if (!hf || evt->event_id != HTTP_EVENT_ON_HEADER || !evt->header_key || !evt->header_value) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
        hf->icy_metaint = atoi(evt->header_value);
        hf->icy_audio_left = hf->icy_metaint;
        ESP_LOGI(TAG, "ICY metadata interval: %d", hf->icy_metaint);
    } else if (strcasecmp(evt->header_key, "icy-name") == 0 && evt->header_value[0]) {
        strlcpy(hf->station_name, evt->header_value, sizeof(hf->station_name));
    }
    return ESP_OK;
}

static esp_err_t init_audio_player(void)
{
    ESP_RETURN_ON_ERROR(board_audio_init(), TAG, "audio board");
    audio_player_config_t cfg = {
        .mute_fn = audio_mute,
        .clk_set_fn = audio_clk,
        .write_fn = board_audio_write,
        .priority = 5,
        .coreID = 1,
    };
    ESP_RETURN_ON_ERROR(audio_player_new(cfg), TAG, "audio player");
    return audio_player_callback_register(audio_cb, NULL);
}

static int http_stream_open(http_file_t *hf)
{
    if (hf->opened) {
        return 0;
    }
    esp_http_client_config_t cfg = {
        .url                  = hf->url,
        .timeout_ms           = 3000,
        .buffer_size          = 8192,
        .buffer_size_tx       = 1024,
        .crt_bundle_attach    = esp_crt_bundle_attach,
        .keep_alive_enable    = true,
        .max_redirection_count = 8,    /* follow HTTP 3xx redirects */
        .event_handler        = radio_http_event,
        .user_data            = hf,
    };
    hf->client = esp_http_client_init(&cfg);
    if (!hf->client) {
        errno = ENOMEM;
        return -1;
    }
    esp_http_client_set_header(hf->client, "User-Agent", "SpotpearMP3Radio/1.0");
    esp_http_client_set_header(hf->client, "Icy-MetaData", "1");
    esp_http_client_set_header(hf->client, "Accept", "audio/mpeg,audio/aac,audio/*,*/*");

    esp_err_t ret = esp_http_client_open(hf->client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "radio open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(hf->client);
        hf->client = NULL;
        errno = EIO;
        return -1;
    }
    esp_http_client_fetch_headers(hf->client);
    publish_radio_metadata(hf->station_name, hf->stream_title);
    int status = esp_http_client_get_status_code(hf->client);
    /* 200 OK or ICY 200 â€” anything else with a 4xx/5xx body is an error */
    if (status >= 400) {
        ESP_LOGE(TAG, "radio HTTP status %d for %s", status, hf->url);
        esp_http_client_close(hf->client);
        esp_http_client_cleanup(hf->client);
        hf->client = NULL;
        errno = EIO;
        return -1;
    }
    ESP_LOGI(TAG, "radio connected: status=%d url=%s", status, hf->url);
    hf->opened = true;
    return 0;
}

static int http_raw_read(http_file_t *hf, char *buf, size_t size)
{
    int retries = 0;
    while (true) {
        int r = esp_http_client_read(hf->client, buf, size);
        if (r >= 0) return r;
        if (++retries > 4) {
            ESP_LOGW(TAG, "radio read timeout, triggering reconnect");
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void parse_icy_metadata(http_file_t *hf, char *metadata)
{
    const char *key = "StreamTitle='";
    char *start = strstr(metadata, key);
    if (!start) return;
    start += strlen(key);
    char *end = strstr(start, "';");
    if (end) *end = '\0';
    if (start[0] && strcmp(start, hf->stream_title) != 0) {
        strlcpy(hf->stream_title, start, sizeof(hf->stream_title));
        publish_radio_metadata(hf->station_name, hf->stream_title);
        ESP_LOGI(TAG, "ICY: %s - %s", hf->station_name, hf->stream_title);
    }
}

static ssize_t http_stream_read_audio(http_file_t *hf, char *buf, size_t size)
{
    if (http_stream_open(hf) != 0) return -1;
    if (hf->icy_metaint <= 0) return http_raw_read(hf, buf, size);

    size_t total = 0;
    while (total < size) {
        if (hf->icy_audio_left > 0) {
            size_t wanted = size - total;
            if (wanted > (size_t)hf->icy_audio_left) wanted = hf->icy_audio_left;
            int r = http_raw_read(hf, buf + total, wanted);
            if (r <= 0) return total ? (ssize_t)total : r;
            total += r;
            hf->icy_audio_left -= r;
            continue;
        }

        uint8_t blocks = 0;
        int r = http_raw_read(hf, (char *)&blocks, 1);
        if (r <= 0) return total ? (ssize_t)total : r;
        size_t bytes_left = (size_t)blocks * 16;
        char metadata[512];
        size_t saved = 0;
        while (bytes_left > 0) {
            char chunk[128];
            size_t n = bytes_left < sizeof(chunk) ? bytes_left : sizeof(chunk);
            r = http_raw_read(hf, chunk, n);
            if (r <= 0) return total ? (ssize_t)total : r;
            size_t keep = (size_t)r;
            if (keep > sizeof(metadata) - 1 - saved) keep = sizeof(metadata) - 1 - saved;
            if (keep) {
                memcpy(metadata + saved, chunk, keep);
                saved += keep;
            }
            bytes_left -= r;
        }
        metadata[saved] = '\0';
        if (saved) parse_icy_metadata(hf, metadata);
        hf->icy_audio_left = hf->icy_metaint;
    }
    return total;
}

static void http_stream_task(void *arg)
{
    http_file_t *hf = (http_file_t *)arg;
    uint8_t *chunk = malloc(RADIO_STREAM_CHUNK);
    if (!chunk) {
        ESP_LOGE(TAG, "radio stream chunk allocation failed");
        hf->stream_done = true;
        vTaskDelete(NULL);
        return;
    }

    while (!hf->stream_stop) {
        ssize_t read_len = http_stream_read_audio(hf, (char *)chunk, RADIO_STREAM_CHUNK);
        if (read_len <= 0) break;

        size_t sent = 0;
        while (sent < (size_t)read_len && !hf->stream_stop) {
            sent += xStreamBufferSend(hf->stream_buffer, chunk + sent,
                                      (size_t)read_len - sent, pdMS_TO_TICKS(250));
        }
    }

    free(chunk);
    hf->stream_done = true;
    vTaskDelete(NULL);
}

static int http_stream_buffer_start(http_file_t *hf)
{
    if (hf->stream_buffer) return 0;
    if (http_stream_open(hf) != 0) return -1;

    hf->stream_buffer_ctrl = heap_caps_malloc(sizeof(*hf->stream_buffer_ctrl),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    hf->stream_buffer_storage = heap_caps_malloc(RADIO_STREAM_BUFFER_SIZE,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hf->stream_buffer_ctrl || !hf->stream_buffer_storage) {
        ESP_LOGE(TAG, "radio stream buffer allocation failed");
        heap_caps_free(hf->stream_buffer_ctrl);
        heap_caps_free(hf->stream_buffer_storage);
        hf->stream_buffer_ctrl = NULL;
        hf->stream_buffer_storage = NULL;
        errno = ENOMEM;
        return -1;
    }
    hf->stream_buffer = xStreamBufferCreateStatic(RADIO_STREAM_BUFFER_SIZE, 1,
                                                   hf->stream_buffer_storage,
                                                   hf->stream_buffer_ctrl);
    if (!hf->stream_buffer) {
        ESP_LOGE(TAG, "radio static stream buffer creation failed");
        heap_caps_free(hf->stream_buffer_ctrl);
        heap_caps_free(hf->stream_buffer_storage);
        hf->stream_buffer_ctrl = NULL;
        hf->stream_buffer_storage = NULL;
        errno = ENOMEM;
        return -1;
    }
    ESP_LOGI(TAG, "radio buffer allocated in PSRAM: %u bytes, free PSRAM=%u",
             RADIO_STREAM_BUFFER_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (xTaskCreatePinnedToCore(http_stream_task, "radio_net", 6144, hf, 6,
                                &hf->stream_task, 0) != pdPASS) {
        hf->stream_buffer = NULL;
        heap_caps_free(hf->stream_buffer_ctrl);
        heap_caps_free(hf->stream_buffer_storage);
        hf->stream_buffer_ctrl = NULL;
        hf->stream_buffer_storage = NULL;
        hf->stream_buffer = NULL;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static ssize_t http_cookie_read(void *cookie, char *buf, size_t size)
{
    http_file_t *hf = (http_file_t *)cookie;
    size_t total = 0;
    if (hf->rewind_pos < hf->rewind_len) {
        size_t available = hf->rewind_len - hf->rewind_pos;
        size_t n = size < available ? size : available;
        memcpy(buf, hf->rewind_buf + hf->rewind_pos, n);
        hf->rewind_pos += n;
        total += n;
    }
    if (total < size) {
        if (http_stream_buffer_start(hf) != 0) return total ? (ssize_t)total : -1;

        if (!hf->prebuffered) {
            s_radio_buffering = true;
            while (!hf->stream_done &&
                   xStreamBufferBytesAvailable(hf->stream_buffer) < RADIO_STREAM_PREBUFFER) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            hf->prebuffered = true;
            s_radio_buffering = false;
            ESP_LOGI(TAG, "radio prebuffer ready: %u bytes",
                     (unsigned)xStreamBufferBytesAvailable(hf->stream_buffer));
        }

        size_t r = xStreamBufferReceive(hf->stream_buffer, buf + total, size - total,
                                        pdMS_TO_TICKS(1000));
        if (r == 0) {
            if (hf->stream_done && xStreamBufferIsEmpty(hf->stream_buffer)) {
                return total ? (ssize_t)total : 0;
            }
            return total ? (ssize_t)total : -1;
        }
        size_t space = sizeof(hf->rewind_buf) - hf->rewind_len;
        size_t n = r < space ? r : space;
        if (n) {
            memcpy(hf->rewind_buf + hf->rewind_len, buf + total, n);
            hf->rewind_len += n;
            hf->rewind_pos = hf->rewind_len;
        }
        total += r;
    }
    return (ssize_t)total;
}

static int http_cookie_seek(void *cookie, off_t *offset, int whence)
{
    http_file_t *hf = (http_file_t *)cookie;
    if (whence == SEEK_SET && offset && *offset == 0) {
        hf->rewind_pos = 0;
        return 0;
    }
    errno = ESPIPE;
    return -1;
}

static int http_cookie_close(void *cookie)
{
    http_file_t *hf = (http_file_t *)cookie;
    hf->stream_stop = true;
    s_radio_buffering = false;
    while (hf->stream_task && !hf->stream_done) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (hf->stream_buffer) {
        hf->stream_buffer = NULL;
        heap_caps_free(hf->stream_buffer_ctrl);
        heap_caps_free(hf->stream_buffer_storage);
    }
    if (hf->client) {
        if (hf->opened) {
            esp_http_client_close(hf->client);
        }
        esp_http_client_cleanup(hf->client);
    }
    free(hf);
    return 0;
}

static FILE *open_http_file(const char *url, const char *station_name)
{
    http_file_t *hf = calloc(1, sizeof(*hf));
    if (!hf) {
        return NULL;
    }
    strlcpy(hf->url, url, sizeof(hf->url));
    strlcpy(hf->station_name, station_name && station_name[0] ? station_name : "Internet Radio",
            sizeof(hf->station_name));
    cookie_io_functions_t io = {
        .read = http_cookie_read,
        .write = NULL,
        .seek = http_cookie_seek,
        .close = http_cookie_close,
    };
    FILE *fp = fopencookie(hf, "rb", io);
    if (!fp) {
        free(hf);
    }
    return fp;
}

/* Wrap an SD-card MP3 in a cookie FILE so playback position can be tracked. */
typedef struct {
    FILE *fp;
    long size;
} sd_mp3_file_t;

static ssize_t sd_mp3_read(void *cookie, char *buf, size_t len)
{
    sd_mp3_file_t *m = cookie;
    size_t n = fread(buf, 1, len, m->fp);
    if (m->size > 0) {
        long pos = ftell(m->fp);
        if (pos >= 0) {
            long pct = (pos * 100) / m->size;
            s_mp3_progress = pct > 100 ? 100 : (uint8_t)pct;
        }
    }
    return (ssize_t)n;
}

static int sd_mp3_seek(void *cookie, off_t *offset, int whence)
{
    sd_mp3_file_t *m = cookie;
    if (fseek(m->fp, (long)*offset, whence) != 0) {
        return -1;
    }
    *offset = ftell(m->fp);
    return 0;
}

static int sd_mp3_close(void *cookie)
{
    sd_mp3_file_t *m = cookie;
    fclose(m->fp);
    free(m);
    return 0;
}

static FILE *open_mp3_file(const char *path)
{
    FILE *fp = board_open_file(path);
    if (!fp) {
        return NULL;
    }
    sd_mp3_file_t *m = calloc(1, sizeof(*m));
    if (!m) {
        fclose(fp);
        return NULL;
    }
    m->fp = fp;
    if (fseek(fp, 0, SEEK_END) == 0) {
        m->size = ftell(fp);
    }
    rewind(fp);
    cookie_io_functions_t io = {
        .read = sd_mp3_read,
        .write = NULL,
        .seek = sd_mp3_seek,
        .close = sd_mp3_close,
    };
    FILE *wrapped = fopencookie(m, "rb", io);
    if (!wrapped) {
        fclose(fp);
        free(m);
        return NULL;
    }
    return wrapped;
}

static void stop_playback(void)
{
    audio_player_stop();
    s_playing = false;
    s_paused = false;
    ui_set_playing(false);
    ui_set_busy(false);
    ui_set_status("Oprit");
}

static void pause_playback(void)
{
    if (s_playing && !s_paused && audio_player_pause() == ESP_OK) {
        s_paused = true;
        ui_set_playing(false);
        ui_set_status("Pauza");
    }
}

static void resume_playback(void)
{
    if (s_playing && s_paused && audio_player_resume() == ESP_OK) {
        s_paused = false;
        ui_set_playing(true);
        ui_set_status("Redare");
    }
}

static void play_stream_url(const char *name, const char *url, bool test_mode)
{
    FILE *fp = NULL;
    s_player_idle = false;
    s_player_unknown = false;

    if (!url || url[0] == '\0') {
        ui_set_status("Fara URL");
        return;
    }

    fp = open_http_file(url, name);
    if (!fp) {
        ui_set_status("Eroare flux");
        return;
    }
    if (audio_player_play(fp) != ESP_OK) {
        fclose(fp);
        ui_set_status("Eroare redare");
        return;
    }
    s_mode = UI_MODE_RADIO;
    s_playing = true;
    s_paused = false;
    s_mp3_progress = UI_PROGRESS_HIDE;
    ui_set_mode(s_mode);
    ui_set_title(name && name[0] ? name : "Radio Test");
    ui_set_subtitle(test_mode ? "Test stream" : "");
    set_now_strings(name && name[0] ? name : "Radio Test", test_mode ? "Test stream" : "");
    ui_set_playing(true);
    ui_set_volume(board_audio_get_volume());
    ui_set_status(test_mode ? "Testare" : "Redare");
}

static void play_current(void)
{
    FILE *fp = NULL;
    s_player_idle = false;
    s_player_unknown = false;

    if (s_mode == UI_MODE_MP3) {
        if (s_track_count <= 0) {
            ui_set_status("Fara MP3");
            return;
        }
        fp = open_mp3_file(s_tracks[s_track_index]);
    } else {
        if (s_station_count <= 0 || s_stations[s_station_index].url[0] == '\0') {
            ui_set_status("Fara URL");
            return;
        }
        fp = open_http_file(s_stations[s_station_index].url, s_stations[s_station_index].name);
    }

    if (!fp) {
        ui_set_status("Eroare deschidere");
        return;
    }
    if (audio_player_play(fp) != ESP_OK) {
        fclose(fp);
        ui_set_status("Eroare redare");
        return;
    }
    s_playing = true;
    s_paused = false;
    s_mp3_progress = (s_mode == UI_MODE_MP3) ? 0 : UI_PROGRESS_HIDE;
    set_now_playing();
    ui_set_status("Redare");
}

static void next_item(int dir)
{
    if (s_mode == UI_MODE_MP3 && s_track_count > 0) {
        s_track_index = (s_track_index + dir + s_track_count) % s_track_count;
    } else if (s_mode == UI_MODE_RADIO && s_station_count > 0) {
        s_station_index = (s_station_index + dir + s_station_count) % s_station_count;
    }
    set_now_playing();
    save_player_state();
    if (s_playing) {
        play_current();
    }
}

static void handle_action(int action)
{
    switch (action) {
    case UI_ACTION_MODE_RADIO:
        if (s_mode != UI_MODE_RADIO) {
            s_mode = UI_MODE_RADIO;
            set_now_playing();
            play_current();
            save_player_state();
        }
        break;
    case UI_ACTION_MODE_MP3:
        if (s_mode != UI_MODE_MP3) {
            s_mode = UI_MODE_MP3;
            set_now_playing();
            play_current();
            save_player_state();
        }
        break;
    case UI_ACTION_MODE_AI:
        if (s_playing) {
            stop_playback();
        }
        s_mode = UI_MODE_AI;
        ui_set_ai_phase(UI_AI_IDLE);
        set_now_playing();
        ui_set_status("Apasa microfonul");
        break;
    case UI_ACTION_PREV:
        next_item(-1);
        break;
    case UI_ACTION_NEXT:
        next_item(1);
        break;
    case UI_ACTION_PLAY_PAUSE:
        if (s_mode == UI_MODE_AI) {
            ai_start_conversation();
        } else if (s_playing && !s_paused) {
            pause_playback();
        } else if (s_playing && s_paused) {
            resume_playback();
        } else {
            play_current();
        }
        break;
    case UI_ACTION_VOL_SET:
        board_audio_set_volume(ui_get_arc_volume());
        save_player_state();
        break;
    case UI_ACTION_VOL_DOWN: {
        uint8_t v = board_audio_get_volume();
        board_audio_set_volume(v > 5 ? v - 5 : 0);
        ui_set_volume(board_audio_get_volume());
        save_player_state();
        break;
    }
    case UI_ACTION_VOL_UP: {
        uint8_t v = board_audio_get_volume();
        board_audio_set_volume(v < 95 ? v + 5 : 100);
        ui_set_volume(board_audio_get_volume());
        save_player_state();
        break;
    }
    case UI_ACTION_AI_VOICE:
        s_ai_voice_index = (s_ai_voice_index + 1) % (int)(sizeof(s_ai_voices) / sizeof(s_ai_voices[0]));
        if (s_mode == UI_MODE_AI) {
            set_now_playing();
            ui_set_status("Voce");
        }
        break;
    case APP_ACTION_WEB_RADIO_PLAY:
        if (s_web_station_index >= 0 && s_web_station_index < s_station_count) {
            s_mode = UI_MODE_RADIO;
            s_station_index = s_web_station_index;
            play_current();
            save_player_state();
        }
        break;
    case APP_ACTION_WEB_MP3_PLAY:
        if (s_web_track_index >= 0 && s_web_track_index < s_track_count) {
            s_mode = UI_MODE_MP3;
            s_track_index = s_web_track_index;
            play_current();
            save_player_state();
        }
        break;
    case APP_ACTION_WEB_STOP:
        stop_playback();
        break;
    case APP_ACTION_WEB_VOLUME: {
        int v = s_web_volume;
        if (v >= 0) {
            board_audio_set_volume((uint8_t)(v > 100 ? 100 : v));
            ui_set_volume(board_audio_get_volume());
            save_player_state();
        }
        break;
    }
    case APP_ACTION_WEB_MP3_DELETE: {
        int idx = s_web_delete_index;
        if (idx >= 0 && idx < s_track_count) {
            if (s_playing && s_mode == UI_MODE_MP3 && idx == s_track_index) {
                stop_playback();
            }
            if (unlink(s_tracks[idx]) != 0) {
                ESP_LOGW(TAG, "unlink %s failed", s_tracks[idx]);
            }
        }
    } /* fallthrough: rescan after delete */
    case APP_ACTION_WEB_RESCAN:
        s_track_count = board_find_mp3_files(s_tracks, MAX_TRACKS);
        if (s_track_index >= s_track_count) {
            s_track_index = 0;
        }
        if (s_mode == UI_MODE_MP3) {
            set_now_playing();
        }
        break;
    case UI_ACTION_OPEN_LIST: {
        char *opts = NULL;
        if (s_mode == UI_MODE_RADIO && s_station_count > 0) {
            size_t cap = (size_t)s_station_count * (sizeof(s_stations[0].name) + 1) + 1;
            opts = calloc(1, cap);
            if (!opts) break;
            for (int i = 0; i < s_station_count; i++) {
                if (i) strlcat(opts, "\n", cap);
                strlcat(opts, s_stations[i].name, cap);
            }
            ui_show_picker("Alege postul", opts, s_station_index);
        } else if (s_mode == UI_MODE_MP3 && s_track_count > 0) {
            size_t cap = (size_t)s_track_count * 97 + 1;
            opts = calloc(1, cap);
            if (!opts) break;
            for (int i = 0; i < s_track_count; i++) {
                if (i) strlcat(opts, "\n", cap);
                strlcat(opts, base_name(s_tracks[i]), cap);
            }
            ui_show_picker("Alege melodia", opts, s_track_index);
        }
        free(opts);
        break;
    }
    case UI_ACTION_LIST_PICK: {
        int idx = ui_get_picked_index();
        if (s_mode == UI_MODE_RADIO && idx >= 0 && idx < s_station_count) {
            s_station_index = idx;
        } else if (s_mode == UI_MODE_MP3 && idx >= 0 && idx < s_track_count) {
            s_track_index = idx;
        } else {
            break;
        }
        set_now_playing();
        play_current();
        save_player_state();
        break;
    }
    case APP_ACTION_WEB_RADIO_TEST:
        play_stream_url(s_web_test_name, s_web_test_url, true);
        break;
    default:
        break;
    }
}

static void ui_action_sender(int action)
{
    if (s_action_queue) {
        xQueueSend(s_action_queue, &action, 0);
    }
}

static void wav_write_u16(FILE *f, uint16_t v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
}

static void wav_write_u32(FILE *f, uint32_t v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f);
    fputc((v >> 24) & 0xff, f);
}

static void wav_write_header(FILE *f, uint32_t data_size)
{
    fwrite("RIFF", 1, 4, f);
    wav_write_u32(f, 36 + data_size);
    fwrite("WAVEfmt ", 1, 8, f);
    wav_write_u32(f, 16);
    wav_write_u16(f, 1);
    wav_write_u16(f, AI_RECORD_CHANNELS);
    wav_write_u32(f, AI_RECORD_RATE);
    wav_write_u32(f, AI_RECORD_RATE * AI_RECORD_CHANNELS * (AI_RECORD_BITS / 8));
    wav_write_u16(f, AI_RECORD_CHANNELS * (AI_RECORD_BITS / 8));
    wav_write_u16(f, AI_RECORD_BITS);
    fwrite("data", 1, 4, f);
    wav_write_u32(f, data_size);
}

/* Voice-activity thresholds, as in AI_Voice_Assistant_IDF. */
#define AI_SPEECH_START_LEVEL     5     /* % of full scale to consider speech started */
#define AI_SPEECH_CONTINUE_LEVEL  3     /* below this counts as silence */
#define AI_START_TIMEOUT_MS       5000  /* give up if nobody speaks */
#define AI_SILENCE_END_MS         1200  /* stop after this much trailing silence */
#define AI_MIN_SPEECH_MS          1000

static esp_err_t ai_record_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open wav");
    wav_write_header(f, 0);

    if (board_audio_set_input_format(AI_RECORD_RATE, AI_RECORD_BITS, AI_RECORD_CHANNELS) != ESP_OK) {
        fclose(f);
        return ESP_FAIL;
    }
    int16_t samples[512];
    const uint32_t chunk_ms = (512 * 1000U) / AI_RECORD_RATE;
    const size_t bytes_max = (size_t)AI_RECORD_RATE * 2 * CONFIG_AI_RECORD_SECONDS;
    size_t bytes_total = 0;
    bool speech_started = false;
    uint32_t waited_ms = 0, silence_ms = 0, speech_ms = 0;

    while (bytes_total < bytes_max) {
        size_t bytes_read = 0;
        if (board_audio_read(samples, sizeof(samples), &bytes_read, 1000) != ESP_OK || bytes_read == 0) {
            fclose(f);
            board_audio_set_output_format(44100, 16, 2);
            return ESP_FAIL;
        }
        int32_t peak = 0;
        for (size_t i = 0; i < bytes_read / 2; i++) {
            int32_t mag = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
            if (mag > peak) peak = mag;
        }
        int level = (int)((peak * 100) / INT16_MAX);

        if (!speech_started) {
            if (level >= AI_SPEECH_START_LEVEL) {
                speech_started = true;
                ESP_LOGI(TAG, "AI: speech started");
            } else {
                waited_ms += chunk_ms;
                if (waited_ms >= AI_START_TIMEOUT_MS) break;
                continue;
            }
        }
        fwrite(samples, 1, bytes_read, f);
        bytes_total += bytes_read;
        speech_ms += chunk_ms;
        if (level >= AI_SPEECH_CONTINUE_LEVEL) {
            silence_ms = 0;
        } else {
            silence_ms += chunk_ms;
        }
        if (speech_ms >= AI_MIN_SPEECH_MS && silence_ms >= AI_SILENCE_END_MS) {
            ESP_LOGI(TAG, "AI: speech ended after silence");
            break;
        }
    }

    fseek(f, 0, SEEK_SET);
    wav_write_header(f, bytes_total);
    fclose(f);
    esp_err_t out_ret = board_audio_set_output_format(44100, 16, 2);
    if (bytes_total == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return out_ret;
}

static char *http_read_text_response(esp_http_client_handle_t client, size_t max_len)
{
    char *buf = calloc(1, max_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t used = 0;
    while (used < max_len) {
        int r = esp_http_client_read(client, buf + used, max_len - used);
        if (r <= 0) {
            break;
        }
        used += r;
    }
    buf[used] = '\0';
    return buf;
}

static char s_openai_api_key[200] = CONFIG_OPENAI_API_KEY;

static void add_openai_auth_header(esp_http_client_handle_t client)
{
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", s_openai_api_key);
    esp_http_client_set_header(client, "Authorization", auth);
}

/* ---- real-time tools the AI assistant can call (function calling) ---- */

static char *http_get_text(const char *url, size_t max_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }
    char *out = NULL;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        out = http_read_text_response(client, max_len);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return out;
}

static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        }
    }
    dst[j] = '\0';
}

/* Weather via Open-Meteo (free, no key): geocode the city, then fetch the
   forecast and hand the raw JSON to the model to interpret. */
static char *tool_get_weather(const char *args_json)
{
    char city[96] = {0};
    cJSON *args = cJSON_Parse(args_json);
    cJSON *cj = args ? cJSON_GetObjectItem(args, "city") : NULL;
    if (cJSON_IsString(cj)) {
        strlcpy(city, cj->valuestring, sizeof(city));
    }
    cJSON_Delete(args);
    if (!city[0]) {
        return strdup("{\"error\":\"lipseste numele localitatii\"}");
    }

    char enc[200];
    url_encode(city, enc, sizeof(enc));
    char url[420];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=ro&format=json", enc);
    char *geo = http_get_text(url, 2048);
    if (!geo) {
        return strdup("{\"error\":\"geocodare esuata\"}");
    }
    cJSON *gj = cJSON_Parse(geo);
    free(geo);
    cJSON *results = gj ? cJSON_GetObjectItem(gj, "results") : NULL;
    cJSON *first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
    cJSON *lat = first ? cJSON_GetObjectItem(first, "latitude") : NULL;
    cJSON *lon = first ? cJSON_GetObjectItem(first, "longitude") : NULL;
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        cJSON_Delete(gj);
        return strdup("{\"error\":\"localitate negasita\"}");
    }
    double la = lat->valuedouble;
    double lo = lon->valuedouble;
    cJSON_Delete(gj);

    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=3", la, lo);
    char *wx = http_get_text(url, 3072);
    if (!wx) {
        return strdup("{\"error\":\"serviciu meteo indisponibil\"}");
    }
    return wx;
}

/* Exchange rates via frankfurter.app (ECB, free, no key). */
static char *tool_get_exchange_rate(const char *args_json)
{
    char from[8] = "EUR", to[8] = "RON";
    cJSON *args = cJSON_Parse(args_json);
    cJSON *fj = args ? cJSON_GetObjectItem(args, "from") : NULL;
    cJSON *tj = args ? cJSON_GetObjectItem(args, "to") : NULL;
    if (cJSON_IsString(fj) && strlen(fj->valuestring) == 3) {
        for (int i = 0; i < 3; i++) from[i] = (char)toupper((unsigned char)fj->valuestring[i]);
        from[3] = '\0';
    }
    if (cJSON_IsString(tj) && strlen(tj->valuestring) == 3) {
        for (int i = 0; i < 3; i++) to[i] = (char)toupper((unsigned char)tj->valuestring[i]);
        to[3] = '\0';
    }
    cJSON_Delete(args);

    char url[128];
    snprintf(url, sizeof(url), "https://api.frankfurter.app/latest?from=%s&to=%s", from, to);
    char *res = http_get_text(url, 1024);
    if (!res) {
        return strdup("{\"error\":\"serviciu curs valutar indisponibil\"}");
    }
    return res;
}

static cJSON *build_ai_tools(void)
{
    return cJSON_Parse(
        "[{\"type\":\"function\",\"name\":\"get_weather\","
        "\"description\":\"Vremea curenta si prognoza pe 3 zile pentru o localitate (Open-Meteo). weather_code este cod WMO.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\","
        "\"description\":\"Numele localitatii, de ex. Bucuresti\"}},\"required\":[\"city\"]}},"
        "{\"type\":\"function\",\"name\":\"get_exchange_rate\","
        "\"description\":\"Cursul valutar curent intre doua valute, coduri ISO de 3 litere (ex. EUR, RON, USD).\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},"
        "\"to\":{\"type\":\"string\"}},\"required\":[\"from\",\"to\"]}}]");
}

static char *run_ai_tool(const char *name, const char *args_json)
{
    if (strcmp(name, "get_weather") == 0) return tool_get_weather(args_json);
    if (strcmp(name, "get_exchange_rate") == 0) return tool_get_exchange_rate(args_json);
    return strdup("{\"error\":\"functie necunoscuta\"}");
}

static esp_err_t ai_transcribe_wav(const char *path, char *out, size_t out_len)
{
    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open stt wav");
    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    ESP_RETURN_ON_FALSE(file_len > 0, ESP_FAIL, TAG, "empty wav");

    const char *boundary = "----spotpear-ai-boundary";
    char head[512];
    snprintf(head, sizeof(head),
             "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nro\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"question.wav\"\r\n"
             "Content-Type: audio/wav\r\n\r\n",
             boundary, CONFIG_OPENAI_TRANSCRIBE_MODEL, boundary, boundary, boundary);
    char tail[64];
    snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
    int content_len = strlen(head) + file_len + strlen(tail);

    esp_http_client_config_t cfg = {
        .url = "https://api.openai.com/v1/audio/transcriptions",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "stt client");
    add_openai_auth_header(client);
    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t ret = esp_http_client_open(client, content_len);
    if (ret == ESP_OK) {
        esp_http_client_write(client, head, strlen(head));
        char buf[1024];
        size_t r = 0;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
            esp_http_client_write(client, buf, r);
        }
        esp_http_client_write(client, tail, strlen(tail));
        esp_http_client_fetch_headers(client);
        char *response = http_read_text_response(client, 4096);
        cJSON *json = response ? cJSON_Parse(response) : NULL;
        cJSON *text = json ? cJSON_GetObjectItem(json, "text") : NULL;
        if (cJSON_IsString(text) && text->valuestring[0]) {
            strlcpy(out, text->valuestring, out_len);
        } else {
            ret = ESP_FAIL;
        }
        cJSON_Delete(json);
        free(response);
    }
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static esp_err_t openai_responses_post(const char *body, char **response_out)
{
    esp_http_client_config_t cfg = {
        .url = "https://api.openai.com/v1/responses",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    add_openai_auth_header(client);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t ret = esp_http_client_open(client, strlen(body));
    if (ret == ESP_OK) {
        esp_http_client_write(client, body, strlen(body));
        esp_http_client_fetch_headers(client);
        *response_out = http_read_text_response(client, 8192);
        if (!*response_out) {
            ret = ESP_FAIL;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static bool extract_output_text(cJSON *json, char *answer, size_t answer_len)
{
    cJSON *output_text = cJSON_GetObjectItem(json, "output_text");
    if (cJSON_IsString(output_text)) {
        strlcpy(answer, output_text->valuestring, answer_len);
        return true;
    }
    cJSON *output = cJSON_GetObjectItem(json, "output");
    if (!cJSON_IsArray(output)) {
        return false;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, output) {
        cJSON *content = cJSON_GetObjectItem(item, "content");
        if (!cJSON_IsArray(content)) {
            continue;
        }
        cJSON *part = NULL;
        cJSON_ArrayForEach(part, content) {
            cJSON *text = cJSON_GetObjectItem(part, "text");
            if (cJSON_IsString(text)) {
                strlcpy(answer, text->valuestring, answer_len);
                return true;
            }
        }
    }
    return false;
}

#define AI_MAX_TOOL_ROUNDS 3

static esp_err_t ai_chat_romanian(const char *question, char *answer, size_t answer_len)
{
    char sysmsg[300];
    char timestr[48] = "necunoscuta";
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    if (lt.tm_year + 1900 >= 2024) {
        strftime(timestr, sizeof(timestr), "%A %Y-%m-%d %H:%M", &lt);
    }
    snprintf(sysmsg, sizeof(sysmsg),
             "Esti un asistent vocal pe un dispozitiv ESP32 aflat in Romania. Data si ora locala: %s. "
             "Raspunde concis, natural si numai in limba romana. "
             "Pentru vreme sau curs valutar foloseste functiile disponibile.", timestr);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_OPENAI_CHAT_MODEL);
    cJSON_AddItemToObject(root, "tools", build_ai_tools());
    cJSON *input = cJSON_AddArrayToObject(root, "input");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", sysmsg);
    cJSON_AddItemToArray(input, sys);
    cJSON *usr = cJSON_CreateObject();
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", question);
    cJSON_AddItemToArray(input, usr);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(body, ESP_ERR_NO_MEM, TAG, "chat body");

    esp_err_t ret = ESP_FAIL;
    for (int round = 0; round < AI_MAX_TOOL_ROUNDS && body; round++) {
        char *response = NULL;
        ret = openai_responses_post(body, &response);
        free(body);
        body = NULL;
        if (ret != ESP_OK || !response) {
            free(response);
            return ESP_FAIL;
        }
        cJSON *json = cJSON_Parse(response);
        free(response);
        if (!json) {
            return ESP_FAIL;
        }

        cJSON *idj = cJSON_GetObjectItem(json, "id");
        const char *resp_id = cJSON_IsString(idj) ? idj->valuestring : NULL;

        /* execute any requested tool calls, then continue the response chain */
        cJSON *next_root = NULL;
        cJSON *next_input = NULL;
        cJSON *output = cJSON_GetObjectItem(json, "output");
        if (cJSON_IsArray(output) && resp_id && round < AI_MAX_TOOL_ROUNDS - 1) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, output) {
                cJSON *type = cJSON_GetObjectItem(item, "type");
                if (!cJSON_IsString(type) || strcmp(type->valuestring, "function_call") != 0) {
                    continue;
                }
                cJSON *name = cJSON_GetObjectItem(item, "name");
                cJSON *cargs = cJSON_GetObjectItem(item, "arguments");
                cJSON *call_id = cJSON_GetObjectItem(item, "call_id");
                if (!cJSON_IsString(name) || !cJSON_IsString(call_id)) {
                    continue;
                }
                if (!next_root) {
                    ui_set_subtitle("Verific pe internet...");
                    next_root = cJSON_CreateObject();
                    cJSON_AddStringToObject(next_root, "model", CONFIG_OPENAI_CHAT_MODEL);
                    cJSON_AddItemToObject(next_root, "tools", build_ai_tools());
                    cJSON_AddStringToObject(next_root, "previous_response_id", resp_id);
                    next_input = cJSON_AddArrayToObject(next_root, "input");
                }
                ESP_LOGI(TAG, "AI tool call: %s(%s)", name->valuestring,
                         cJSON_IsString(cargs) ? cargs->valuestring : "{}");
                char *tool_out = run_ai_tool(name->valuestring,
                                             cJSON_IsString(cargs) ? cargs->valuestring : "{}");
                cJSON *fo = cJSON_CreateObject();
                cJSON_AddStringToObject(fo, "type", "function_call_output");
                cJSON_AddStringToObject(fo, "call_id", call_id->valuestring);
                cJSON_AddStringToObject(fo, "output", tool_out ? tool_out : "{}");
                free(tool_out);
                cJSON_AddItemToArray(next_input, fo);
            }
        }
        if (next_root) {
            body = cJSON_PrintUnformatted(next_root);
            cJSON_Delete(next_root);
            cJSON_Delete(json);
            ret = ESP_FAIL;  /* answer comes from the next round */
            continue;
        }

        ret = extract_output_text(json, answer, answer_len) ? ESP_OK : ESP_FAIL;
        cJSON_Delete(json);
        break;
    }
    free(body);
    return ret;
}

static esp_err_t ai_tts_to_mp3(const char *text, const char *path)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_OPENAI_TTS_MODEL);
    cJSON_AddStringToObject(root, "voice", s_ai_voices[s_ai_voice_index]);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "response_format", "mp3");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(body, ESP_ERR_NO_MEM, TAG, "tts body");

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(body);
        return ESP_FAIL;
    }
    esp_http_client_config_t cfg = {
        .url = "https://api.openai.com/v1/audio/speech",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 45000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        free(body);
        return ESP_FAIL;
    }
    add_openai_auth_header(client);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t ret = esp_http_client_open(client, strlen(body));
    if (ret == ESP_OK) {
        esp_http_client_write(client, body, strlen(body));
        esp_http_client_fetch_headers(client);
        char buf[1024];
        int r = 0;
        while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
            fwrite(buf, 1, r, f);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(f);
    free(body);
    return ret;
}

static void ai_task(void *arg)
{
    (void)arg;
    char question[512] = {0};
    char answer[1024] = {0};

    if (s_openai_api_key[0] == '\0') {
        ui_set_title("Lipseste cheia API");
        ui_set_subtitle("Pune openai.txt pe cardul SD");
        ui_set_status("Eroare");
        s_ai_task = NULL;
        vTaskDelete(NULL);
    }

    stop_playback();
    s_mode = UI_MODE_AI;
    ui_set_mode(UI_MODE_AI);
    ui_set_ai_phase(UI_AI_LISTENING);
    ui_set_title("Ascult...");
    ui_set_subtitle("Vorbeste acum");
    ui_set_status("Inregistrare");
    esp_err_t rec = ai_record_wav(AI_WAV_PATH);
    if (rec != ESP_OK) {
        ui_set_ai_phase(UI_AI_IDLE);
        ui_set_title(rec == ESP_ERR_TIMEOUT ? "Nu am auzit nimic" : "Eroare microfon");
        ui_set_subtitle(rec == ESP_ERR_TIMEOUT ? "Incearca din nou" : "");
        ui_set_status(rec == ESP_ERR_TIMEOUT ? "Apasa microfonul" : "Eroare");
        s_ai_task = NULL;
        vTaskDelete(NULL);
    }

    ui_set_ai_phase(UI_AI_THINKING);
    ui_set_title("Ma gandesc...");
    ui_set_subtitle("Transcriu intrebarea");
    ui_set_status("Transcriere");
    if (ai_transcribe_wav(AI_WAV_PATH, question, sizeof(question)) != ESP_OK) {
        ui_set_ai_phase(UI_AI_IDLE);
        ui_set_title("Eroare transcriere");
        ui_set_status("Eroare");
        s_ai_task = NULL;
        vTaskDelete(NULL);
    }

    ui_set_subtitle(question);
    ui_set_status("AI");
    if (ai_chat_romanian(question, answer, sizeof(answer)) != ESP_OK) {
        ui_set_ai_phase(UI_AI_IDLE);
        ui_set_title("Eroare AI");
        ui_set_status("Eroare");
        s_ai_task = NULL;
        vTaskDelete(NULL);
    }

    ui_set_title("Vorbesc...");
    ui_set_subtitle(s_ai_voices[s_ai_voice_index]);
    ui_set_status("Sinteza vocala");
    if (ai_tts_to_mp3(answer, AI_TTS_PATH) != ESP_OK) {
        ui_set_ai_phase(UI_AI_IDLE);
        ui_set_title("Eroare TTS");
        ui_set_status("Eroare");
        s_ai_task = NULL;
        vTaskDelete(NULL);
    }

    FILE *fp = board_open_file(AI_TTS_PATH);
    if (fp && audio_player_play(fp) == ESP_OK) {
        s_playing = true;
        s_paused = false;
        ui_set_ai_phase(UI_AI_SPEAKING);
        ui_set_playing(true);
        ui_set_status("Redare");
    } else {
        if (fp) {
            fclose(fp);
        }
        ui_set_ai_phase(UI_AI_IDLE);
        ui_set_status("Eroare redare");
    }
    s_ai_task = NULL;
    vTaskDelete(NULL);
}

static void ai_start_conversation(void)
{
    if (s_ai_task) {
        return;
    }
    xTaskCreatePinnedToCore(ai_task, "ai_task", 12288, NULL, 4, &s_ai_task, 1);
}

static void load_radio_stations(void)
{
    FILE *f = fopen("/sdcard/radio.txt", "r");
    if (f) {
        char line[384];
        while (fgets(line, sizeof(line), f) && s_station_count < MAX_STATIONS) {
            char *p = trim(line);
            if (p[0] == '\0' || p[0] == '#') {
                continue;
            }
            char *sep = strchr(p, '|');
            if (!sep) {
                continue;
            }
            *sep++ = '\0';
            strlcpy(s_stations[s_station_count].name, trim(p), sizeof(s_stations[0].name));
            strlcpy(s_stations[s_station_count].url, trim(sep), sizeof(s_stations[0].url));
            s_station_count++;
        }
        fclose(f);
    }
    if (s_station_count == 0 && CONFIG_RADIO_DEFAULT_STREAM_URL[0] != '\0') {
        strlcpy(s_stations[0].name, CONFIG_RADIO_DEFAULT_STREAM_NAME, sizeof(s_stations[0].name));
        strlcpy(s_stations[0].url, CONFIG_RADIO_DEFAULT_STREAM_URL, sizeof(s_stations[0].url));
        s_station_count = 1;
    }
}

static esp_err_t save_radio_stations_to_sd(void)
{
    FILE *f = fopen("/sdcard/radio.txt", "w");
    if (!f) {
        return ESP_FAIL;
    }
    for (int i = 0; i < s_station_count; ++i) {
        fprintf(f, "%s|%s\n", s_stations[i].name, s_stations[i].url);
    }
    fclose(f);
    return ESP_OK;
}

static void save_stations_to_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "st_count", (uint8_t)s_station_count);
    char key[16];
    for (int i = 0; i < s_station_count; i++) {
        snprintf(key, sizeof(key), "st%dn", i);
        nvs_set_str(nvs, key, s_stations[i].name);
        snprintf(key, sizeof(key), "st%du", i);
        nvs_set_str(nvs, key, s_stations[i].url);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_stations_from_nvs(void)
{
    if (s_station_count > 0) return;  /* SD already loaded stations */
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t count = 0;
    if (nvs_get_u8(nvs, "st_count", &count) != ESP_OK || count == 0) {
        nvs_close(nvs);
        return;
    }
    char key[8];
    for (int i = 0; i < (int)count && s_station_count < MAX_STATIONS; i++) {
        size_t name_len = sizeof(s_stations[s_station_count].name);
        size_t url_len  = sizeof(s_stations[s_station_count].url);
        snprintf(key, sizeof(key), "st%dn", i);
        nvs_get_str(nvs, key, s_stations[s_station_count].name, &name_len);
        snprintf(key, sizeof(key), "st%du", i);
        nvs_get_str(nvs, key, s_stations[s_station_count].url, &url_len);
        if (s_stations[s_station_count].url[0] != '\0') {
            s_station_count++;
        }
    }
    nvs_close(nvs);
    if (s_station_count > 0) {
        ESP_LOGI(TAG, "Loaded %d station(s) from NVS", s_station_count);
    }
}

static void save_player_state(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "mode", (uint8_t)s_mode);
    nvs_set_i32(nvs, "track_idx", (int32_t)s_track_index);
    nvs_set_i32(nvs, "station_idx", (int32_t)s_station_index);
    nvs_set_u8(nvs, "volume", board_audio_get_volume());
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_player_state(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t mode = (uint8_t)UI_MODE_RADIO;
    nvs_get_u8(nvs, "mode", &mode);
    s_mode = (ui_mode_t)mode;
    int32_t idx = 0;
    if (nvs_get_i32(nvs, "track_idx", &idx) == ESP_OK) {
        s_track_index = (s_track_count > 0 && idx < s_track_count) ? (int)idx : 0;
    }
    if (nvs_get_i32(nvs, "station_idx", &idx) == ESP_OK) {
        s_station_index = (s_station_count > 0 && idx < s_station_count) ? (int)idx : 0;
    }
    uint8_t vol = 60;
    nvs_get_u8(nvs, "volume", &vol);
    board_audio_set_volume(vol);
    ui_set_volume(vol);
    uint16_t saver_s = 0;
    if (nvs_get_u16(nvs, "saver_s", &saver_s) == ESP_OK) {
        s_saver_timeout_ms = (uint32_t)saver_s * 1000;
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "Player state restored: mode=%d track=%d station=%d vol=%d",
             s_mode, s_track_index, s_station_index, vol);
}

static esp_err_t add_radio_station(const char *name, const char *url)
{
    if (!url || url[0] == '\0' || s_station_count >= MAX_STATIONS) {
        return ESP_ERR_INVALID_ARG;
    }
    station_t *station = &s_stations[s_station_count];
    strlcpy(station->name, name && name[0] ? name : "Radio Stream", sizeof(station->name));
    strlcpy(station->url, url, sizeof(station->url));
    s_station_count++;
    if (save_radio_stations_to_sd() != ESP_OK) {
        ESP_LOGW(TAG, "SD station save failed, using NVS only");
    }
    save_stations_to_nvs();
    return ESP_OK;
}

static esp_err_t delete_radio_station(int index)
{
    if (index < 0 || index >= s_station_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mode == UI_MODE_RADIO && s_station_index == index && s_playing) {
        stop_playback();
    }
    for (int i = index; i < s_station_count - 1; ++i) {
        s_stations[i] = s_stations[i + 1];
    }
    s_station_count--;
    if (s_station_index >= s_station_count) {
        s_station_index = s_station_count > 0 ? s_station_count - 1 : 0;
    }
    set_now_playing();
    if (save_radio_stations_to_sd() != ESP_OK) {
        ESP_LOGW(TAG, "SD station save failed after delete, using NVS only");
    }
    save_stations_to_nvs();
    return ESP_OK;
}

/* /sdcard/openai.txt: "key=sk-..." or the bare key on the first non-empty line.
 * Keeps the key off the firmware image and out of version control. */
static bool load_openai_key_from_sd(void)
{
    FILE *f = fopen("/sdcard/openai.txt", "r");
    if (!f) {
        return false;
    }
    bool found = false;
    char line[256];
    while (!found && fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (p[0] == '\0' || p[0] == '#') {
            continue;
        }
        if (strncmp(p, "key=", 4) == 0) {
            p = trim(p + 4);
        }
        if (p[0] != '\0') {
            strlcpy(s_openai_api_key, p, sizeof(s_openai_api_key));
            found = true;
        }
    }
    fclose(f);
    return found;
}

static bool load_wifi_from_sd(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    FILE *f = fopen("/sdcard/wifi.txt", "r");
    if (!f) {
        return false;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (strncmp(p, "ssid=", 5) == 0) {
            strlcpy(ssid, trim(p + 5), ssid_len);
        } else if (strncmp(p, "password=", 9) == 0) {
            strlcpy(pass, trim(p + 9), pass_len);
        }
    }
    fclose(f);
    return ssid[0] != '\0';
}

static bool load_wifi_from_nvs(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t ret = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (ret == ESP_OK) {
        nvs_get_str(nvs, "password", pass, &pass_len);
    }
    nvs_close(nvs);
    return ret == ESP_OK && ssid[0] != '\0';
}

static esp_err_t save_wifi_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open wifi nvs");
    esp_err_t ret = nvs_set_str(nvs, "ssid", ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, "password", pass);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *s)
{
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0) {
            *dst++ = (char)((hex_value(src[1]) << 4) | hex_value(src[2]));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool form_get_value(char *body, const char *key, char *out, size_t out_len)
{
    char *save = NULL;
    for (char *token = strtok_r(body, "&", &save); token; token = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(token, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';
        url_decode(token);
        url_decode(eq);
        if (strcmp(token, key) == 0) {
            strlcpy(out, eq, out_len);
            return out[0] != '\0';
        }
    }
    return false;
}

static esp_err_t send_json_string(httpd_req_t *req, const char *s)
{
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "\""), TAG, "json chunk");
    while (s && *s) {
        char out[8];
        unsigned char c = (unsigned char)*s;
        if (*s == '"' || *s == '\\') {
            out[0] = '\\';
            out[1] = *s;
            out[2] = '\0';
            ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, out), TAG, "json escape");
        } else if (c < 0x20) {
            snprintf(out, sizeof(out), "\\u%04x", c);
            ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, out), TAG, "json ctrl");
        } else {
            out[0] = *s;
            out[1] = '\0';
            ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, out), TAG, "json char");
        }
        s++;
    }
    return httpd_resp_sendstr_chunk(req, "\"");
}

static esp_err_t send_json_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t send_json_error(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"ok\":false,\"error\":");
    send_json_string(req, message);
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t web_root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html lang='ro'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Radio &amp; MP3 - ESP32</title>"
        "<link rel='icon' href='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22>"
        "<circle cx=%2250%22 cy=%2250%22 r=%2248%22 fill=%22%23319ce9%22/>"
        "<text x=%2250%22 y=%2270%22 font-size=%2254%22 text-anchor=%22middle%22>%E2%99%AA</text></svg>'>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{margin:0;background:#0a1420;color:#dfe9ef;font-family:system-ui,'Segoe UI',Roboto,sans-serif;padding-bottom:40px}"
        "header{position:sticky;top:0;z-index:5;background:linear-gradient(165deg,#123049,#0a1420 90%);border-bottom:1px solid #1c3348;padding:12px 16px 14px}"
        ".hin{max-width:720px;margin:auto}"
        ".top{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}"
        ".brand{font-weight:600;font-size:15px;color:#bcd3e0}"
        ".badge{font-size:12px;color:#7bd88f}"
        "#ttl{font-size:20px;font-weight:600;color:#fff;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
        "#sub{color:#7fd4f2;font-size:14px;min-height:18px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
        "#stat{color:#55c8ee;font-size:11px;letter-spacing:.05em;margin-top:2px}"
        ".tiny{font-size:12px;color:#8aa0ad}"
        ".mut{color:#8aa0ad;font-size:13px;word-break:break-all}"
        "#prog{height:4px;background:#1c3348;border-radius:2px;margin-top:8px;overflow:hidden}"
        "#prog i{display:block;height:100%;width:0;background:#e8a13a;transition:width .5s}"
        ".ctl{display:flex;align-items:center;gap:10px;margin-top:12px}"
        "button{border:0;border-radius:10px;background:#319ce9;color:#04121d;font-size:14px;font-weight:600;padding:9px 14px;cursor:pointer}"
        "button:active{transform:scale(.96)}"
        "button.alt2{background:#24404f;color:#cfe2ec}"
        "button.ico{width:42px;height:42px;border-radius:50%;padding:0;background:#16344a;color:#dfe9ef;font-size:16px;flex:none}"
        "button.ico.big{width:52px;height:52px;background:#319ce9;color:#04121d;font-size:20px}"
        "button.sm{padding:6px 10px;font-size:13px;border-radius:8px}"
        ".ctl input[type=range]{flex:1;accent-color:#319ce9;min-width:60px}"
        "main{max-width:720px;margin:14px auto;padding:0 12px;display:grid;gap:14px}"
        "section{background:#101f2e;border:1px solid #1c3348;border-radius:14px;padding:14px 16px}"
        "h2{margin:0 0 10px;font-size:12px;letter-spacing:.08em;color:#6fb9de;text-transform:uppercase}"
        "label{display:block;margin:10px 0 4px;color:#8aa0ad;font-size:13px}"
        "input:not([type=range]),select{width:100%;background:#0b1723;border:1px solid #1c3348;color:#dfe9ef;border-radius:9px;padding:10px 12px;font-size:14px}"
        ".row{display:flex;gap:8px;align-items:center;margin:6px 0}.row input{flex:1}"
        ".item{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:9px 10px;border-bottom:1px solid #14202c;border-radius:8px}"
        ".item:last-child{border-bottom:0}"
        ".item.active{background:#12283a;box-shadow:inset 3px 0 0 #319ce9}"
        ".item .nm{font-size:14px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
        ".item em{color:#55c8ee;font-style:normal;font-size:11px}"
        ".item .btns{flex:none;display:flex;gap:6px}"
        "details summary{cursor:pointer;color:#9fc2d8;font-size:14px;margin:6px 0}"
        "#toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%) translateY(80px);background:#0f2f1e;color:#7bd88f;border:1px solid #1e5c38;padding:10px 18px;border-radius:22px;font-size:14px;transition:.25s;opacity:0;z-index:9;max-width:90vw}"
        "#toast.show{opacity:1;transform:translateX(-50%)}"
        "#toast.err{background:#331418;color:#f09a9a;border-color:#7a2a30}"
        "</style></head><body>"
        "<header><div class='hin'>"
        "<div class='top'><span class='brand'>&#127911; Radio &amp; MP3</span><span class='badge' id='batt'></span></div>"
        "<div id='ttl'>-</div><div id='sub' class='mut'></div><div id='stat' class='tiny'></div>"
        "<div id='prog' hidden><i id='progi'></i></div>"
        "<div class='ctl'>"
        "<button class='ico' onclick=\"post('/api/prev')\" aria-label='Anterior'>&#9198;</button>"
        "<button class='ico big' id='pp' onclick=\"post('/api/playpause')\" aria-label='Redare/Pauza'>&#9654;</button>"
        "<button class='ico' onclick=\"post('/api/next')\" aria-label='Urmator'>&#9197;</button>"
        "<button class='ico alt2' onclick=\"post('/api/stop','','Oprit')\" aria-label='Stop'>&#9209;</button>"
        "<span class='tiny'>&#128266;</span><input type='range' id='vol' min='0' max='100' value='50'>"
        "<span class='tiny' id='volv'></span>"
        "</div></div></header>"
        "<main>"
        "<section><h2>Posturi salvate</h2><div id='stations'></div></section>"
        "<section><h2>Cauta post radio</h2>"
        "<div class='row'><input id='q' placeholder='nume sau gen, ex. jazz'><button onclick='searchRadio()'>Cauta</button></div>"
        "<div id='results'></div>"
        "<details><summary>Adauga URL direct</summary>"
        "<label>URL stream</label><input id='url' placeholder='http://.../stream.mp3'>"
        "<label>Nume</label><input id='name' placeholder='Numele postului'>"
        "<div class='row'><button class='alt2' onclick='testUrl()'>Testeaza</button><button onclick='saveUrl()'>Salveaza</button></div>"
        "</details></section>"
        "<section><h2>Melodii MP3</h2><div id='tracks'></div>"
        "<div class='row'><input type='file' id='mp3file' accept='.mp3' multiple><button onclick='uploadMp3()'>Incarca</button></div>"
        "<div id='upst' class='tiny'></div></section>"
        "<section><h2>Setari</h2>"
        "<label>Screensaver dupa</label><select id='saver' onchange='saveSaver()'>"
        "<option value='0'>Niciodata</option><option value='15'>15 secunde</option><option value='30'>30 secunde</option>"
        "<option value='60'>1 minut</option><option value='120'>2 minute</option><option value='300'>5 minute</option></select>"
        "<label>Cheie OpenAI (<span id='keyst'>-</span>)</label>"
        "<div class='row'><input id='aikey' type='password' placeholder='sk-...'><button onclick='saveKey()'>Salveaza</button></div>"
        "<div class='tiny' id='ipinfo'></div></section>"
        "<section><details><summary>Setari WiFi</summary><form method='post' action='/save'>"
        "<label>SSID</label><input name='ssid' maxlength='32' required>"
        "<label>Parola</label><input name='password' type='password' maxlength='64'>"
        "<div class='row'><button type='submit' onclick=\"return confirm('Salvezi WiFi si repornesti placa?')\">Salveaza si reporneste</button></div>"
        "</form></details></section>"
        "</main><div id='toast'></div>"
        "<script>"
        "const $=i=>document.getElementById(i);"
        "let ST={},STATIONS=[],TRACKS=[],FOUND=[],volDrag=false;"
        "const MODES={radio:'RADIO',mp3:'MP3',ai:'ASISTENT AI'};"
        "const STATES={playing:'Redare',paused:'Pauza',stopped:'Oprit'};"
        "function esc(s){return String(s||'').replace(/[&<>\\\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\\\"':'&quot;',\"'\":'&#39;'}[c]));}"
        "function toast(m,err){let t=$('toast');t.textContent=m;t.className='show'+(err?' err':'');clearTimeout(t._h);t._h=setTimeout(()=>t.className='',2600);}"
        "async function post(u,o,msg){try{let r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(o||{})});"
        "let j=await r.json().catch(()=>({}));"
        "if(!r.ok||j.ok===false)toast(j.error||'Eroare',1);else if(msg)toast(msg);"
        "await loadState();}catch(e){toast('Fara conexiune',1);}}"
        "async function loadState(){try{ST=await(await fetch('/api/state')).json();renderState();renderLists();}catch(e){}}"
        "function renderState(){"
        "$('ttl').textContent=ST.title||'-';"
        "$('sub').textContent=ST.subtitle||'';"
        "$('stat').textContent=(MODES[ST.mode]||'')+' \\u00b7 '+(STATES[ST.state]||'');"
        "$('pp').innerHTML=ST.state=='playing'?'&#9208;':'&#9654;';"
        "$('batt').textContent=ST.battery>=0?'\\uD83D\\uDD0B '+ST.battery+'%':'';"
        "if(!volDrag){$('vol').value=ST.volume;$('volv').textContent=ST.volume;}"
        "let p=$('prog');if(ST.progress>=0){p.hidden=false;$('progi').style.width=ST.progress+'%';}else p.hidden=true;}"
        "function itemHtml(name,active,btns){return `<div class='item${active?' active':''}'><span class='nm'>${name}${active?' <em>\\u00b7 acum</em>':''}</span><span class='btns'>${btns}</span></div>`;}"
        "function renderLists(){"
        "$('stations').innerHTML=STATIONS.map((s,i)=>itemHtml(esc(s.name),ST.mode=='radio'&&ST.station_idx==i,"
        "`<button class='sm' onclick='post(\\\"/api/radio/play\\\",{idx:${i}})'>&#9654;</button><button class='sm alt2' onclick='delStation(${i})'>&#128465;</button>`)).join('')"
        "||\"<div class='mut'>Niciun post salvat</div>\";"
        "$('tracks').innerHTML=TRACKS.map((t,i)=>itemHtml(esc(t.name),ST.mode=='mp3'&&ST.track_idx==i,"
        "`<button class='sm' onclick='post(\\\"/api/mp3/play\\\",{idx:${i}})'>&#9654;</button><button class='sm alt2' onclick='delTrack(${i})'>&#128465;</button>`)).join('')"
        "||\"<div class='mut'>Niciun fisier MP3</div>\";}"
        "async function loadStations(){try{STATIONS=await(await fetch('/api/stations')).json();renderLists();}catch(e){}}"
        "async function loadTracks(){try{TRACKS=await(await fetch('/api/tracks')).json();renderLists();}catch(e){}}"
        "function delStation(i){let s=STATIONS[i];if(!s)return;"
        "if(confirm('Stergi postul \\\"'+s.name+'\\\"?'))post('/api/radio/delete',{idx:i},'Post sters').then(loadStations);}"
        "async function searchRadio(){$('results').innerHTML=\"<div class='mut'>Caut...</div>\";"
        "try{let q=encodeURIComponent($('q').value);"
        "let r=await fetch('https://de1.api.radio-browser.info/json/stations/search?limit=20&hidebroken=true&name='+q);"
        "FOUND=await r.json();"
        "$('results').innerHTML=FOUND.map((s,i)=>itemHtml(esc(s.name),false,"
        "`<button class='sm alt2' onclick='testFound(${i})'>Test</button><button class='sm' onclick='saveFound(${i})'>Salveaza</button>`)).join('')"
        "||\"<div class='mut'>Niciun rezultat</div>\";}"
        "catch(e){$('results').innerHTML=\"<div class='mut'>Cautarea a esuat</div>\";}}"
        "function testFound(i){let s=FOUND[i];post('/api/radio/test',{name:s.name,url:s.url_resolved||s.url},'Test pornit');}"
        "function saveFound(i){let s=FOUND[i];post('/api/radio/save',{name:s.name,url:s.url_resolved||s.url},'Post salvat').then(loadStations);}"
        "function testUrl(){post('/api/radio/test',{name:$('name').value,url:$('url').value},'Test pornit');}"
        "function saveUrl(){post('/api/radio/save',{name:$('name').value,url:$('url').value},'Post salvat').then(loadStations);}"
        "async function loadSettings(){try{let s=await(await fetch('/api/settings')).json();"
        "$('saver').value=String(s.saver);$('keyst').textContent=s.ai_key_set?'setata':'nesetata';"
        "$('ipinfo').textContent=s.ip?('Adresa dispozitivului: http://'+s.ip):'';}catch(e){}}"
        "function saveSaver(){post('/api/settings',{saver:$('saver').value},'Setare salvata');}"
        "function saveKey(){let k=$('aikey').value.trim();if(!k)return toast('Introdu cheia',1);"
        "post('/api/openai_key',{key:k},'Cheie salvata').then(loadSettings);$('aikey').value='';}"
        "function delTrack(i){let t=TRACKS[i];if(!t)return;"
        "if(confirm('Stergi fisierul \\\"'+t.name+'\\\" de pe card?'))post('/api/mp3/delete',{idx:i},'Fisier sters').then(()=>setTimeout(loadTracks,600));}"
        "function uploadMp3(){let files=$('mp3file').files;if(!files.length)return toast('Alege fisiere MP3',1);upNext(files,0);}"
        "function upNext(files,i){if(i>=files.length){$('upst').textContent='';$('mp3file').value='';toast('Incarcare completa');setTimeout(loadTracks,600);return;}"
        "let f=files[i];if(!/\\.mp3$/i.test(f.name)){toast(f.name+': nu e MP3',1);upNext(files,i+1);return;}"
        "let x=new XMLHttpRequest();x.open('POST','/api/mp3/upload?name='+encodeURIComponent(f.name));"
        "x.upload.onprogress=e=>{if(e.total)$('upst').textContent='Incarc '+(i+1)+'/'+files.length+' ('+f.name+'): '+Math.round(e.loaded*100/e.total)+'%';};"
        "x.onload=()=>{if(x.status!=200)toast('Eroare la '+f.name,1);upNext(files,i+1);};"
        "x.onerror=()=>{toast('Eroare la '+f.name,1);upNext(files,i+1);};"
        "x.send(f);}"
        "$('vol').addEventListener('input',()=>{volDrag=true;$('volv').textContent=$('vol').value;});"
        "$('vol').addEventListener('change',()=>{post('/api/volume',{vol:$('vol').value});setTimeout(()=>volDrag=false,600);});"
        "$('q').addEventListener('keydown',e=>{if(e.key=='Enter')searchRadio();});"
        "loadState();loadStations();loadTracks();loadSettings();setInterval(loadState,2000);"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_state_get_handler(httpd_req_t *req)
{
    char title[sizeof(s_now_title)];
    char subtitle[sizeof(s_now_subtitle)];
    portENTER_CRITICAL(&s_metadata_lock);
    strlcpy(title, s_now_title, sizeof(title));
    strlcpy(subtitle, s_now_subtitle, sizeof(subtitle));
    portEXIT_CRITICAL(&s_metadata_lock);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"mode\":");
    send_json_string(req, s_mode == UI_MODE_RADIO ? "radio" : (s_mode == UI_MODE_MP3 ? "mp3" : "ai"));
    httpd_resp_sendstr_chunk(req, ",\"state\":");
    send_json_string(req, s_paused ? "paused" : (s_playing ? "playing" : "stopped"));
    httpd_resp_sendstr_chunk(req, ",\"title\":");
    send_json_string(req, title);
    httpd_resp_sendstr_chunk(req, ",\"subtitle\":");
    send_json_string(req, subtitle);
    int progress = (s_mode == UI_MODE_MP3 && s_playing && s_mp3_progress <= 100) ? s_mp3_progress : -1;
    char tail[128];
    snprintf(tail, sizeof(tail),
             ",\"volume\":%u,\"battery\":%d,\"station_idx\":%d,\"track_idx\":%d,\"progress\":%d}",
             board_audio_get_volume(),
             s_battery_cache > 100 ? -1 : (int)s_battery_cache,
             s_mode == UI_MODE_RADIO ? s_station_index : -1,
             s_mode == UI_MODE_MP3 ? s_track_index : -1,
             progress);
    httpd_resp_sendstr_chunk(req, tail);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t api_tracks_get_handler(httpd_req_t *req)
{
    char buf[32];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < s_track_count; ++i) {
        if (i > 0) {
            httpd_resp_sendstr_chunk(req, ",");
        }
        snprintf(buf, sizeof(buf), "{\"idx\":%d,\"name\":", i);
        httpd_resp_sendstr_chunk(req, buf);
        send_json_string(req, base_name(s_tracks[i]));
        httpd_resp_sendstr_chunk(req, "}");
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t api_stations_get_handler(httpd_req_t *req)
{
    char buf[32];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < s_station_count; ++i) {
        if (i > 0) {
            httpd_resp_sendstr_chunk(req, ",");
        }
        snprintf(buf, sizeof(buf), "{\"idx\":%d,\"name\":", i);
        httpd_resp_sendstr_chunk(req, buf);
        send_json_string(req, s_stations[i].name);
        httpd_resp_sendstr_chunk(req, ",\"url\":");
        send_json_string(req, s_stations[i].url);
        httpd_resp_sendstr_chunk(req, "}");
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t api_radio_test_handler(httpd_req_t *req)
{
    char body[800];
    char name[48]  = {0};
    char url[320]  = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "radio test body");
    char *body_copy = malloc(sizeof(body));
    if (!body_copy) return send_json_error(req, "no memory");
    memcpy(body_copy, body, sizeof(body));
    form_get_value(body, "name", name, sizeof(name));
    bool has_url = form_get_value(body_copy, "url", url, sizeof(url));
    free(body_copy);
    if (!has_url) {
        return send_json_error(req, "missing url");
    }
    play_stream_url(name, url, true);
    return send_json_ok(req);
}

static esp_err_t api_radio_save_handler(httpd_req_t *req)
{
    char body[800];
    char name[48]  = {0};
    char url[320]  = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "radio save body");
    char *body_copy = malloc(sizeof(body));
    if (!body_copy) return send_json_error(req, "no memory");
    memcpy(body_copy, body, sizeof(body));
    form_get_value(body, "name", name, sizeof(name));
    bool has_url = form_get_value(body_copy, "url", url, sizeof(url));
    free(body_copy);
    if (!has_url) {
        return send_json_error(req, "missing url");
    }
    if (add_radio_station(name, url) != ESP_OK) {
        return send_json_error(req, "station not saved");
    }
    return send_json_ok(req);
}

static esp_err_t api_radio_delete_handler(httpd_req_t *req)
{
    char body[64];
    char idx_text[12] = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "radio delete body");
    if (!form_get_value(body, "idx", idx_text, sizeof(idx_text)) || delete_radio_station(atoi(idx_text)) != ESP_OK) {
        return send_json_error(req, "delete failed");
    }
    return send_json_ok(req);
}

static esp_err_t api_radio_play_handler(httpd_req_t *req)
{
    char body[64];
    char idx_text[12] = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "radio play body");
    if (!form_get_value(body, "idx", idx_text, sizeof(idx_text))) {
        return send_json_error(req, "missing index");
    }
    int idx = atoi(idx_text);
    if (idx < 0 || idx >= s_station_count) {
        return send_json_error(req, "bad index");
    }
    s_web_station_index = idx;
    int action = APP_ACTION_WEB_RADIO_PLAY;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) {
        return send_json_error(req, "command queue full");
    }
    return send_json_ok(req);
}

static esp_err_t api_mp3_play_handler(httpd_req_t *req)
{
    char body[64];
    char idx_text[12] = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "mp3 play body");
    if (!form_get_value(body, "idx", idx_text, sizeof(idx_text))) {
        return send_json_error(req, "missing index");
    }
    int idx = atoi(idx_text);
    if (idx < 0 || idx >= s_track_count) {
        return send_json_error(req, "bad index");
    }
    s_web_track_index = idx;
    int action = APP_ACTION_WEB_MP3_PLAY;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) {
        return send_json_error(req, "command queue full");
    }
    return send_json_ok(req);
}

static esp_err_t api_play_pause_handler(httpd_req_t *req)
{
    int action = UI_ACTION_PLAY_PAUSE;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) return send_json_error(req, "command queue full");
    return send_json_ok(req);
}

static esp_err_t api_stop_handler(httpd_req_t *req)
{
    int action = APP_ACTION_WEB_STOP;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) return send_json_error(req, "command queue full");
    return send_json_ok(req);
}

static esp_err_t api_prev_handler(httpd_req_t *req)
{
    int action = UI_ACTION_PREV;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) return send_json_error(req, "command queue full");
    return send_json_ok(req);
}

static esp_err_t api_next_handler(httpd_req_t *req)
{
    int action = UI_ACTION_NEXT;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) return send_json_error(req, "command queue full");
    return send_json_ok(req);
}

static esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"saver\":%u,\"ai_key_set\":%s,\"ip\":\"%s\"}",
             (unsigned)(s_saver_timeout_ms / 1000),
             s_openai_api_key[0] ? "true" : "false",
             s_sta_ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t api_settings_post_handler(httpd_req_t *req)
{
    char body[96];
    char val[12] = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "settings body");
    if (!form_get_value(body, "saver", val, sizeof(val))) {
        return send_json_error(req, "lipseste valoarea");
    }
    int sec = atoi(val);
    if (sec < 0 || sec > 3600) {
        return send_json_error(req, "valoare invalida");
    }
    s_saver_timeout_ms = (uint32_t)sec * 1000;
    nvs_handle_t nvs;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u16(nvs, "saver_s", (uint16_t)sec);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return send_json_ok(req);
}

static esp_err_t api_openai_key_handler(httpd_req_t *req)
{
    char body[320];
    char key[sizeof(s_openai_api_key)] = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "key body");
    if (!form_get_value(body, "key", key, sizeof(key)) || strlen(key) < 10) {
        return send_json_error(req, "cheie invalida");
    }
    strlcpy(s_openai_api_key, key, sizeof(s_openai_api_key));
    FILE *f = fopen("/sdcard/openai.txt", "w");
    if (!f) {
        return send_json_error(req, "cheia e activa doar pana la restart (SD inaccesibil)");
    }
    fprintf(f, "key=%s\n", key);
    fclose(f);
    return send_json_ok(req);
}

static esp_err_t api_mp3_delete_handler(httpd_req_t *req)
{
    char body[64];
    char idx_text[12] = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "mp3 delete body");
    if (!form_get_value(body, "idx", idx_text, sizeof(idx_text))) {
        return send_json_error(req, "missing index");
    }
    int idx = atoi(idx_text);
    if (idx < 0 || idx >= s_track_count) {
        return send_json_error(req, "bad index");
    }
    s_web_delete_index = idx;
    int action = APP_ACTION_WEB_MP3_DELETE;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) {
        return send_json_error(req, "command queue full");
    }
    return send_json_ok(req);
}

static esp_err_t api_mp3_upload_handler(httpd_req_t *req)
{
    char query[200] = {0};
    char name[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return send_json_error(req, "lipseste numele fisierului");
    }
    url_decode(name);
    size_t n = strlen(name);
    if (n < 5 || n > 80 || strcasecmp(name + n - 4, ".mp3") != 0 ||
        strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        return send_json_error(req, "nume invalid (doar .mp3)");
    }
    if (req->content_len <= 0 || req->content_len > 32 * 1024 * 1024) {
        return send_json_error(req, "fisier gol sau prea mare (max 32 MB)");
    }

    char path[112];
    snprintf(path, sizeof(path), "/sdcard/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return send_json_error(req, "nu pot scrie pe cardul SD");
    }
    char *buf = malloc(8192);
    if (!buf) {
        fclose(f);
        unlink(path);
        return send_json_error(req, "no memory");
    }
    int remaining = req->content_len;
    bool fail = false;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 8192 ? 8192 : remaining);
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t)r) {
            fail = true;
            break;
        }
        remaining -= r;
    }
    free(buf);
    fclose(f);
    if (fail) {
        unlink(path);
        return send_json_error(req, "incarcare esuata");
    }
    int action = APP_ACTION_WEB_RESCAN;
    xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100));
    return send_json_ok(req);
}

static esp_err_t api_volume_handler(httpd_req_t *req)
{
    char body[64];
    char vol_text[12] = {0};
    ESP_RETURN_ON_ERROR(read_req_body(req, body, sizeof(body)), TAG, "volume body");
    if (!form_get_value(body, "vol", vol_text, sizeof(vol_text))) {
        return send_json_error(req, "missing vol");
    }
    int v = atoi(vol_text);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_web_volume = v;
    int action = APP_ACTION_WEB_VOLUME;
    if (xQueueSend(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) return send_json_error(req, "command queue full");
    return send_json_ok(req);
}

static esp_err_t wifi_manager_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t wifi_manager_save_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0 && offset < (int)sizeof(body) - 1) {
        int chunk = remaining;
        int space = (int)sizeof(body) - 1 - offset;
        if (chunk > space) {
            chunk = space;
        }
        int read_len = httpd_req_recv(req, body + offset, chunk);
        if (read_len <= 0) {
            return ESP_FAIL;
        }
        offset += read_len;
        remaining -= read_len;
    }

    char ssid_body[192];
    char pass_body[192];
    char ssid[33] = {0};
    char pass[65] = {0};
    strlcpy(ssid_body, body, sizeof(ssid_body));
    strlcpy(pass_body, body, sizeof(pass_body));
    bool has_ssid = form_get_value(ssid_body, "ssid", ssid, sizeof(ssid));
    form_get_value(pass_body, "password", pass, sizeof(pass));

    if (!has_ssid || save_wifi_to_nvs(ssid, pass) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "WiFi save failed");
    }

    httpd_resp_sendstr(req, "Saved. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static esp_err_t wifi_stack_init(void)
{
    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG, "wifi events");
    }

    if (!s_wifi_initialized) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
        esp_err_t ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(ret, TAG, "event loop");
        }
        s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
        s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_wifi_sta_netif && s_wifi_ap_netif, ESP_FAIL, TAG, "wifi netif");

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
        s_wifi_initialized = true;
    }

    if (!s_wifi_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
        s_wifi_handlers_registered = true;
    }
    return ESP_OK;
}

static esp_err_t web_server_start(void)
{
    if (s_web_server_active) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 28;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_wifi_httpd, &cfg), TAG, "web server");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_root_get_handler,
    };
    const httpd_uri_t wifi_save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = wifi_manager_save_handler,
    };
    const httpd_uri_t api_state = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = api_state_get_handler,
    };
    const httpd_uri_t api_tracks = {
        .uri = "/api/tracks",
        .method = HTTP_GET,
        .handler = api_tracks_get_handler,
    };
    const httpd_uri_t api_stations = {
        .uri = "/api/stations",
        .method = HTTP_GET,
        .handler = api_stations_get_handler,
    };
    const httpd_uri_t api_radio_test = {
        .uri = "/api/radio/test",
        .method = HTTP_POST,
        .handler = api_radio_test_handler,
    };
    const httpd_uri_t api_radio_save = {
        .uri = "/api/radio/save",
        .method = HTTP_POST,
        .handler = api_radio_save_handler,
    };
    const httpd_uri_t api_radio_delete = {
        .uri = "/api/radio/delete",
        .method = HTTP_POST,
        .handler = api_radio_delete_handler,
    };
    const httpd_uri_t api_radio_play = {
        .uri = "/api/radio/play",
        .method = HTTP_POST,
        .handler = api_radio_play_handler,
    };
    const httpd_uri_t api_mp3_play = {
        .uri = "/api/mp3/play",
        .method = HTTP_POST,
        .handler = api_mp3_play_handler,
    };
    const httpd_uri_t api_play_pause = {
        .uri = "/api/playpause",
        .method = HTTP_POST,
        .handler = api_play_pause_handler,
    };
    const httpd_uri_t api_settings_get = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = api_settings_get_handler,
    };
    const httpd_uri_t api_settings_post = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = api_settings_post_handler,
    };
    const httpd_uri_t api_openai_key = {
        .uri = "/api/openai_key",
        .method = HTTP_POST,
        .handler = api_openai_key_handler,
    };
    const httpd_uri_t api_mp3_delete = {
        .uri = "/api/mp3/delete",
        .method = HTTP_POST,
        .handler = api_mp3_delete_handler,
    };
    const httpd_uri_t api_mp3_upload = {
        .uri = "/api/mp3/upload",
        .method = HTTP_POST,
        .handler = api_mp3_upload_handler,
    };
    const httpd_uri_t api_prev = {
        .uri = "/api/prev",
        .method = HTTP_POST,
        .handler = api_prev_handler,
    };
    const httpd_uri_t api_next = {
        .uri = "/api/next",
        .method = HTTP_POST,
        .handler = api_next_handler,
    };
    const httpd_uri_t api_volume = {
        .uri = "/api/volume",
        .method = HTTP_POST,
        .handler = api_volume_handler,
    };
    const httpd_uri_t api_stop = {
        .uri = "/api/stop",
        .method = HTTP_POST,
        .handler = api_stop_handler,
    };
    const httpd_uri_t redirect = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = wifi_manager_redirect_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &root), TAG, "register root");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &wifi_save), TAG, "register wifi save");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_state), TAG, "register api state");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_tracks), TAG, "register api tracks");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_stations), TAG, "register api stations");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_radio_test), TAG, "register radio test");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_radio_save), TAG, "register radio save");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_radio_delete), TAG, "register radio delete");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_radio_play), TAG, "register radio play");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_mp3_play), TAG, "register mp3 play");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_play_pause), TAG, "register play pause");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_stop), TAG, "register stop");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_prev), TAG, "register prev");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_next), TAG, "register next");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_volume), TAG, "register volume");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_settings_get), TAG, "register settings get");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_settings_post), TAG, "register settings post");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_openai_key), TAG, "register openai key");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_mp3_delete), TAG, "register mp3 delete");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &api_mp3_upload), TAG, "register mp3 upload");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_wifi_httpd, &redirect), TAG, "register redirect");
    s_web_server_active = true;
    return ESP_OK;
}

static esp_err_t wifi_manager_start(void)
{
    ESP_RETURN_ON_ERROR(wifi_stack_init(), TAG, "wifi stack");
    s_wifi_sta_reconnect = false;

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, WIFI_MANAGER_AP_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, WIFI_MANAGER_AP_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(WIFI_MANAGER_AP_SSID);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen(WIFI_MANAGER_AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi ap mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "wifi ap config");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi ap start");
        s_wifi_started = true;
    }
    ESP_RETURN_ON_ERROR(web_server_start(), TAG, "web server");

    s_wifi_manager_active = true;
    ESP_LOGW(TAG, "WiFi manager active: SSID %s password %s, open http://192.168.4.1", WIFI_MANAGER_AP_SSID, WIFI_MANAGER_AP_PASS);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        if (s_wifi_sta_reconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_show_ip = true;  /* app_loop shows the notice from the UI task */
        ESP_LOGI(TAG, "Web UI active at http://" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_init(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    if (!load_wifi_from_sd(ssid, sizeof(ssid), pass, sizeof(pass)) &&
        !load_wifi_from_nvs(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strlcpy(ssid, CONFIG_RADIO_WIFI_SSID, sizeof(ssid));
        strlcpy(pass, CONFIG_RADIO_WIFI_PASSWORD, sizeof(pass));
    }
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "WiFi not configured");
        return wifi_manager_start();
    }

    ESP_RETURN_ON_ERROR(wifi_stack_init(), TAG, "wifi stack");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    s_wifi_sta_reconnect = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi cfg");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_wifi_started = true;
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect");
    }
    return ESP_OK;
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_sntp_synced = true;
}

static void sntp_start(void)
{
    if (esp_sntp_enabled()) {
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
}

/* Restore system time from the PCF85063 RTC (kept in local time). */
static void clock_init(void)
{
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);  /* Romania */
    tzset();

    uint8_t reg = 0;
    if (I2C_Read(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &reg, 1) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 RTC not detected");
        return;
    }
    s_rtc_present = true;
    PCF85063_Init();

    datetime_t dt;
    PCF85063_Read_Time(&dt);
    if (dt.year >= 2024 && dt.year < 2100 && dt.month >= 1 && dt.month <= 12 &&
        dt.day >= 1 && dt.day <= 31) {
        struct tm tm = {
            .tm_year = dt.year - 1900,
            .tm_mon = dt.month - 1,
            .tm_mday = dt.day,
            .tm_hour = dt.hour,
            .tm_min = dt.minute,
            .tm_sec = dt.second,
            .tm_isdst = -1,
        };
        time_t t = mktime(&tm);
        if (t > 0) {
            struct timeval tv = { .tv_sec = t };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "System time restored from RTC: %04u-%02u-%02u %02u:%02u",
                     dt.year, dt.month, dt.day, dt.hour, dt.minute);
        }
    }
}

static void clock_save_to_rtc(void)
{
    if (!s_rtc_present) {
        return;
    }
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    datetime_t dt = {
        .year = (uint16_t)(lt.tm_year + 1900),
        .month = (uint8_t)(lt.tm_mon + 1),
        .day = (uint8_t)lt.tm_mday,
        .dotw = (uint8_t)lt.tm_wday,
        .hour = (uint8_t)lt.tm_hour,
        .minute = (uint8_t)lt.tm_min,
        .second = (uint8_t)lt.tm_sec,
    };
    PCF85063_Set_All(dt);
    ESP_LOGI(TAG, "RTC updated from NTP");
}

static void app_loop(void)
{
    int last_button = 0;
    int64_t last_bat_ms = 0;
    int64_t last_clock_ms = 0;
    int64_t last_progress_ms = 0;
    bool sntp_started = false;
    while (true) {
        lv_timer_handler();

        int action;
        while (xQueueReceive(s_action_queue, &action, 0) == pdTRUE) {
            handle_action(action);
        }

        if (s_radio_metadata_dirty && s_mode == UI_MODE_RADIO) {
            char station[sizeof(s_radio_station)];
            char title[sizeof(s_radio_title)];
            portENTER_CRITICAL(&s_metadata_lock);
            strlcpy(station, s_radio_station, sizeof(station));
            strlcpy(title, s_radio_title, sizeof(title));
            s_radio_metadata_dirty = false;
            portEXIT_CRITICAL(&s_metadata_lock);
            ui_set_radio_metadata(station, title[0] ? title : "Astept metadata...");
            set_now_strings(station, title);
        }

        bool boot = board_boot_button_pressed();
        if (boot && !last_button) {
            action = UI_ACTION_PLAY_PAUSE;
            xQueueSend(s_action_queue, &action, 0);
        }
        last_button = boot ? 1 : 0;

        /* Battery level â€” read every 30 s */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_bat_ms > 30000) {
            s_battery_cache = board_battery_read_pct();
            ui_set_battery(s_battery_cache);
            last_bat_ms = now_ms;
        }

        /* Clock, WiFi indicator, NTP housekeeping and screensaver â€” every second */
        if (now_ms - last_clock_ms >= 1000) {
            last_clock_ms = now_ms;
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            char clock_buf[8] = "--:--";
            if (lt.tm_year + 1900 >= 2024) {
                snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
                ui_set_clock(clock_buf);
            } else {
                ui_set_clock(NULL);
            }

            bool wifi_ok = s_wifi_events &&
                           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT);
            ui_set_wifi(wifi_ok);
            if (wifi_ok && !sntp_started) {
                sntp_start();
                sntp_started = true;
            }
            if (s_sntp_synced) {
                s_sntp_synced = false;
                clock_save_to_rtc();
            }

            if (s_show_ip) {
                s_show_ip = false;
                char notice[40];
                snprintf(notice, sizeof(notice), "IP: %s", s_sta_ip);
                ui_show_notice(notice);
            }

            /* Screensaver: dim + clock after the configured idle time (wake check runs every loop) */
            uint32_t idle_ms = lv_disp_get_inactive_time(NULL);
            if (!ui_saver_active() && s_saver_timeout_ms > 0 && idle_ms > s_saver_timeout_ms &&
                !(s_mode == UI_MODE_AI && s_ai_task)) {
                ui_show_saver(true);
                Set_Backlight(10);
            }
            if (ui_saver_active()) {
                char line2[32];
                const char *mode_name = s_mode == UI_MODE_RADIO ? "RADIO" :
                                        s_mode == UI_MODE_MP3 ? "MP3" : "AI";
                if (s_battery_cache <= 100) {
                    snprintf(line2, sizeof(line2), "%s  |  %u%%", mode_name,
                             (unsigned)s_battery_cache);
                } else {
                    strlcpy(line2, mode_name, sizeof(line2));
                }
                ui_saver_update(clock_buf, s_playing ? s_now_title : "", line2);
            }
        }

        /* Wake from screensaver immediately on touch */
        if (ui_saver_active() && lv_disp_get_inactive_time(NULL) < 500) {
            ui_show_saver(false);
            Set_Backlight(LCD_Backlight);
        }

        /* MP3 progress arc and buffering spinner â€” every 500 ms */
        if (now_ms - last_progress_ms >= 500) {
            last_progress_ms = now_ms;
            ui_set_progress((s_mode == UI_MODE_MP3 && s_playing)
                                ? s_mp3_progress : UI_PROGRESS_HIDE);
            ui_set_busy(s_radio_buffering && s_playing);
        }

        if (s_player_unknown) {
            s_player_unknown = false;
            s_playing = false;
            s_paused = false;
            ui_set_playing(false);
            ui_set_status("Format invalid");
        }
        if (s_player_idle) {
            s_player_idle = false;
            s_playing = false;
            s_paused = false;
            ui_set_playing(false);
            if (s_mode == UI_MODE_MP3) {
                next_item(1);
                play_current();
            } else if (s_mode == UI_MODE_RADIO) {
                ui_set_status("Reconectare");
                play_current();
            } else {
                /* AI: TTS answer finished — back to idle, no auto-play */
                ui_set_ai_phase(UI_AI_IDLE);
                ui_set_status("Apasa microfonul");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(board_display_init());
    clock_init();
    ESP_ERROR_CHECK(board_lvgl_init());
    s_action_queue = xQueueCreate(10, sizeof(int));
    ESP_ERROR_CHECK(s_action_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ui_create(ui_action_sender);
    ui_set_status("Pornire...");
    ESP_ERROR_CHECK(board_touch_init());
    ESP_ERROR_CHECK(board_lvgl_indev_init());
    ESP_ERROR_CHECK(init_audio_player());
    if (board_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "Battery ADC init failed â€” check CONFIG_BATTERY_ADC_CHANNEL");
    }

    if (board_sd_mount() == ESP_OK) {
        s_track_count = board_find_mp3_files(s_tracks, MAX_TRACKS);
        load_radio_stations();
        if (load_openai_key_from_sd()) {
            ESP_LOGI(TAG, "OpenAI API key loaded from SD card");
        }
    } else {
        ui_set_status("Fara card SD");
    }
    load_stations_from_nvs();
    load_player_state();

    esp_err_t wifi_ret = wifi_init();
    bool wifi_connected = false;
    if (wifi_ret == ESP_OK && !s_wifi_manager_active) {
        ui_set_status("Conectare WiFi...");
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;
        if (wifi_connected) {
            web_server_start();
        } else {
            ui_set_status("Config AP");
            wifi_manager_start();
        }
    }

    set_now_playing();
    s_battery_cache = board_battery_read_pct();
    ui_set_battery(s_battery_cache);
    ui_set_wifi(wifi_connected);
    if (s_wifi_manager_active) {
        ui_set_status("Config AP");
        ui_set_subtitle("WiFi: 192.168.4.1");
    } else {
        ui_set_status(wifi_connected ? "Pregatit" : "Fara WiFi");
    }
    app_loop();
}




