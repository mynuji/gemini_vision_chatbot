#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 설정 메뉴 UI 초기화
 */
esp_err_t ui_settings_init(void);

/**
 * @brief 설정 메뉴 화면 열기
 */
void ui_settings_open(void);

/**
 * @brief 설정 메뉴 화면 닫기 및 기존 표정 화면 복귀
 */
void ui_settings_close(void);

/**
 * @brief 현재 설정 메뉴 화면이 활성화되어 있는지 여부
 */
bool ui_settings_is_active(void);

#ifdef __cplusplus
}
#endif
