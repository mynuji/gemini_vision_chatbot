#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_EMOTION_IDLE = 0,
    UI_EMOTION_HALF_BLINK,
    UI_EMOTION_BLINK,
    UI_EMOTION_LISTENING,
    UI_EMOTION_SPEAKING,
    UI_EMOTION_SLEEPY
} ui_emotion_t;

/**
 * @brief GC9A01 LCD 패널 및 LVGL 포트 초기화
 */
esp_err_t display_ui_init(void);

/**
 * @brief 양쪽 눈 표정 및 하단 상태 텍스트 변경
 */
void display_ui_set_emotion(ui_emotion_t left, ui_emotion_t right, const char *status_text);

/**
 * @brief 하단 상태 텍스트만 단독 변경
 */
void display_ui_set_status(const char *status_text);

/**
 * @brief 화면 중앙에 단독 텍스트 출력 (부팅/하드웨어 점검용)
 */
void display_ui_show_message(const char *msg);

/**
 * @brief 자연스러운 자동 눈 깜빡임 백그라운드 태스크 시작
 */
void display_ui_start_auto_blink(void);

/**
 * @brief 자동 눈 깜빡임 일시 정지/재개
 */
void display_ui_pause_auto_blink(bool pause);

/**
 * @brief LCD 패널 핸들 반환 (카메라 등 직접 제어 필요 시)
 */
esp_lcd_panel_handle_t display_ui_get_panel_handle(void);

#ifdef __cplusplus
}
#endif
