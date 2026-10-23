#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 정전식 터치 센서(GPIO 44) 인터럽트 초기화
 */
esp_err_t touch_input_init(void);

/**
 * @brief 터치 발생 이벤트가 있었는지 확인하고 플래그 클리어 (1회성 소비)
 */
bool touch_input_has_event(void);

/**
 * @brief 현재 터치 상태 판정 (최근 300ms 이내에 펄스가 있었는지 여부)
 */
bool touch_input_is_active(void);

/**
 * @brief 마지막 터치 펄스 발생 시각(ms) 반환
 */
uint32_t touch_input_get_last_isr_time(void);

#ifdef __cplusplus
}
#endif
