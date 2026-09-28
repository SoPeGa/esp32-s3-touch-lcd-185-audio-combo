#include "ui_round.h"
#include <stdio.h>
#include <string.h>

#define COLOR_ACCENT      0x4da8e8
#define COLOR_ACCENT_DIM  0x33505f
#define COLOR_STATUS      0x55c8ee
#define COLOR_SUBTITLE    0x8fd6f2
#define COLOR_PROGRESS    0xe8a13a

static lv_obj_t *s_vol_arc;
static lv_obj_t *s_progress_arc;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_mode_dots[UI_MODE_COUNT];
static lv_obj_t *s_title_label;
static lv_obj_t *s_subtitle_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_play_btn;
static lv_obj_t *s_prev_btn;
static lv_obj_t *s_next_btn;
static lv_obj_t *s_busy_spinner;
static lv_obj_t *s_vol_popup;
static lv_obj_t *s_vol_popup_label;
static lv_timer_t *s_vol_popup_timer;
static lv_obj_t *s_ai_page;
static lv_obj_t *s_ai_face;
static lv_obj_t *s_ai_left_eye;
static lv_obj_t *s_ai_right_eye;
static lv_obj_t *s_ai_left_brow;
static lv_obj_t *s_ai_right_brow;
static lv_obj_t *s_ai_mouth;
static lv_obj_t *s_ai_left_wave;
static lv_obj_t *s_ai_right_wave;
static volatile int s_ai_phase = UI_AI_IDLE;
static uint32_t s_ai_frame;
static lv_obj_t *s_picker;
static lv_obj_t *s_picker_roller;
static int s_picked_index = -1;
static lv_obj_t *s_saver;
static lv_obj_t *s_saver_clock;
static lv_obj_t *s_saver_line1;
static lv_obj_t *s_saver_line2;
static lv_obj_t *s_notice;
static lv_timer_t *s_notice_timer;
static ui_action_cb_t s_action_cb;
static ui_mode_t s_current_mode = UI_MODE_RADIO;
static uint8_t s_arc_volume = 55;
static bool s_ui_playing;
static bool s_wifi_connected;
static char s_status_text[48];

static void send_action(int action)
{
    if (s_action_cb) s_action_cb(action);
}

static void send_mode_action(int mode)
{
    send_action(mode == UI_MODE_RADIO ? UI_ACTION_MODE_RADIO :
                mode == UI_MODE_MP3   ? UI_ACTION_MODE_MP3   : UI_ACTION_MODE_AI);
}

static void vol_popup_show(uint8_t volume)
{
    if (!s_vol_popup) return;
    char buf[24];
    snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX "  %u", (unsigned)volume);
    lv_label_set_text(s_vol_popup_label, buf);
    lv_obj_clear_flag(s_vol_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_vol_popup_timer);
    lv_timer_resume(s_vol_popup_timer);
}

static void vol_popup_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_add_flag(s_vol_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_vol_popup_timer);
}

static void vol_arc_event_cb(lv_event_t *e)
{
    (void)e;
    s_arc_volume = (uint8_t)lv_arc_get_value(s_vol_arc);
    vol_popup_show(s_arc_volume);
    send_action(UI_ACTION_VOL_SET);
}

static void mode_tap_cb(lv_event_t *e)
{
    (void)e;
    send_mode_action((s_current_mode + 1) % UI_MODE_COUNT);
}

static void subtitle_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_mode == UI_MODE_AI) send_action(UI_ACTION_AI_VOICE);
}

static void long_press_cb(lv_event_t *e)
{
    (void)e;
    if (s_picker || s_saver) return;
    send_action(s_current_mode == UI_MODE_AI ? UI_ACTION_AI_RESET : UI_ACTION_OPEN_LIST);
}

static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (s_picker || s_saver) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT) {
        send_mode_action((s_current_mode + 1) % UI_MODE_COUNT);
    } else if (dir == LV_DIR_RIGHT) {
        send_mode_action((s_current_mode + UI_MODE_COUNT - 1) % UI_MODE_COUNT);
    }
}

static void btn_event_cb(lv_event_t *e)
{
    send_action((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *make_round_btn(lv_obj_t *parent, const char *symbol, int action,
                                int x, int y, int size)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x11283a), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, action == UI_ACTION_PLAY_PAUSE ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x245c84), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);
    return btn;
}

static const struct { int16_t x; int16_t y; const char *text; } SCALE[] = {
    {-91, 112, "0"}, {-145, -17, "20"}, {-70, -130, "40"},
    {70, -130, "60"}, {145, -17, "80"}, {91, 112, "100"},
};

static lv_obj_t *make_plain(lv_obj_t *parent, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

/* Animated assistant avatar, adapted from AI_Voice_Assistant_IDF. */
static void create_ai_page(lv_obj_t *inner)
{
    s_ai_page = lv_obj_create(inner);
    lv_obj_remove_style_all(s_ai_page);
    lv_obj_set_size(s_ai_page, 276, 276);
    lv_obj_center(s_ai_page);
    lv_obj_clear_flag(s_ai_page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ai_page, LV_OBJ_FLAG_HIDDEN);

    s_ai_face = make_plain(s_ai_page, 96, 96, 0xffd36e);
    lv_obj_set_style_border_width(s_ai_face, 3, 0);
    lv_obj_set_style_border_color(s_ai_face, lv_color_hex(0xfff0b8), 0);
    lv_obj_align(s_ai_face, LV_ALIGN_TOP_MID, 0, 70);

    s_ai_left_brow = make_plain(s_ai_face, 18, 4, 0x6a3d20);
    lv_obj_align(s_ai_left_brow, LV_ALIGN_CENTER, -19, -31);
    s_ai_right_brow = make_plain(s_ai_face, 18, 4, 0x6a3d20);
    lv_obj_align(s_ai_right_brow, LV_ALIGN_CENTER, 19, -31);

    s_ai_left_eye = make_plain(s_ai_face, 12, 16, 0x172027);
    lv_obj_align(s_ai_left_eye, LV_ALIGN_CENTER, -19, -16);
    s_ai_right_eye = make_plain(s_ai_face, 12, 16, 0x172027);
    lv_obj_align(s_ai_right_eye, LV_ALIGN_CENTER, 19, -16);

    lv_obj_t *cheek = make_plain(s_ai_face, 14, 8, 0xff7a86);
    lv_obj_set_style_bg_opa(cheek, LV_OPA_50, 0);
    lv_obj_align(cheek, LV_ALIGN_CENTER, -27, 10);
    cheek = make_plain(s_ai_face, 14, 8, 0xff7a86);
    lv_obj_set_style_bg_opa(cheek, LV_OPA_50, 0);
    lv_obj_align(cheek, LV_ALIGN_CENTER, 27, 10);

    s_ai_mouth = make_plain(s_ai_face, 32, 7, 0x31161c);
    lv_obj_align(s_ai_mouth, LV_ALIGN_CENTER, 0, 21);

    s_ai_left_wave = make_plain(s_ai_page, 12, 38, 0x000000);
    lv_obj_set_style_bg_opa(s_ai_left_wave, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ai_left_wave, 3, 0);
    lv_obj_set_style_border_color(s_ai_left_wave, lv_color_hex(0x4fb3ff), 0);
    lv_obj_align(s_ai_left_wave, LV_ALIGN_CENTER, -72, -20);
    s_ai_right_wave = make_plain(s_ai_page, 12, 38, 0x000000);
    lv_obj_set_style_bg_opa(s_ai_right_wave, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ai_right_wave, 3, 0);
    lv_obj_set_style_border_color(s_ai_right_wave, lv_color_hex(0x4fb3ff), 0);
    lv_obj_align(s_ai_right_wave, LV_ALIGN_CENTER, 72, -20);
}

static void ai_anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static int last_phase = -1;
    static uint32_t last_step = 0xffffffff;
    if (s_current_mode != UI_MODE_AI || !s_ai_page) {
        last_phase = -1;
        return;
    }
    s_ai_frame++;
    int phase = s_ai_phase;
    uint32_t step = s_ai_frame / 5;
    if (phase == last_phase && (phase == UI_AI_IDLE || step == last_step)) {
        return;
    }
    last_phase = phase;
    last_step = step;
    bool pulse = step % 2;

    lv_obj_set_style_bg_color(s_play_btn,
        lv_color_hex(phase == UI_AI_IDLE ? 0x11283a : 0xb02535), 0);

    if (phase == UI_AI_LISTENING) {
        lv_obj_set_style_bg_color(s_ai_face, lv_color_hex(0x73d7ff), 0);
        lv_obj_set_size(s_ai_left_eye, 14, 18);
        lv_obj_set_size(s_ai_right_eye, 14, 18);
        lv_obj_set_style_transform_angle(s_ai_left_brow, -140, 0);
        lv_obj_set_style_transform_angle(s_ai_right_brow, 140, 0);
        lv_obj_set_size(s_ai_mouth, 20, 14);
        lv_obj_set_style_opa(s_ai_left_wave, pulse ? LV_OPA_90 : LV_OPA_40, 0);
        lv_obj_set_style_opa(s_ai_right_wave, pulse ? LV_OPA_40 : LV_OPA_90, 0);
    } else if (phase == UI_AI_THINKING) {
        lv_obj_set_style_bg_color(s_ai_face, lv_color_hex(0xd7b7ff), 0);
        lv_obj_set_size(s_ai_left_eye, 9, pulse ? 9 : 12);
        lv_obj_set_size(s_ai_right_eye, 9, pulse ? 12 : 9);
        lv_obj_set_style_transform_angle(s_ai_left_brow, 120, 0);
        lv_obj_set_style_transform_angle(s_ai_right_brow, -120, 0);
        lv_obj_set_size(s_ai_mouth, 22, 5);
        lv_obj_set_style_opa(s_ai_left_wave, LV_OPA_0, 0);
        lv_obj_set_style_opa(s_ai_right_wave, LV_OPA_0, 0);
    } else if (phase == UI_AI_SPEAKING) {
        static const int mouth_h[] = {8, 20, 12, 26, 14, 22};
        int idx = step % (sizeof(mouth_h) / sizeof(mouth_h[0]));
        lv_obj_set_style_bg_color(s_ai_face, lv_color_hex(0xffd36e), 0);
        lv_obj_set_size(s_ai_left_eye, 12, 16);
        lv_obj_set_size(s_ai_right_eye, 12, 16);
        lv_obj_set_style_transform_angle(s_ai_left_brow, 0, 0);
        lv_obj_set_style_transform_angle(s_ai_right_brow, 0, 0);
        lv_obj_set_size(s_ai_mouth, 32, mouth_h[idx]);
        lv_obj_set_style_opa(s_ai_left_wave, pulse ? LV_OPA_80 : LV_OPA_30, 0);
        lv_obj_set_style_opa(s_ai_right_wave, pulse ? LV_OPA_30 : LV_OPA_80, 0);
    } else {
        lv_obj_set_style_bg_color(s_ai_face, lv_color_hex(0xffd36e), 0);
        lv_obj_set_size(s_ai_left_eye, 12, 16);
        lv_obj_set_size(s_ai_right_eye, 12, 16);
        lv_obj_set_style_transform_angle(s_ai_left_brow, 0, 0);
        lv_obj_set_style_transform_angle(s_ai_right_brow, 0, 0);
        lv_obj_set_size(s_ai_mouth, 32, 7);
        lv_obj_set_style_opa(s_ai_left_wave, LV_OPA_20, 0);
        lv_obj_set_style_opa(s_ai_right_wave, LV_OPA_20, 0);
    }
}

static void refresh_status_line(void)
{
    if (!s_status_label) return;
    char buf[96];
    snprintf(buf, sizeof(buf), "#%06x %s# %s",
             s_wifi_connected ? COLOR_STATUS : 0x5a6a72, LV_SYMBOL_WIFI, s_status_text);
    lv_label_set_text(s_status_label, buf);
}

void ui_create(ui_action_cb_t cb)
{
    s_action_cb = cb;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020609), 0);
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_vol_arc = lv_arc_create(scr);
    lv_obj_set_size(s_vol_arc, 344, 344);
    lv_obj_center(s_vol_arc);
    lv_arc_set_bg_angles(s_vol_arc, 130, 50);
    lv_arc_set_range(s_vol_arc, 0, 100);
    lv_arc_set_value(s_vol_arc, s_arc_volume);
    lv_obj_set_style_arc_width(s_vol_arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_vol_arc, lv_color_hex(0x344653), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_vol_arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_vol_arc, lv_color_hex(0x319ce9), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_arc, lv_color_hex(0xaeb8be), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_vol_arc, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_vol_arc, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_arc, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_arc, vol_arc_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* thin MP3 progress arc in the bottom gap of the volume arc */
    s_progress_arc = lv_arc_create(scr);
    lv_obj_set_size(s_progress_arc, 344, 344);
    lv_obj_center(s_progress_arc);
    lv_arc_set_bg_angles(s_progress_arc, 64, 116);
    lv_arc_set_mode(s_progress_arc, LV_ARC_MODE_REVERSE);
    lv_arc_set_range(s_progress_arc, 0, 100);
    lv_arc_set_value(s_progress_arc, 0);
    lv_obj_set_style_arc_width(s_progress_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_progress_arc, lv_color_hex(0x22333f), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_progress_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_progress_arc, lv_color_hex(COLOR_PROGRESS), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_progress_arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(s_progress_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_progress_arc, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 6; i++) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, SCALE[i].text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x9aa9b1), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, SCALE[i].x, SCALE[i].y);
    }

    s_battery_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_CENTER, 0, -145);
    ui_set_battery(UI_BATTERY_UNKNOWN);

    lv_obj_t *inner = lv_obj_create(scr);
    lv_obj_set_size(inner, 276, 276);
    lv_obj_center(inner);
    lv_obj_set_style_radius(inner, 138, 0);
    lv_obj_set_style_clip_corner(inner, true, 0);
    lv_obj_set_style_bg_color(inner, lv_color_hex(0x10283a), 0);
    lv_obj_set_style_bg_grad_color(inner, lv_color_hex(0x06111b), 0);
    lv_obj_set_style_bg_grad_dir(inner, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(inner, 2, 0);
    lv_obj_set_style_border_color(inner, lv_color_hex(0x3d9ed7), 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(inner, long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    s_clock_label = lv_label_create(inner);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xdfe9ef), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(s_clock_label, "--:--");

    s_mode_label = lv_label_create(inner);
    lv_obj_set_width(s_mode_label, 160);
    lv_obj_set_style_text_align(s_mode_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0x6fb9de), 0);
    lv_obj_align(s_mode_label, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_add_flag(s_mode_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mode_label, 10);
    lv_obj_add_event_cb(s_mode_label, mode_tap_cb, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < UI_MODE_COUNT; i++) {
        s_mode_dots[i] = lv_obj_create(inner);
        lv_obj_set_size(s_mode_dots[i], 8, 8);
        lv_obj_set_style_radius(s_mode_dots[i], 4, 0);
        lv_obj_set_style_border_width(s_mode_dots[i], 0, 0);
        lv_obj_set_style_bg_color(s_mode_dots[i], lv_color_hex(COLOR_ACCENT_DIM), 0);
        lv_obj_align(s_mode_dots[i], LV_ALIGN_TOP_MID, (i - 1) * 18, 58);
        lv_obj_clear_flag(s_mode_dots[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_title_label = lv_label_create(inner);
    lv_obj_set_width(s_title_label, 232);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_22, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_add_flag(s_title_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_title_label, long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    s_subtitle_label = lv_label_create(inner);
    lv_obj_set_width(s_subtitle_label, 236);
    lv_label_set_long_mode(s_subtitle_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_subtitle_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_subtitle_label, lv_color_hex(COLOR_SUBTITLE), 0);
    lv_obj_set_style_text_font(s_subtitle_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_subtitle_label, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_ext_click_area(s_subtitle_label, 8);
    lv_obj_add_event_cb(s_subtitle_label, subtitle_tap_cb, LV_EVENT_CLICKED, NULL);

    create_ai_page(inner);

    s_busy_spinner = lv_spinner_create(inner, 1000, 60);
    lv_obj_set_size(s_busy_spinner, 26, 26);
    lv_obj_align(s_busy_spinner, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_arc_width(s_busy_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_busy_spinner, lv_color_hex(0x1b3648), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_busy_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_busy_spinner, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_clear_flag(s_busy_spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_busy_spinner, LV_OBJ_FLAG_HIDDEN);

    s_prev_btn = make_round_btn(inner, LV_SYMBOL_PREV, UI_ACTION_PREV, -54, 91, 36);
    s_play_btn = make_round_btn(inner, LV_SYMBOL_PLAY, UI_ACTION_PLAY_PAUSE, 0, 91, 44);
    s_next_btn = make_round_btn(inner, LV_SYMBOL_NEXT, UI_ACTION_NEXT, 54, 91, 36);
    lv_timer_create(ai_anim_timer_cb, 90, NULL);

    s_status_label = lv_label_create(inner);
    lv_obj_set_width(s_status_label, 200);
    lv_label_set_recolor(s_status_label, true);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_STATUS), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* volume value popup, shown while the arc is dragged */
    s_vol_popup = lv_obj_create(scr);
    lv_obj_set_size(s_vol_popup, 116, 44);
    lv_obj_align(s_vol_popup, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_radius(s_vol_popup, 22, 0);
    lv_obj_set_style_bg_color(s_vol_popup, lv_color_hex(0x0d2b40), 0);
    lv_obj_set_style_bg_opa(s_vol_popup, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_vol_popup, 1, 0);
    lv_obj_set_style_border_color(s_vol_popup, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_pad_all(s_vol_popup, 0, 0);
    lv_obj_clear_flag(s_vol_popup, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_vol_popup, LV_OBJ_FLAG_HIDDEN);
    s_vol_popup_label = lv_label_create(s_vol_popup);
    lv_obj_set_style_text_font(s_vol_popup_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_vol_popup_label, lv_color_white(), 0);
    lv_obj_center(s_vol_popup_label);
    s_vol_popup_timer = lv_timer_create(vol_popup_timer_cb, 1200, NULL);
    lv_timer_pause(s_vol_popup_timer);

    strlcpy(s_status_text, "-", sizeof(s_status_text));
    ui_set_mode(UI_MODE_RADIO);
    ui_set_radio_metadata("Internet Radio", "Astept metadata...");
    ui_set_status("Pornire...");
}

void ui_set_mode(ui_mode_t mode)
{
    s_current_mode = mode;
    if (!s_mode_label) return;
    if (mode == UI_MODE_MP3) {
        lv_label_set_text(s_mode_label, "PLAYER MP3");
    } else if (mode == UI_MODE_AI) {
        lv_label_set_text(s_mode_label, "ASISTENT AI");
    } else {
        lv_label_set_text(s_mode_label, "RADIO INTERNET");
    }
    for (int i = 0; i < UI_MODE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_mode_dots[i],
            lv_color_hex(i == (int)mode ? COLOR_ACCENT : COLOR_ACCENT_DIM), 0);
    }
    if (mode != UI_MODE_MP3) ui_set_progress(UI_PROGRESS_HIDE);

    lv_obj_t *play_lbl = lv_obj_get_child(s_play_btn, 0);
    if (mode == UI_MODE_AI) {
        lv_obj_add_flag(s_prev_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ai_page, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(play_lbl, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_14, 0);
        lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 170);
        lv_obj_set_style_text_font(s_subtitle_label, &lv_font_montserrat_12, 0);
        lv_obj_align(s_subtitle_label, LV_ALIGN_TOP_MID, 0, 190);
        lv_obj_add_flag(s_subtitle_label, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(s_prev_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ai_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_play_btn, lv_color_hex(0x11283a), 0);
        lv_label_set_text(play_lbl, s_ui_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_22, 0);
        lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 80);
        lv_obj_set_style_text_font(s_subtitle_label, &lv_font_montserrat_14, 0);
        lv_obj_align(s_subtitle_label, LV_ALIGN_TOP_MID, 0, 112);
        lv_obj_clear_flag(s_subtitle_label, LV_OBJ_FLAG_CLICKABLE);
    }
}

void ui_set_radio_metadata(const char *station, const char *stream_title)
{
    lv_label_set_text(s_title_label, (station && station[0]) ? station : "-");
    lv_label_set_text(s_subtitle_label, (stream_title && stream_title[0]) ? stream_title : "-");
}

void ui_set_mp3_metadata(const char *title, const char *artist)
{
    lv_label_set_text(s_title_label, (title && title[0]) ? title : "-");
    lv_label_set_text(s_subtitle_label, (artist && artist[0]) ? artist : "-");
}

void ui_set_playing(bool playing)
{
    s_ui_playing = playing;
    if (!s_play_btn || s_current_mode == UI_MODE_AI) return;  /* AI mode keeps the mic symbol */
    lv_obj_t *lbl = lv_obj_get_child(s_play_btn, 0);
    lv_label_set_text(lbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void ui_set_ai_phase(ui_ai_phase_t phase)
{
    s_ai_phase = phase;
}

void ui_set_volume(uint8_t volume)
{
    if (volume == s_arc_volume && s_vol_arc) return;
    s_arc_volume = volume;
    if (s_vol_arc) lv_arc_set_value(s_vol_arc, volume);
}

void ui_set_battery(uint8_t pct)
{
    if (!s_battery_label) return;
    const char *sym;
    uint32_t color;
    char buf[24];
    if (pct > 100) {
        lv_label_set_text(s_battery_label, LV_SYMBOL_BATTERY_EMPTY " --");
        lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x7c8a92), 0);
        return;
    }
    sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL :
          pct > 55 ? LV_SYMBOL_BATTERY_3 :
          pct > 30 ? LV_SYMBOL_BATTERY_2 :
          pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    color = pct > 50 ? 0x7bd88f : pct > 20 ? 0xeec84a : 0xf06a5a;
    snprintf(buf, sizeof(buf), "%s %u", sym, (unsigned)pct);
    lv_label_set_text(s_battery_label, buf);
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(color), 0);
}

uint8_t ui_get_arc_volume(void) { return s_arc_volume; }

void ui_set_title(const char *title)
{
    if (s_title_label) lv_label_set_text(s_title_label, (title && title[0]) ? title : "-");
}

void ui_set_subtitle(const char *subtitle)
{
    if (s_subtitle_label) lv_label_set_text(s_subtitle_label, (subtitle && subtitle[0]) ? subtitle : "-");
}

void ui_set_status(const char *status)
{
    strlcpy(s_status_text, (status && status[0]) ? status : "-", sizeof(s_status_text));
    refresh_status_line();
}

void ui_set_wifi(bool connected)
{
    if (connected == s_wifi_connected && s_status_label) return;
    s_wifi_connected = connected;
    refresh_status_line();
}

void ui_set_busy(bool busy)
{
    if (!s_busy_spinner) return;
    if (busy) lv_obj_clear_flag(s_busy_spinner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_busy_spinner, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_clock(const char *hhmm)
{
    if (s_clock_label) lv_label_set_text(s_clock_label, (hhmm && hhmm[0]) ? hhmm : "--:--");
}

void ui_set_progress(uint8_t progress)
{
    if (!s_progress_arc) return;
    if (progress > 100) {
        lv_obj_add_flag(s_progress_arc, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_progress_arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(s_progress_arc, progress);
}

/* ---- scrollable station/track picker ---- */

static void picker_ok_cb(lv_event_t *e)
{
    (void)e;
    if (!s_picker_roller) return;
    s_picked_index = (int)lv_roller_get_selected(s_picker_roller);
    ui_close_picker();
    send_action(UI_ACTION_LIST_PICK);
}

static void picker_cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_close_picker();
}

void ui_show_picker(const char *title, const char *options, int selected)
{
    ui_close_picker();
    if (!options || !options[0]) return;

    s_picker = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_picker, 360, 360);
    lv_obj_center(s_picker);
    lv_obj_set_style_radius(s_picker, 180, 0);
    lv_obj_set_style_bg_color(s_picker, lv_color_hex(0x04101a), 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_picker, 0, 0);
    lv_obj_set_style_pad_all(s_picker, 0, 0);
    lv_obj_clear_flag(s_picker, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(s_picker);
    lv_label_set_text(cap, title ? title : "");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x6fb9de), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 34);

    s_picker_roller = lv_roller_create(s_picker);
    lv_roller_set_options(s_picker_roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_picker_roller, 5);
    lv_obj_set_width(s_picker_roller, 272);
    lv_obj_align(s_picker_roller, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_bg_color(s_picker_roller, lv_color_hex(0x0a1b2a), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_picker_roller, lv_color_hex(0x9fb4c0), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_picker_roller, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_picker_roller, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_picker_roller, lv_color_hex(0x1c4666), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_picker_roller, lv_color_white(), LV_PART_SELECTED);
    int count = (int)lv_roller_get_option_cnt(s_picker_roller);
    if (selected >= 0 && selected < count) {
        lv_roller_set_selected(s_picker_roller, selected, LV_ANIM_OFF);
    }

    lv_obj_t *ok = lv_btn_create(s_picker);
    lv_obj_set_size(ok, 52, 52);
    lv_obj_set_style_radius(ok, 26, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, -42, -26);
    lv_obj_add_event_cb(ok, picker_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(ok);
    lv_label_set_text(lbl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x04121d), 0);
    lv_obj_center(lbl);

    lv_obj_t *cancel = lv_btn_create(s_picker);
    lv_obj_set_size(cancel, 52, 52);
    lv_obj_set_style_radius(cancel, 26, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x24404f), 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 42, -26);
    lv_obj_add_event_cb(cancel, picker_cancel_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(cancel);
    lv_label_set_text(lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
}

void ui_close_picker(void)
{
    if (s_picker) {
        lv_obj_del(s_picker);
        s_picker = NULL;
        s_picker_roller = NULL;
    }
}

int ui_get_picked_index(void) { return s_picked_index; }

/* ---- transient notice pill (e.g. IP address after WiFi connect) ---- */

static void notice_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_notice) lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_notice_timer);
}

void ui_show_notice(const char *text)
{
    if (!text || !text[0]) return;
    if (!s_notice) {
        s_notice = lv_label_create(lv_scr_act());
        lv_obj_set_style_bg_color(s_notice, lv_color_hex(0x0d2b40), 0);
        lv_obj_set_style_bg_opa(s_notice, LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_notice, 1, 0);
        lv_obj_set_style_border_color(s_notice, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_radius(s_notice, 16, 0);
        lv_obj_set_style_pad_hor(s_notice, 16, 0);
        lv_obj_set_style_pad_ver(s_notice, 9, 0);
        lv_obj_set_style_text_font(s_notice, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_notice, lv_color_white(), 0);
        s_notice_timer = lv_timer_create(notice_timer_cb, 8000, NULL);
        lv_timer_pause(s_notice_timer);
    }
    lv_label_set_text(s_notice, text);
    lv_obj_align(s_notice, LV_ALIGN_CENTER, 0, -4);
    lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_notice);
    lv_timer_reset(s_notice_timer);
    lv_timer_resume(s_notice_timer);
}

/* ---- clock screensaver ---- */

void ui_show_saver(bool show)
{
    if (show == (s_saver != NULL)) return;
    if (!show) {
        lv_obj_del(s_saver);
        s_saver = NULL;
        s_saver_clock = s_saver_line1 = s_saver_line2 = NULL;
        return;
    }
    /* full-screen overlay: swallows the wake-up touch so nothing underneath is pressed */
    s_saver = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_saver, 360, 360);
    lv_obj_center(s_saver);
    lv_obj_set_style_radius(s_saver, 180, 0);
    lv_obj_set_style_bg_color(s_saver, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_saver, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_saver, 0, 0);
    lv_obj_clear_flag(s_saver, LV_OBJ_FLAG_SCROLLABLE);

    s_saver_clock = lv_label_create(s_saver);
    lv_obj_set_style_text_font(s_saver_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_saver_clock, lv_color_hex(0x9fc4dc), 0);
    lv_obj_align(s_saver_clock, LV_ALIGN_CENTER, 0, -34);
    lv_label_set_text(s_saver_clock, "--:--");

    s_saver_line1 = lv_label_create(s_saver);
    lv_obj_set_width(s_saver_line1, 250);
    lv_label_set_long_mode(s_saver_line1, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_saver_line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_saver_line1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_saver_line1, lv_color_hex(0x4a8cb4), 0);
    lv_obj_align(s_saver_line1, LV_ALIGN_CENTER, 0, 26);
    lv_label_set_text(s_saver_line1, "");

    s_saver_line2 = lv_label_create(s_saver);
    lv_obj_set_style_text_font(s_saver_line2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_saver_line2, lv_color_hex(0x35505f), 0);
    lv_obj_align(s_saver_line2, LV_ALIGN_CENTER, 0, 52);
    lv_label_set_text(s_saver_line2, "");
}

bool ui_saver_active(void) { return s_saver != NULL; }

void ui_saver_update(const char *clock_str, const char *line1, const char *line2)
{
    if (!s_saver) return;
    if (clock_str) lv_label_set_text(s_saver_clock, clock_str);
    if (line1) lv_label_set_text(s_saver_line1, line1);
    if (line2) {
        lv_label_set_text(s_saver_line2, line2);
        lv_obj_align(s_saver_line2, LV_ALIGN_CENTER, 0, 52);
    }
}

void ui_handle_touch(int x, int y) { (void)x; (void)y; }
