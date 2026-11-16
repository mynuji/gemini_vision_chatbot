#include "wifi_ws.h"
#include "settings_manager.h"
#include "time_sync.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "WIFI_WS";

static esp_websocket_client_handle_t s_ws_client = NULL;
static volatile bool s_is_ws_connected = false;
static volatile bool s_is_wifi_connected = false;
static volatile bool s_is_scanning = false;
static SemaphoreHandle_t s_send_mutex = NULL;
static esp_timer_handle_t s_wifi_disconn_timer = NULL;
static esp_timer_handle_t s_ws_watchdog_timer = NULL;

static wifi_ws_state_cb_t s_state_cb = NULL;
static wifi_ws_rx_cb_t s_rx_cb = NULL;

static char s_ws_uri[128] = {0};
static char s_ip_str[24] = "0.0.0.0";

static void notify_state(wifi_ws_state_t state, const char *info) {
    if (s_state_cb != NULL) {
        s_state_cb(state, info);
    }
}

static void wifi_disconn_timer_cb(void *arg) {
    if (!s_is_wifi_connected) {
        notify_state(WIFI_WS_STATE_DISCONNECTED, "WIFI DISCONNECTED");
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket Connected to %s", s_ws_uri);
        s_is_ws_connected = true;
        notify_state(WIFI_WS_STATE_WS_CONNECTED, "JETSON CONNECTED");
        wifi_ws_send_client_info();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket Disconnected from %s", s_ws_uri);
        s_is_ws_connected = false;
        if (s_is_wifi_connected) {
            notify_state(WIFI_WS_STATE_WS_DISCONNECTED, "JETSON DISCONNECTED");
        } else {
            notify_state(WIFI_WS_STATE_DISCONNECTED, "WIFI DISCONNECTED");
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->data_len > 0 && s_rx_cb != NULL) {
            s_rx_cb((uint8_t)data->op_code, (const uint8_t *)data->data_ptr, (size_t)data->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        if (data != NULL) {
            ESP_LOGE(TAG, "WebSocket Error! type=%d, sock_errno=%d, tls_err=0x%x, http_status=%d",
                     data->error_handle.error_type,
                     data->error_handle.esp_transport_sock_errno,
                     data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_ws_handshake_status_code);
        } else {
            ESP_LOGE(TAG, "WebSocket Error event received (data is null)");
        }
        s_is_ws_connected = false;
        if (s_is_wifi_connected) {
            notify_state(WIFI_WS_STATE_WS_DISCONNECTED, "JETSON DISCONNECTED");
        }
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WebSocket Closed cleanly by server or client");
        s_is_ws_connected = false;
        if (s_is_wifi_connected) {
            notify_state(WIFI_WS_STATE_WS_DISCONNECTED, "JETSON DISCONNECTED");
        }
        break;

    default:
        break;
    }
}

static esp_err_t start_websocket_client_internal(void) {
    if (s_ws_uri[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws_client != NULL) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = s_ws_uri;
    ws_cfg.reconnect_timeout_ms = 3000;
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.ping_interval_sec = 15;      // 공유기 NAT 세션 만료 방지용 15초 하트비트 Ping
    ws_cfg.pingpong_timeout_sec = 20;   // 서버(Jetson) 부하/지연 고려하여 20초로 완화
    ws_cfg.enable_close_reconnect = true; // Clean Close 프레임 수신 시에도 태스크 종료되지 않고 자동 재접속!
    ws_cfg.task_stack = 8192;
    ws_cfg.buffer_size = 8192;  // 카메라 JPEG 및 오디오 패킷 초고속 송수신 (8KB 청크)

    ESP_LOGI(TAG, "Starting WebSocket client with URI: %s (Heap before init: %uKB)",
             s_ws_uri, (unsigned int)(esp_get_free_internal_heap_size() / 1024));
    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (s_ws_client != NULL) {
        esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)s_ws_client);
        esp_websocket_client_start(s_ws_client);
        notify_state(WIFI_WS_STATE_WS_CONNECTING, "CONNECTING TO JETSON...");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "esp_websocket_client_init failed! Out of memory? (FreeHeap: %uKB)",
                 (unsigned int)(esp_get_free_internal_heap_size() / 1024));
        return ESP_ERR_NO_MEM;
    }
}

esp_err_t wifi_ws_check_and_reconnect(void) {
    if (!s_is_wifi_connected || s_ws_uri[0] == '\0' || s_is_scanning) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_is_ws_connected && s_ws_client != NULL && esp_websocket_client_is_connected(s_ws_client)) {
        return ESP_OK;
    }

    // 과도한 재시작 방지 (최소 4초 간격)
    static uint32_t s_last_reconnect_attempt = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_last_reconnect_attempt < 4000) {
        return ESP_OK;
    }
    s_last_reconnect_attempt = now;

    ESP_LOGW(TAG, "WebSocket is disconnected while Wi-Fi is OK. Triggering reconnect/restart...");
    return start_websocket_client_internal();
}

static void ws_watchdog_timer_cb(void *arg) {
    if (s_is_wifi_connected && !s_is_ws_connected && !s_is_scanning) {
        wifi_ws_check_and_reconnect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi Station started, connecting to AP...");
        s_is_wifi_connected = false;
        s_is_ws_connected = false;

        // Wi-Fi 연결 해제 시 서버(WebSocket) 접속 시도 즉시 중단
        if (s_ws_client != NULL) {
            ESP_LOGI(TAG, "Wi-Fi disconnected -> stopping WebSocket client connection attempts");
            esp_websocket_client_stop(s_ws_client);
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
        }
        notify_state(WIFI_WS_STATE_WIFI_CONNECTING, "CONNECTING WIFI...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        int reason = disconn ? disconn->reason : -1;
        ESP_LOGW(TAG, "Wi-Fi Disconnected, reason: %d (%s), retrying...", reason,
                 (reason == 201) ? "NO_AP_FOUND" : (reason == 202 || reason == 15) ? "AUTH_FAIL" : "OTHER");
        s_is_wifi_connected = false;
        s_is_ws_connected = false;

        // Wi-Fi 연결 해제 시 서버(WebSocket) 접속 시도 즉시 중단
        if (s_ws_client != NULL) {
            ESP_LOGI(TAG, "Wi-Fi disconnected -> stopping WebSocket client connection attempts");
            esp_websocket_client_stop(s_ws_client);
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
        }

        // 약전계 환경에서 순간적인 비콘 누락으로 인한 1~2초 재연결 시 화면 깜빡임 방지 (2.5초 디바운스)
        if (s_wifi_disconn_timer == NULL) {
            esp_timer_create_args_t t_args = {};
            t_args.callback = wifi_disconn_timer_cb;
            t_args.name = "wifi_disconn";
            esp_timer_create(&t_args, &s_wifi_disconn_timer);
        }
        if (!esp_timer_is_active(s_wifi_disconn_timer)) {
            esp_timer_start_once(s_wifi_disconn_timer, 2500000);
        }

        if (!s_is_scanning) {
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "Wi-Fi disconnected event received during AP scan; suppressing auto-reconnect.");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_ip_str, sizeof(s_ip_str));
        s_is_wifi_connected = true;

        if (s_wifi_disconn_timer != NULL && esp_timer_is_active(s_wifi_disconn_timer)) {
            esp_timer_stop(s_wifi_disconn_timer);
        }
        ESP_LOGI(TAG, "Got IP: %s, GW: " IPSTR ", Mask: " IPSTR ", FreeHeap: %uKB",
                 s_ip_str,
                 IP2STR(&event->ip_info.gw),
                 IP2STR(&event->ip_info.netmask),
                 (unsigned int)(esp_get_free_internal_heap_size() / 1024));
        notify_state(WIFI_WS_STATE_WIFI_CONNECTED, "WIFI OK, WS CONN...");

        // Wi-Fi 연결 및 IP 획득 완료 -> NTP 시간 동기화 시작
        time_sync_start();

        // IP를 받았으므로 WebSocket 접속 시작
        if (s_ws_uri[0] != '\0') {
            start_websocket_client_internal();
        }
    }
}

esp_err_t wifi_ws_init(void) {
    ESP_LOGI(TAG, "Initializing NVS Flash, Netif & Event Loop...");

    if (s_send_mutex == NULL) {
        s_send_mutex = xSemaphoreCreateMutex();
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 8;
    cfg.static_tx_buf_num = 4;
    cfg.dynamic_tx_buf_num = 8;
    cfg.cache_tx_buf_num = 16;
    cfg.mgmt_sbuf_num = 6;
    cfg.rx_mgmt_buf_num = 2;

    ESP_LOGI(TAG, "Pre-WiFi Free Internal Heap: %u bytes, Largest block: %u bytes",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_LOGI(TAG, "Post-WiFi Free Internal Heap: %u bytes, Largest block: %u bytes",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // 한국 국가 코드(KR) 설정: 채널 1~13 활성화 및 액티브 스캔 허용 (채널 13번 탐색 안정성 대폭 향상)
    wifi_country_t country = {
        .cc = "KR",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    esp_wifi_set_country(&country);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    return ESP_OK;
}

esp_err_t wifi_ws_start(const char *ssid, const char *pass, const char *ws_uri) {
    if (ssid == NULL || ws_uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_ws_uri, ws_uri, sizeof(s_ws_uri) - 1);
    s_ws_uri[sizeof(s_ws_uri) - 1] = '\0';

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (pass != NULL) {
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi = -95;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 실시간 오디오 저지연 전송을 위해 절전 모드 비활성화
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // WebSocket 연결 상태 자동 복구 워치독 타이머 (4초 주기)
    if (s_ws_watchdog_timer == NULL) {
        esp_timer_create_args_t t_args = {};
        t_args.callback = ws_watchdog_timer_cb;
        t_args.name = "ws_watchdog";
        esp_timer_create(&t_args, &s_ws_watchdog_timer);
        esp_timer_start_periodic(s_ws_watchdog_timer, 4000000);
    }

    ESP_LOGI(TAG, "Wi-Fi STA started for SSID: %s (Power Save: NONE, WS Watchdog active)", ssid);
    return ESP_OK;
}

bool wifi_ws_is_wifi_connected(void) {
    return s_is_wifi_connected;
}

bool wifi_ws_is_connected(void) {
    return s_is_ws_connected && (s_ws_client != NULL) && esp_websocket_client_is_connected(s_ws_client);
}

esp_err_t wifi_ws_send_bin(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!wifi_ws_is_connected()) return ESP_ERR_INVALID_STATE;
    if (s_send_mutex == NULL) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(2500)) == pdTRUE) {
        esp_err_t ret = ESP_OK;
        if (wifi_ws_is_connected()) {
            int sent = esp_websocket_client_send_bin(s_ws_client, (const char *)data, len, pdMS_TO_TICKS(2500));
            if (sent < 0) ret = ESP_FAIL;
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(s_send_mutex);
        return ret;
    } else {
        ESP_LOGW(TAG, "wifi_ws_send_bin mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t wifi_ws_send_text(const char *text) {
    if (text == NULL) return ESP_ERR_INVALID_ARG;
    if (!wifi_ws_is_connected()) return ESP_ERR_INVALID_STATE;
    if (s_send_mutex == NULL) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(2500)) == pdTRUE) {
        esp_err_t ret = ESP_OK;
        if (wifi_ws_is_connected()) {
            int sent = esp_websocket_client_send_text(s_ws_client, text, strlen(text), pdMS_TO_TICKS(2500));
            if (sent < 0) ret = ESP_FAIL;
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(s_send_mutex);
        return ret;
    } else {
        ESP_LOGW(TAG, "wifi_ws_send_text mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
}

void wifi_ws_set_state_callback(wifi_ws_state_cb_t cb) {
    s_state_cb = cb;
}

void wifi_ws_set_rx_callback(wifi_ws_rx_cb_t cb) {
    s_rx_cb = cb;
}

const char* wifi_ws_get_ip_str(void) {
    return s_ip_str;
}

int wifi_ws_scan_aps(wifi_ap_info_t *out_list, int max_aps) {
    if (!out_list || max_aps <= 0) return 0;

    ESP_LOGI(TAG, "Starting Wi-Fi AP scan for up to %d networks...", max_aps);

    // 1. Wi-Fi 초기화 및 시작 상태 점검 (미시작 시 STA 모드로 자동 시작)
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t mode_err = esp_wifi_get_mode(&mode);
    if (mode_err != ESP_OK || mode == WIFI_MODE_NULL) {
        ESP_LOGI(TAG, "Wi-Fi not started yet. Setting STA mode and starting...");
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 2. 스캔 플래그 활성화 -> 백그라운드 이벤트 루프의 재연결 시도 억제
    s_is_scanning = true;

    // 3. 미연결 상태일 때 기존 연결 시도(CONNECTING 상태)를 명시적으로 해제
    // ESP-IDF는 Station이 CONNECTING 상태일 때 esp_wifi_scan_start()를 호출하면 ESP_ERR_WIFI_STATE를 반환함
    if (!s_is_wifi_connected) {
        ESP_LOGI(TAG, "Wi-Fi not connected: calling esp_wifi_disconnect() to stop ongoing connection attempts");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    // 4. 스캔 설정 (Active scan, 채널별 최소 100ms / 최대 200ms)
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 200;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "esp_wifi_scan_start returned ESP_ERR_WIFI_STATE, retrying after disconnect...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(&scan_cfg, true);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s (0x%x)", esp_err_to_name(err), err);
        s_is_scanning = false;
        if (!s_is_wifi_connected) {
            esp_wifi_connect();
        }
        return 0;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    ESP_LOGI(TAG, "esp_wifi_scan_get_ap_num found %u APs", ap_num);
    if (ap_num == 0) {
        ESP_LOGW(TAG, "No Wi-Fi APs found in scan.");
        s_is_scanning = false;
        if (!s_is_wifi_connected) {
            esp_wifi_connect();
        }
        return 0;
    }

    wifi_ap_record_t *records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!records) {
        ESP_LOGE(TAG, "Failed to allocate memory for AP scan records");
        s_is_scanning = false;
        if (!s_is_wifi_connected) {
            esp_wifi_connect();
        }
        return 0;
    }

    esp_wifi_scan_get_ap_records(&ap_num, records);

    int count = 0;
    for (int i = 0; i < ap_num && count < max_aps; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (strlen(ssid) == 0) continue;

        // 중복 SSID 방지 (신호가 가장 강한 첫 번째 것만 유지)
        bool duplicate = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(out_list[j].ssid, ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        strncpy(out_list[count].ssid, ssid, sizeof(out_list[count].ssid) - 1);
        out_list[count].ssid[sizeof(out_list[count].ssid) - 1] = '\0';
        out_list[count].rssi = records[i].rssi;
        out_list[count].authmode = records[i].authmode;
        count++;
    }

    free(records);
    s_is_scanning = false;

    // 미연결 상태였다면 재연결 시도 재개
    if (!s_is_wifi_connected) {
        esp_wifi_connect();
    }

    ESP_LOGI(TAG, "Scan finished: returned %d unique APs.", count);
    return count;
}

esp_err_t wifi_ws_reconnect_new_ap(const char *ssid, const char *pass) {
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Reconnecting to new Wi-Fi AP: SSID='%s'", ssid);

    // 기존 WebSocket 연결 종료
    if (s_ws_client != NULL) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
    s_is_ws_connected = false;
    s_is_wifi_connected = false;

    // 기존 Wi-Fi 연결 해제 후 새 config 설정
    esp_wifi_disconnect();

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (pass != NULL && pass[0] != '\0') {
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi = -95;
    wifi_config.sta.threshold.authmode = (pass != NULL && pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    notify_state(WIFI_WS_STATE_WIFI_CONNECTING, "CONNECTING WIFI...");
    return esp_wifi_connect();
}

esp_err_t wifi_ws_send_llm_type(uint8_t llm_type) {
    if (!s_is_ws_connected || s_ws_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *name = settings_manager_get_llm_type_name(llm_type);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"set_llm_type\",\"llm_type\":\"%s\"}", name);
    ESP_LOGI(TAG, "Sending LLM Type to server: %s", buf);
    return wifi_ws_send_text(buf);
}

esp_err_t wifi_ws_send_client_info(void) {
    if (!s_is_ws_connected || s_ws_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const app_settings_t *cfg = settings_manager_get();
    const char *llm_name = settings_manager_get_llm_type_name(cfg->llm_type);
    const ntp_server_info_t *ntp_info = time_sync_get_ntp_server(cfg->ntp_server_idx);
    const timezone_info_t *tz_info = time_sync_get_timezone(cfg->timezone_idx);
    const char *lang_str = (cfg->language == APP_LANG_KO) ? "ko" : "en";

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"client_info\","
             "\"volume\":%u,"
             "\"mic_gain\":%u,"
             "\"brightness\":%u,"
             "\"llm_type\":\"%s\","
             "\"llm_type_idx\":%u,"
             "\"ntp_server\":\"%s\","
             "\"ntp_server_host\":\"%s\","
             "\"ntp_server_idx\":%u,"
             "\"timezone\":\"%s\","
             "\"timezone_posix\":\"%s\","
             "\"timezone_idx\":%u,"
             "\"language\":\"%s\","
             "\"language_idx\":%u}",
             (unsigned int)cfg->volume,
             (unsigned int)cfg->mic_gain,
             (unsigned int)cfg->brightness,
             llm_name ? llm_name : "gemini api",
             (unsigned int)cfg->llm_type,
             (ntp_info && ntp_info->name) ? ntp_info->name : "time.google.com",
             (ntp_info && ntp_info->server) ? ntp_info->server : "time.google.com",
             (unsigned int)cfg->ntp_server_idx,
             (tz_info && tz_info->name) ? tz_info->name : "Seoul (GMT+9)",
             (tz_info && tz_info->tz_posix) ? tz_info->tz_posix : "KST-9",
             (unsigned int)cfg->timezone_idx,
             lang_str,
             (unsigned int)cfg->language);
    ESP_LOGI(TAG, "Sending client info to server: %s", buf);
    return wifi_ws_send_text(buf);
}


