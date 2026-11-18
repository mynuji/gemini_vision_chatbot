#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 🌐 멀티 프로젝트 통합 펌웨어 배포 저장소 (Public)
#define OTA_DEFAULT_REPO_OWNER    "mynuji"
#define OTA_DEFAULT_REPO_NAME     "esp32_firmware_releases"
#define OTA_DEFAULT_BRANCH        "main"
#define OTA_DEFAULT_PROJECT_NAME  "gemini_vision_chatbot"

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_CONNECTING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_FINISHING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

typedef struct {
    char latest_version[32];   // version.txt에서 읽은 최신 버전 (예: "1.0.1")
    char current_version[32];  // 현재 기기에 설치된 버전 (예: "1.0.0")
    char download_url[256];    // 펌웨어 바이너리 raw URL
    int  asset_size;           // 바이너리 파일 크기 (바이트, 알 수 없는 경우 0)
    bool has_update;           // 최신 버전이 현재 버전보다 새로운지 여부
    char error_msg[64];        // 에러 발생 시 안내 메시지
} ota_release_info_t;

typedef void (*ota_progress_cb_t)(ota_state_t state, int progress_pct, const char *msg);

/**
 * @brief 현재 실행 중인 펌웨어의 버전 문자열 반환 (예: "1.0.0")
 */
const char* ota_updater_get_current_version(void);

/**
 * @brief Public GitHub raw 엔드포인트에서 최신 버전 정보 확인 (인증 토큰 일체 불필요)
 * @param project_name 프로젝트 디렉토리명 (NULL일 경우 기본 "gemini_vision_chatbot")
 * @param out_info 결과 구조체 포인터
 * @return ESP_OK 성공, 그 외 에러 코드
 */
esp_err_t ota_updater_check_latest_version(const char *project_name, ota_release_info_t *out_info);

/**
 * @brief 백그라운드 태스크에서 펌웨어 바이너리를 다운로드하여 OTA 플래시 실행 (토큰 불필요)
 * @param download_url 다운로드할 raw 바이너리 URL (NULL일 경우 최신 기본 URL 사용)
 * @param cb 진행 상태 및 완료 알림 콜백
 * @return ESP_OK 작업 시작 성공, ESP_ERR_INVALID_STATE 이미 진행 중
 */
esp_err_t ota_updater_start(const char *download_url, ota_progress_cb_t cb);

/**
 * @brief 현재 OTA 진행 중 여부 확인
 */
bool ota_updater_is_running(void);

/**
 * @brief 진행 중인 OTA 취소 요청
 */
void ota_updater_cancel(void);

#ifdef __cplusplus
}
#endif
