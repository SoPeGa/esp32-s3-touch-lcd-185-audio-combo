#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    UI_MODE_RADIO = 0,
    UI_MODE_MP3   = 1,
    UI_MODE_AI    = 2,
} ui_mode_t;

#define UI_MODE_COUNT 3

typedef void (*ui_action_cb_t)(int action);

enum {
    UI_ACTION_PREV = 1,
    UI_ACTION_PLAY_PAUSE,
    UI_ACTION_NEXT,
    UI_ACTION_MODE_RADIO,
    UI_ACTION_MODE_MP3,
    UI_ACTION_VOL_DOWN,
    UI_ACTION_VOL_UP,
    UI_ACTION_MODE_AI,
    UI_ACTION_AI_VOICE,
    UI_ACTION_VOL_SET,   /* arc drag — read ui_get_arc_volume() for value */
    UI_ACTION_OPEN_LIST, /* long-press — app builds the list and calls ui_show_picker() */
    UI_ACTION_LIST_PICK, /* picker confirmed — read ui_get_picked_index() */
    UI_ACTION_AI_RESET,  /* long-press in AI mode — start a new conversation */
};

#define UI_BATTERY_UNKNOWN 255
#define UI_PROGRESS_HIDE   255

typedef enum {
    UI_AI_IDLE = 0,
    UI_AI_LISTENING,
    UI_AI_THINKING,
    UI_AI_SPEAKING,
} ui_ai_phase_t;

void    ui_create(ui_action_cb_t cb);
void    ui_set_mode(ui_mode_t mode);
void    ui_set_playing(bool playing);
void    ui_set_volume(uint8_t volume);
void    ui_set_battery(uint8_t pct);   /* UI_BATTERY_UNKNOWN = show "--" */
uint8_t ui_get_arc_volume(void);
void    ui_set_title(const char *title);
void    ui_set_subtitle(const char *subtitle);
void    ui_set_status(const char *status);
void    ui_set_radio_metadata(const char *station, const char *stream_title);
void    ui_set_mp3_metadata(const char *title, const char *artist);
void    ui_set_progress(uint8_t progress);  /* 0-100 shows the MP3 arc, UI_PROGRESS_HIDE hides it */
void    ui_set_clock(const char *hhmm);     /* NULL = time not known yet */
void    ui_set_wifi(bool connected);
void    ui_set_busy(bool busy);             /* buffering/connecting spinner */
void    ui_set_ai_phase(ui_ai_phase_t phase); /* only stores the value — safe from any task */
void    ui_handle_touch(int x, int y);      /* no-op, LVGL indev handles touch */

/* scrollable picker (stations/tracks), opened on long-press */
void    ui_show_picker(const char *title, const char *options, int selected); /* options = '\n' separated */
void    ui_close_picker(void);
int     ui_get_picked_index(void);

void    ui_show_notice(const char *text);   /* pill overlay, auto-hides after 8 s */

/* clock screensaver overlay; backlight is handled by the app */
void    ui_show_saver(bool show);
bool    ui_saver_active(void);
void    ui_saver_update(const char *clock_str, const char *line1, const char *line2);
