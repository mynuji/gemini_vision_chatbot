#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *server;
} ntp_server_info_t;

typedef struct {
    const char *name;
    const char *tz_posix;
} timezone_info_t;

/**
 * @brief 등록된 NTP 서버 개수 반환
 */
uint8_t time_sync_get_ntp_server_count(void);

/**
 * @brief 특정 인덱스의 NTP 서버 정보 반환
 */
const ntp_server_info_t* time_sync_get_ntp_server(uint8_t idx);

/**
 * @brief 특정 인덱스의 NTP 서버 이름 반환
 */
const char* time_sync_get_ntp_server_name(uint8_t idx);

/**
 * @brief 등록된 타임존 개수 반환
 */
uint8_t time_sync_get_timezone_count(void);

/**
 * @brief 특정 인덱스의 타임존 정보 반환
 */
const timezone_info_t* time_sync_get_timezone(uint8_t idx);

/**
 * @brief 특정 인덱스의 타임존 이름 반환
 */
const char* time_sync_get_timezone_name(uint8_t idx);

/**
 * @brief 타임존 초기 환경변수 설정 및 time_sync 모듈 초기화
 */
esp_err_t time_sync_init(void);

/**
 * @brief SNTP 서비스를 시작하여 NTP 서버로부터 시간 동기화 진행
 */
esp_err_t time_sync_start(void);

/**
 * @brief 현재 NTP 시간 동기화가 성공했는지 여부
 */
bool time_sync_is_synced(void);

/**
 * @brief 설정 변경 시 NTP 서버 및 타임존을 즉시 반영
 */
void time_sync_apply_settings(uint8_t server_idx, uint8_t tz_idx);

/**
 * @brief 현재 시각을 "HH:MM:SS" 문자열로 반환 (동기화 전이면 "--:--:--")
 */
void time_sync_get_time_str(char *buf, size_t max_len);

/**
 * @brief 현재 일시를 "YYYY-MM-DD HH:MM:SS" 문자열로 반환 (동기화 전이면 "Syncing...")
 */
void time_sync_get_date_time_str(char *buf, size_t max_len);

#ifdef __cplusplus
}
#endif
