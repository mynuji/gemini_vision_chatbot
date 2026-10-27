#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_MAGIC 0x53455432 // "SET2"

typedef enum {
    LLM_TYPE_GEMINI_API = 0,
    LLM_TYPE_GEMINI_REALTIME,
    LLM_TYPE_CHATGPT_REALTIME,
    LLM_TYPE_LOCAL_LLM,
    LLM_TYPE_COUNT
} llm_type_t;

typedef struct {
    uint32_t magic;
    uint8_t  volume;          // 10 ~ 100 (%)
    char     wifi_ssid[33];   // SSID (최대 32자 + null)
    char     wifi_pass[65];   // 비밀번호 (최대 64자 + null)
    char     server_ip[64];   // 서버 IP 또는 호스트 (예: "192.168.0.2")
    uint16_t server_port;     // 서버 포트 (기본값: 8765)
    uint8_t  brightness;      // 10 ~ 100 (%)
    uint8_t  mic_gain;        // 10 ~ 50 (1.0x ~ 5.0x, 기본값: 35)
    uint8_t  llm_type;        // 0: gemini api, 1: gemini realtime api, 2: chatgpt realtime api, 3: local llm
} app_settings_t;

const char* settings_manager_get_llm_type_name(uint8_t type);

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

/**
 * @brief 마이크 감도 설정만 빠르게 변경 및 저장 (10 ~ 60, 즉 1.0x ~ 6.0x)
 */
esp_err_t settings_manager_set_mic_gain(uint8_t mic_gain);

/**
 * @brief 서버 포트 번호 변경 및 저장
 */
esp_err_t settings_manager_set_port(uint16_t port);

/**
 * @brief LLM 타입 변경 및 저장
 */
esp_err_t settings_manager_set_llm_type(uint8_t llm_type);

#ifdef __cplusplus
}
#endif
