#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_MAGIC 0x53455433 // "SET3"

typedef enum {
    LLM_TYPE_GEMINI_API = 0,
    LLM_TYPE_GEMINI_REALTIME,
    LLM_TYPE_GEMINI_NATIVE_AUDIO,
    LLM_TYPE_CHATGPT_REALTIME,
    LLM_TYPE_LOCAL_LLM,
    LLM_TYPE_COUNT
} llm_type_t;

typedef enum {
    APP_LANG_KO = 0, // 한국어
    APP_LANG_EN = 1  // English
} app_language_t;

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
    uint8_t  ntp_server_idx;  // 0: google, 1: cloudflare, 2: pool.ntp.org, 3: ms, 4: apple
    uint8_t  timezone_idx;    // 0: Seoul (GMT+9), 1: GMT-11, ...
    uint8_t  language;        // 0: 한국어, 1: English
} app_settings_t;

const char* settings_manager_get_llm_type_name(uint8_t type);
app_language_t settings_manager_get_language(void);
esp_err_t settings_manager_set_language(app_language_t lang);

typedef void (*settings_change_cb_t)(const app_settings_t *new_settings);

/**
 * @brief 설정 변경 시 호출될 콜백 등록
 */
void settings_manager_set_change_callback(settings_change_cb_t cb);

/**
 * @brief NVS에서 설정을 로드. 저장된 설정이 없으면 기본값으로 초기화하고 저장.
 */
esp_err_t settings_manager_init(void);

/**
 * @brief 현재 활성화된 설정 구조체 포인터 반환
 */
const app_settings_t* settings_manager_get(void);

/**
 * @brief 설정을 NVS에 저장 (비동기 큐잉 - UI/웹소켓 스레드 블로킹 및 캐시 패닉 방지)
 */
esp_err_t settings_manager_save(const app_settings_t *new_settings);

/**
 * @brief 설정을 NVS에 동기적으로 저장 (저장 완료까지 대기 - 재부팅 전 사용)
 */
esp_err_t settings_manager_save_sync(const app_settings_t *new_settings);

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

/**
 * @brief LLM 타입 변경 및 저장 (콜백 미호출 - 서버로부터 수신 시 재귀 전송 방지용)
 */
esp_err_t settings_manager_set_llm_type_no_notify(uint8_t llm_type);

/**
 * @brief 볼륨 설정만 빠르게 변경 및 저장 (콜백 미호출)
 */
esp_err_t settings_manager_set_volume_no_notify(uint8_t volume);

/**
 * @brief 마이크 감도 설정만 빠르게 변경 및 저장 (콜백 미호출)
 */
esp_err_t settings_manager_set_mic_gain_no_notify(uint8_t mic_gain);

/**
 * @brief NTP 서버 인덱스 변경 및 저장
 */
esp_err_t settings_manager_set_ntp_server(uint8_t ntp_server_idx);

/**
 * @brief 타임존(지역) 인덱스 변경 및 저장
 */
esp_err_t settings_manager_set_timezone(uint8_t timezone_idx);

#ifdef __cplusplus
}
#endif
