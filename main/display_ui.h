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

/**
 * @brief 카메라 실시간 프리뷰 모드 시작 (눈 표정 숨김, 카메라 위젯 표시)
 */
void display_ui_start_camera_preview(void);

/**
 * @brief 카메라 실시간 프리뷰 모드 종료 (카메라 위젯 숨김, 눈 표정 복원)
 */
void display_ui_stop_camera_preview(void);

/**
 * @brief 카메라 프리뷰용 240x240 RGB565 버퍼 포인터 반환 (PSRAM)
 */
uint16_t *display_ui_get_preview_buffer(void);

/**
 * @brief 새 카메라 프레임이 버퍼에 채워졌을 때 화면 갱신 트리거
 */
void display_ui_refresh_camera_preview(void);

void display_ui_draw_preview_frame(const uint16_t *rgb565_buf);

/**
 * @brief LCD 백라이트 밝기 조절 (10 ~ 100%)
 */
void display_ui_set_brightness(uint8_t percent);

/**
 * @brief 시계 화면 모드 활성화 / 비활성화
 */
void display_ui_show_clock_mode(bool enable);

/**
 * @brief 현재 시계 화면 모드가 활성화되어 있는지 여부
 */
bool display_ui_is_clock_mode(void);

/**
 * @brief 시계 화면 일시 숨김 (대기 중 간헐적 눈 애니메이션 표시용)
 */
void display_ui_temp_hide_clock(bool hide);

/**
 * @brief 눈 뜨기(Wake-up) 인트로 애니메이션 즉시 트리거
 */
void display_ui_trigger_wake_anim(void);

/**
 * @brief 시계 화면 텍스트 즉시 갱신
 */
void display_ui_update_clock(void);

typedef enum {
    UI_WIFI_DISCONNECTED = 0,
    UI_WIFI_CONNECTING,
    UI_WIFI_CONNECTED
} ui_wifi_state_t;

/**
 * @brief LCD 상단 Wi-Fi 아이콘 상태 설정
 * - UI_WIFI_CONNECTED: 밝은 청록색 (0x00E5FF)
 * - UI_WIFI_CONNECTING: 주황/노란색 (0xFFB300)
 * - UI_WIFI_DISCONNECTED: 빨간색 (0xFF3333)
 */
void display_ui_set_wifi_state(ui_wifi_state_t state);

/**
 * @brief 상단 상태바(Wi-Fi 아이콘 및 설정 힌트 점) 표시/숨김 (설정 메뉴 등에서 활용)
 */
void display_ui_show_top_bar(bool show);

#ifdef __cplusplus
}
#endif
