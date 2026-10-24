#include "ui_settings.h"
#include "settings_manager.h"
#include "display_ui.h"
#include "audio_io.h"
#include "bsp_board.h"
#include "wifi_ws.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "UI_SETTINGS";

static lv_obj_t *s_settings_container = NULL;
static bool s_is_open = false;

// 서브화면 열거형
typedef enum {
    SUBVIEW_MAIN = 0,
    SUBVIEW_VOLUME,
    SUBVIEW_WIFI,
    SUBVIEW_SERVER,
    SUBVIEW_BRIGHTNESS,
    SUBVIEW_INFO
} subview_t;

static subview_t s_current_view = SUBVIEW_MAIN;

// 임시 편집 버퍼
static app_settings_t s_edit_cfg;
static char s_input_target_buf[65] = {0};
static char *s_input_target_ptr = NULL;
static size_t s_input_max_len = 32;
static lv_obj_t *s_text_display_label = NULL;

// 전방 선언
static void show_view(subview_t view);
static void create_main_menu(lv_obj_t *parent);
static void create_volume_view(lv_obj_t *parent);
static void create_wifi_view(lv_obj_t *parent);
static void create_server_view(lv_obj_t *parent);
static void create_brightness_view(lv_obj_t *parent);
static void create_info_view(lv_obj_t *parent);

esp_err_t ui_settings_init(void) {
    ESP_LOGI(TAG, "Initializing UI Settings module...");
    return ESP_OK;
}

bool ui_settings_is_active(void) {
    return s_is_open;
}

void ui_settings_open(void) {
    if (s_is_open) return;

    ESP_LOGI(TAG, "Opening Settings UI...");
    s_is_open = true;

    // 현재 설정 복사
    memcpy(&s_edit_cfg, settings_manager_get(), sizeof(app_settings_t));

    lvgl_port_lock(-1);
    lv_obj_t *scr = lv_scr_act();

    // 240x240 전체를 덮는 설정 컨테이너 생성
    s_settings_container = lv_obj_create(scr);
    lv_obj_set_size(s_settings_container, 240, 240);
    lv_obj_align(s_settings_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_settings_container, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_width(s_settings_container, 0, 0);
    lv_obj_set_style_pad_all(s_settings_container, 4, 0);
    lv_obj_set_style_radius(s_settings_container, 120, 0); // 원형 디스플레이에 꼭 맞춤
    lv_obj_clear_flag(s_settings_container, LV_OBJ_FLAG_SCROLLABLE);

    show_view(SUBVIEW_MAIN);
    lvgl_port_unlock();
}

void ui_settings_close(void) {
    if (!s_is_open) return;

    ESP_LOGI(TAG, "Closing Settings UI, restoring emotion screen...");
    s_is_open = false;

    lvgl_port_lock(-1);
    if (s_settings_container != NULL) {
        lv_obj_del(s_settings_container);
        s_settings_container = NULL;
    }
    lvgl_port_unlock();

    audio_io_play_tone(800.0f, 60);
}

// ----------------------------------------------------------------------------
// 뷰 전환 함수
// ----------------------------------------------------------------------------
static void show_view(subview_t view) {
    if (!s_settings_container) return;

    s_current_view = view;
    lv_obj_clean(s_settings_container);

    switch (view) {
        case SUBVIEW_MAIN:
            create_main_menu(s_settings_container);
            break;
        case SUBVIEW_VOLUME:
            create_volume_view(s_settings_container);
            break;
        case SUBVIEW_WIFI:
            create_wifi_view(s_settings_container);
            break;
        case SUBVIEW_SERVER:
            create_server_view(s_settings_container);
            break;
        case SUBVIEW_BRIGHTNESS:
            create_brightness_view(s_settings_container);
            break;
        case SUBVIEW_INFO:
            create_info_view(s_settings_container);
            break;
    }
}

// ----------------------------------------------------------------------------
// [1] 메인 메뉴 (스크롤 목록)
// ----------------------------------------------------------------------------
static void on_menu_btn_click(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    audio_io_play_tone(1000.0f, 30);

    if (id == 1) show_view(SUBVIEW_VOLUME);
    else if (id == 2) show_view(SUBVIEW_WIFI);
    else if (id == 3) show_view(SUBVIEW_SERVER);
    else if (id == 4) show_view(SUBVIEW_BRIGHTNESS);
    else if (id == 5) show_view(SUBVIEW_INFO);
    else if (id == 6) {
        // 저장 후 재부팅
        settings_manager_save(&s_edit_cfg);
        audio_io_play_tone(1600.0f, 150);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (id == 7) {
        // 기본값 복원
        settings_manager_reset_to_default();
        audio_io_play_tone(600.0f, 150);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (id == 0) {
        // 닫기
        ui_settings_close();
    }
}

static void create_main_menu(lv_obj_t *parent) {
    // 타이틀 라벨
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D1FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 닫기 버튼 [X]
    lv_obj_t *close_btn = lv_btn_create(parent);
    lv_obj_set_size(close_btn, 36, 26);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -18, 6);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x444444), 0);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_menu_btn_click, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    // 스크롤 가능한 리스트 생성 (원형 화면 안쪽에 여유 있게 배치)
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, 200, 160);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 15);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(list, 10, 0);

    const char *items[] = {
        "1. Volume Control",
        "2. Wi-Fi Config",
        "3. Server Address",
        "4. Display Brightness",
        "5. System Info",
        "6. Save & Reboot",
        "7. Factory Reset"
    };

    for (int i = 0; i < 7; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, NULL, items[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        lv_obj_add_event_cb(btn, on_menu_btn_click, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
    }
}

// ----------------------------------------------------------------------------
// [2] 볼륨 조절 서브화면 (프로그레스바 / 슬라이더)
// ----------------------------------------------------------------------------
static lv_obj_t *s_vol_label = NULL;

static void on_vol_slider_changed(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);
    s_edit_cfg.volume = (uint8_t)val;

    char buf[32];
    snprintf(buf, sizeof(buf), "VOLUME: %d%%", val);
    lv_label_set_text(s_vol_label, buf);

    // 슬라이더 조작 시 현재 볼륨에 맞춘 비프음 재생 (귀로 크기 즉각 확인)
    audio_io_play_tone(440.0f, 30);
}

static void on_vol_save_click(lv_event_t *e) {
    settings_manager_set_volume(s_edit_cfg.volume);
    audio_io_play_tone(1200.0f, 60);
    show_view(SUBVIEW_MAIN);
}

static void create_volume_view(lv_obj_t *parent) {
    s_vol_label = lv_label_create(parent);
    char buf[32];
    snprintf(buf, sizeof(buf), "VOLUME: %d%%", s_edit_cfg.volume);
    lv_label_set_text(s_vol_label, buf);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0x00FF88), 0);
    lv_obj_align(s_vol_label, LV_ALIGN_TOP_MID, 0, 25);

    // 슬라이더 (10 ~ 100)
    lv_obj_t *slider = lv_slider_create(parent);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, s_edit_cfg.volume, LV_ANIM_OFF);
    lv_obj_set_size(slider, 160, 18);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x00FF88), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_vol_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // [저장 & 완료] 버튼
    lv_obj_t *save_btn = lv_btn_create(parent);
    lv_obj_set_size(save_btn, 80, 32);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x007ACC), 0);
    lv_obj_t *lbl = lv_label_create(save_btn);
    lv_label_set_text(lbl, "APPLY");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(save_btn, on_vol_save_click, LV_EVENT_CLICKED, NULL);
}

// ----------------------------------------------------------------------------
// [3] Wi-Fi 및 가로 스크롤 텍스트 입력기
// ----------------------------------------------------------------------------
static const char *s_char_palette = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-!@#$%&*";

static void update_text_display(void) {
    if (s_text_display_label) {
        lv_label_set_text(s_text_display_label, s_input_target_buf);
    }
}

static void on_char_btn_click(lv_event_t *e) {
    char ch = (char)(intptr_t)lv_event_get_user_data(e);
    size_t len = strlen(s_input_target_buf);
    if (len < s_input_max_len - 1) {
        s_input_target_buf[len] = ch;
        s_input_target_buf[len + 1] = '\0';
        update_text_display();
        audio_io_play_tone(1200.0f, 20);
    }
}

static void on_bs_btn_click(lv_event_t *e) {
    size_t len = strlen(s_input_target_buf);
    if (len > 0) {
        s_input_target_buf[len - 1] = '\0';
        update_text_display();
        audio_io_play_tone(700.0f, 30);
    }
}

static void on_space_btn_click(lv_event_t *e) {
    size_t len = strlen(s_input_target_buf);
    if (len < s_input_max_len - 1) {
        s_input_target_buf[len] = ' ';
        s_input_target_buf[len + 1] = '\0';
        update_text_display();
        audio_io_play_tone(900.0f, 20);
    }
}

static void on_text_input_done(lv_event_t *e) {
    if (s_input_target_ptr) {
        strncpy(s_input_target_ptr, s_input_target_buf, s_input_max_len);
    }
    audio_io_play_tone(1500.0f, 60);
    show_view(SUBVIEW_WIFI);
}

static void open_scroll_keyboard(lv_obj_t *parent, const char *title_str, char *target_ptr, size_t max_len) {
    s_input_target_ptr = target_ptr;
    s_input_max_len = max_len;
    strncpy(s_input_target_buf, target_ptr ? target_ptr : "", sizeof(s_input_target_buf) - 1);

    lv_obj_clean(parent);

    // 상단 타이틀
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, title_str);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D1FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 입력된 텍스트 표시 상자
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 190, 32);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x00D1FF), 0);
    lv_obj_set_style_pad_all(box, 2, 0);

    s_text_display_label = lv_label_create(box);
    lv_label_set_text(s_text_display_label, s_input_target_buf);
    lv_obj_set_style_text_color(s_text_display_label, lv_color_white(), 0);
    lv_obj_align(s_text_display_label, LV_ALIGN_LEFT_MID, 4, 0);

    // [핵심] 가로 스크롤 텍스트 바 (수평 스크롤 컨테이너)
    lv_obj_t *scroll_row = lv_obj_create(parent);
    lv_obj_set_size(scroll_row, 210, 48);
    lv_obj_align(scroll_row, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(scroll_row, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(scroll_row, 0, 0);
    lv_obj_set_flex_flow(scroll_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scroll_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(scroll_row, LV_DIR_HOR);
    lv_obj_set_style_pad_all(scroll_row, 4, 0);
    lv_obj_set_style_pad_column(scroll_row, 4, 0);

    // 문자 버튼들 생성 (좌우로 스와이프하여 스크롤)
    size_t pal_len = strlen(s_char_palette);
    for (size_t i = 0; i < pal_len; i++) {
        lv_obj_t *cbtn = lv_btn_create(scroll_row);
        lv_obj_set_size(cbtn, 32, 34);
        lv_obj_set_style_bg_color(cbtn, lv_color_hex(0x333333), 0);
        lv_obj_t *clbl = lv_label_create(cbtn);
        char cstr[2] = { s_char_palette[i], '\0' };
        lv_label_set_text(clbl, cstr);
        lv_obj_center(clbl);
        lv_obj_add_event_cb(cbtn, on_char_btn_click, LV_EVENT_CLICKED, (void *)(intptr_t)s_char_palette[i]);
    }

    // 하단 제어 바: [BS] [Space] [OK]
    lv_obj_t *bs_btn = lv_btn_create(parent);
    lv_obj_set_size(bs_btn, 44, 30);
    lv_obj_align(bs_btn, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    lv_obj_set_style_bg_color(bs_btn, lv_color_hex(0x882222), 0);
    lv_obj_t *bs_lbl = lv_label_create(bs_btn);
    lv_label_set_text(bs_lbl, "BS");
    lv_obj_center(bs_lbl);
    lv_obj_add_event_cb(bs_btn, on_bs_btn_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sp_btn = lv_btn_create(parent);
    lv_obj_set_size(sp_btn, 44, 30);
    lv_obj_align(sp_btn, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(sp_btn, lv_color_hex(0x444444), 0);
    lv_obj_t *sp_lbl = lv_label_create(sp_btn);
    lv_label_set_text(sp_lbl, "SPC");
    lv_obj_center(sp_lbl);
    lv_obj_add_event_cb(sp_btn, on_space_btn_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok_btn = lv_btn_create(parent);
    lv_obj_set_size(ok_btn, 44, 30);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0x008844), 0);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(ok_btn, on_text_input_done, LV_EVENT_CLICKED, NULL);
}

static void on_edit_ssid_click(lv_event_t *e) {
    open_scroll_keyboard(s_settings_container, "EDIT SSID", s_edit_cfg.wifi_ssid, sizeof(s_edit_cfg.wifi_ssid));
}

static void on_edit_pw_click(lv_event_t *e) {
    open_scroll_keyboard(s_settings_container, "EDIT PASSWORD", s_edit_cfg.wifi_pass, sizeof(s_edit_cfg.wifi_pass));
}

static void on_wifi_save_click(lv_event_t *e) {
    settings_manager_save(&s_edit_cfg);
    audio_io_play_tone(1400.0f, 60);
    show_view(SUBVIEW_MAIN);
}

static void create_wifi_view(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "WI-FI CONFIG");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D1FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    // SSID 버튼
    lv_obj_t *ssid_btn = lv_btn_create(parent);
    lv_obj_set_size(ssid_btn, 170, 34);
    lv_obj_align(ssid_btn, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_style_bg_color(ssid_btn, lv_color_hex(0x282828), 0);
    lv_obj_t *slbl = lv_label_create(ssid_btn);
    char sbuf[48];
    snprintf(sbuf, sizeof(sbuf), "SSID: %s", s_edit_cfg.wifi_ssid);
    lv_label_set_text(slbl, sbuf);
    lv_obj_center(slbl);
    lv_obj_add_event_cb(ssid_btn, on_edit_ssid_click, LV_EVENT_CLICKED, NULL);

    // PW 버튼
    lv_obj_t *pw_btn = lv_btn_create(parent);
    lv_obj_set_size(pw_btn, 170, 34);
    lv_obj_align(pw_btn, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_bg_color(pw_btn, lv_color_hex(0x282828), 0);
    lv_obj_t *plbl = lv_label_create(pw_btn);
    char pbuf[48];
    snprintf(pbuf, sizeof(pbuf), "PW: %s", s_edit_cfg.wifi_pass[0] ? "********" : "(None)");
    lv_label_set_text(plbl, pbuf);
    lv_obj_center(plbl);
    lv_obj_add_event_cb(pw_btn, on_edit_pw_click, LV_EVENT_CLICKED, NULL);

    // [저장] 및 [뒤로] 버튼
    lv_obj_t *save_btn = lv_btn_create(parent);
    lv_obj_set_size(save_btn, 80, 32);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_LEFT, 24, -25);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x007ACC), 0);
    lv_obj_t *lbl = lv_label_create(save_btn);
    lv_label_set_text(lbl, "SAVE");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(save_btn, on_wifi_save_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 80, 32);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -24, -25);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x444444), 0);
    lv_obj_t *blbl = lv_label_create(back_btn);
    lv_label_set_text(blbl, "BACK");
    lv_obj_center(blbl);
    lv_obj_add_event_cb(back_btn, (lv_event_cb_t)[](lv_event_t *e){ show_view(SUBVIEW_MAIN); }, LV_EVENT_CLICKED, NULL);
}

// ----------------------------------------------------------------------------
// [4] 서버 주소 & PORT 설정 서브화면
// ----------------------------------------------------------------------------
static void on_edit_ip_click(lv_event_t *e) {
    open_scroll_keyboard(s_settings_container, "EDIT SERVER IP", s_edit_cfg.server_ip, sizeof(s_edit_cfg.server_ip));
}

static void on_server_save_click(lv_event_t *e) {
    settings_manager_save(&s_edit_cfg);
    audio_io_play_tone(1400.0f, 60);
    show_view(SUBVIEW_MAIN);
}

static void create_server_view(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SERVER CONFIG");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D1FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    // IP 버튼
    lv_obj_t *ip_btn = lv_btn_create(parent);
    lv_obj_set_size(ip_btn, 170, 34);
    lv_obj_align(ip_btn, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(ip_btn, lv_color_hex(0x282828), 0);
    lv_obj_t *ilbl = lv_label_create(ip_btn);
    char ibuf[80];
    snprintf(ibuf, sizeof(ibuf), "IP: %s", s_edit_cfg.server_ip);
    lv_label_set_text(ilbl, ibuf);
    lv_obj_center(ilbl);
    lv_obj_add_event_cb(ip_btn, on_edit_ip_click, LV_EVENT_CLICKED, NULL);

    // PORT 라벨
    lv_obj_t *port_lbl = lv_label_create(parent);
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "PORT: %u (Default)", (unsigned int)s_edit_cfg.server_port);
    lv_label_set_text(port_lbl, pbuf);
    lv_obj_set_style_text_color(port_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(port_lbl, LV_ALIGN_TOP_MID, 0, 95);

    // [저장] 및 [뒤로] 버튼
    lv_obj_t *save_btn = lv_btn_create(parent);
    lv_obj_set_size(save_btn, 80, 32);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_LEFT, 24, -25);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x007ACC), 0);
    lv_obj_t *lbl = lv_label_create(save_btn);
    lv_label_set_text(lbl, "SAVE");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(save_btn, on_server_save_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 80, 32);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -24, -25);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x444444), 0);
    lv_obj_t *blbl = lv_label_create(back_btn);
    lv_label_set_text(blbl, "BACK");
    lv_obj_center(blbl);
    lv_obj_add_event_cb(back_btn, (lv_event_cb_t)[](lv_event_t *e){ show_view(SUBVIEW_MAIN); }, LV_EVENT_CLICKED, NULL);
}

// ----------------------------------------------------------------------------
// [5] 화면 밝기 서브화면
// ----------------------------------------------------------------------------
static lv_obj_t *s_bright_label = NULL;

static void on_bright_slider_changed(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);
    s_edit_cfg.brightness = (uint8_t)val;

    char buf[32];
    snprintf(buf, sizeof(buf), "BRIGHTNESS: %d%%", val);
    lv_label_set_text(s_bright_label, buf);

    // 하드웨어 백라이트 반영
    gpio_set_level((gpio_num_t)LCD_BL, (val > 10) ? 1 : 0);
}

static void create_brightness_view(lv_obj_t *parent) {
    s_bright_label = lv_label_create(parent);
    char buf[32];
    snprintf(buf, sizeof(buf), "BRIGHTNESS: %d%%", s_edit_cfg.brightness);
    lv_label_set_text(s_bright_label, buf);
    lv_obj_set_style_text_color(s_bright_label, lv_color_hex(0xFFDD00), 0);
    lv_obj_align(s_bright_label, LV_ALIGN_TOP_MID, 0, 25);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, s_edit_cfg.brightness, LV_ANIM_OFF);
    lv_obj_set_size(slider, 160, 18);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFDD00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_bright_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *save_btn = lv_btn_create(parent);
    lv_obj_set_size(save_btn, 80, 32);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x007ACC), 0);
    lv_obj_t *lbl = lv_label_create(save_btn);
    lv_label_set_text(lbl, "APPLY");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(save_btn, (lv_event_cb_t)[](lv_event_t *e){
        settings_manager_set_brightness(s_edit_cfg.brightness);
        show_view(SUBVIEW_MAIN);
    }, LV_EVENT_CLICKED, NULL);
}

// ----------------------------------------------------------------------------
// [6] 시스템 정보 화면
// ----------------------------------------------------------------------------
static void create_info_view(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "SYSTEM INFO");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D1FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t *info_lbl = lv_label_create(parent);
    char buf[160];
    uint32_t free_heap_kb = esp_get_free_heap_size() / 1024;
    snprintf(buf, sizeof(buf), "SSID: %s\nIP: %s\nFreeHeap: %uKB\nWS: %s",
             s_edit_cfg.wifi_ssid,
             wifi_ws_get_ip_str() ? wifi_ws_get_ip_str() : "N/A",
             (unsigned int)free_heap_kb,
             wifi_ws_is_connected() ? "CONNECTED" : "DISCONN");
    lv_label_set_text(info_lbl, buf);
    lv_obj_set_style_text_color(info_lbl, lv_color_white(), 0);
    lv_obj_align(info_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *back_btn = lv_btn_create(parent);
    lv_obj_set_size(back_btn, 80, 32);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x444444), 0);
    lv_obj_t *blbl = lv_label_create(back_btn);
    lv_label_set_text(blbl, "BACK");
    lv_obj_center(blbl);
    lv_obj_add_event_cb(back_btn, (lv_event_cb_t)[](lv_event_t *e){ show_view(SUBVIEW_MAIN); }, LV_EVENT_CLICKED, NULL);
}
