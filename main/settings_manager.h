#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_MAGIC 0x53455431 // "SET1"

typedef struct {
    uint32_t magic;
    uint8_t  volume;          // 10 ~ 100 (%)
    char     wifi_ssid[33];   // SSID (최대 32자 + null)
    char     wifi_pass[65];   // 비밀번호 (최대 64자 + null)
    char     server_ip[64];   // 서버 IP 또는 호스트 (예: "192.168.0.2")
    uint16_t server_port;     // 서버 포트 (기본값: 8765)
    uint8_t  brightness;      // 10 ~ 100 (%)
    uint8_t  mic_gain;        // 10 ~ 50 (1.0x ~ 5.0x, 기본값: 35)
} app_settings_t;

/**
 * @brief NVS에서 설정을 로드. 저장된 설정이 없으면 기본값으로 초기화하고 저장.
 */
esp_err_t settings_manager_init(void);

/**
 * @brief 현재 활성화된 설정 구조체 포인터 반환
 */
const app_settings_t* settings_manager_get(void);

/**
 * @brief 설정을 NVS에 저장
 */
esp_err_t settings_manager_save(const app_settings_t *new_settings);

/**
 * @brief 설정을 초기 기본값으로 복원
 */
esp_err_t settings_manager_reset_to_default(void);

/**
 * @brief 볼륨 설정만 빠르게 변경 및 저장
 */
esp_err_t settings_manager_set_volume(uint8_t volume);

/**
 * @brief 화면 밝기 설정만 빠르게 변경 및 저장
 */
esp_err_t settings_manager_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
