#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ⚠️ sos_gateway_license 및 sos_gateway_telegram_v3 공용 마스터 키
#define LICENSE_MASTER_KEY_PRIMARY   "Youngik_Secure_SOS_Key_2026!@#"
#define LICENSE_MASTER_KEY_SECONDARY "Secure_SOS_Key_2026!@#"

/**
 * @brief 라이선스 관리자 초기화 (NVS 로드 및 기기 MAC 확인)
 */
esp_err_t license_manager_init(void);

/**
 * @brief 현재 기기의 Wi-Fi STA MAC 주소 문자열 반환 (예: "E0:72:A1:FB:4F:50")
 */
const char* license_manager_get_mac(void);

/**
 * @brief NVS "security" 영역에 저장된 라이선스 키 반환 (없으면 빈 문자열)
 */
const char* license_manager_get_stored_license(void);

/**
 * @brief 현재 기기 MAC과 마스터 키로 계산된 정품 기대 라이선스 키 반환
 */
const char* license_manager_get_expected_license(void);

/**
 * @brief 기기 라이선스 유효성 검증
 * @return true 유효한 정품 라이선스, false 인증 실패 또는 미등록
 */
bool license_manager_validate(void);

/**
 * @brief 수동으로 라이선스 키를 NVS "security" 영역에 저장
 */
esp_err_t license_manager_set_license(const char *license_key);

/**
 * @brief 현재 기기 MAC으로 라이선스 키를 자동 생성하여 NVS에 등록 (자가 프로비저닝)
 */
esp_err_t license_manager_auto_provision(void);

#ifdef __cplusplus
}
#endif
