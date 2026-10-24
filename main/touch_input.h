#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*touch_settings_trigger_cb_t)(void);

/**
 * @brief I2C 정전식 터치 센서(SDA 5, SCL 6, INT 44) 초기화
 */
esp_err_t touch_input_init(void);

/**
 * @brief LVGL 인풋 디바이스 드라이버 등록
 */
esp_err_t touch_input_register_lvgl_indev(void);

/**
 * @brief 단일 터치 이벤트 확인 및 플래그 클리어 (기존 호환성)
 */
bool touch_input_has_event(void);

/**
 * @brief 현재 터치 상태 (누르고 있는지 여부)
 */
bool touch_input_is_active(void);

/**
 * @brief 최근 터치 좌표 및 상태 읽기
 */
bool touch_input_read_coords(int16_t *x, int16_t *y, bool *pressed);

/**
 * @brief 상단 10회 연속 터치 시 호출될 콜백 등록
 */
void touch_input_set_settings_trigger_cb(touch_settings_trigger_cb_t cb);

/**
 * @brief 현재 연속 상단 터치 카운트 반환 (10회 도달 시 리셋)
 */
int touch_input_get_top_tap_count(void);

#ifdef __cplusplus
}
#endif
