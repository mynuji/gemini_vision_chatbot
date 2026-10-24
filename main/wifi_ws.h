#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_WS_STATE_DISCONNECTED = 0,
    WIFI_WS_STATE_WIFI_CONNECTING,
    WIFI_WS_STATE_WIFI_CONNECTED,
    WIFI_WS_STATE_WS_CONNECTING,
    WIFI_WS_STATE_WS_CONNECTED
} wifi_ws_state_t;

/**
 * @brief 네트워크 상태 변경 알림 콜백 함수 타입
 */
typedef void (*wifi_ws_state_cb_t)(wifi_ws_state_t state, const char *info);

/**
 * @brief WebSocket 수신 데이터 콜백 함수 타입 (op_code: 0x01 Text, 0x02 Binary)
 */
typedef void (*wifi_ws_rx_cb_t)(uint8_t op_code, const uint8_t *data, size_t len);

/**
 * @brief Wi-Fi 및 WebSocket 드라이버 초기화 (NVS, Netif, Event Loop 등)
 */
esp_err_t wifi_ws_init(void);

/**
 * @brief Wi-Fi 연결 및 WebSocket 클라이언트 접속 시작
 */
esp_err_t wifi_ws_start(const char *ssid, const char *pass, const char *ws_uri);

/**
 * @brief 젯슨 서버와의 WebSocket 연결 여부 확인
 */
bool wifi_ws_is_connected(void);

/**
 * @brief 젯슨 서버로 바이너리 데이터(마이크 PCM 등) 전송 (스레드 세이프)
 */
esp_err_t wifi_ws_send_bin(const uint8_t *data, size_t len);

/**
 * @brief 젯슨 서버로 텍스트 데이터(JSON 등) 전송 (스레드 세이프)
 */
esp_err_t wifi_ws_send_text(const char *text);

/**
 * @brief 네트워크 상태 콜백 등록 (UI 상태 갱신용)
 */
void wifi_ws_set_state_callback(wifi_ws_state_cb_t cb);

void wifi_ws_set_rx_callback(wifi_ws_rx_cb_t cb);

/**
 * @brief 할당받은 IP 주소 문자열 반환 (예: "192.168.0.56")
 */
const char* wifi_ws_get_ip_str(void);

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_ap_info_t;

/**
 * @brief 주변 2.4GHz Wi-Fi AP들을 검색하여 반환
 * @param ap_list 결과를 저장할 배열 포인터
 * @param max_aps 최대 저장 개수
 * @return 실제로 검색된 AP 개수
 */
int wifi_ws_scan_aps(wifi_ap_info_t *ap_list, int max_aps);

#ifdef __cplusplus
}
#endif
