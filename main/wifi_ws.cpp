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
static SemaphoreHandle_t s_send_mutex = NULL;
static esp_timer_handle_t s_wifi_disconn_timer = NULL;

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

    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi Station started, connecting to AP...");
        s_is_wifi_connected = false;
        s_is_ws_connected = false;
        notify_state(WIFI_WS_STATE_WIFI_CONNECTING, "CONNECTING WIFI...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        int reason = disconn ? disconn->reason : -1;
        ESP_LOGW(TAG, "Wi-Fi Disconnected, reason: %d (%s), retrying...", reason,
                 (reason == 201) ? "NO_AP_FOUND" : (reason == 202 || reason == 15) ? "AUTH_FAIL" : "OTHER");
        s_is_wifi_connected = false;
        s_is_ws_connected = false;

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

        static bool scanned_once = false;
        if (reason == 201 && !scanned_once) {
            scanned_once = true;
            ESP_LOGI(TAG, "Scanning for visible 2.4GHz Wi-Fi networks nearby...");
            wifi_scan_config_t scan_cfg = {};
            esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
            if (scan_err == ESP_OK) {
                uint16_t ap_num = 0;
                esp_wifi_scan_get_ap_num(&ap_num);
                if (ap_num > 0) {
                    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_num);
                    if (ap_list) {
                        esp_wifi_scan_get_ap_records(&ap_num, ap_list);
                        ESP_LOGI(TAG, "==== Found %d 2.4GHz Wi-Fi APs ====", ap_num);
                        for (int i = 0; i < ap_num && i < 20; i++) {
                            ESP_LOGI(TAG, "  - SSID: '%s' (RSSI: %d, Ch: %d)",
                                     ap_list[i].ssid, ap_list[i].rssi, ap_list[i].primary);
                        }
                        free(ap_list);
                    }
                } else {
                    ESP_LOGW(TAG, "No Wi-Fi networks found in 2.4GHz scan!");
                }
            }
        }

        esp_wifi_connect();
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
            if (s_ws_client != NULL) {
                esp_websocket_client_stop(s_ws_client);
                esp_websocket_client_destroy(s_ws_client);
                s_ws_client = NULL;
            }

            esp_websocket_client_config_t ws_cfg = {};
            ws_cfg.uri = s_ws_uri;
            ws_cfg.reconnect_timeout_ms = 2000;
            ws_cfg.network_timeout_ms = 8000;
            ws_cfg.task_stack = 3584;   // 6144 -> 3584 (내부 SRAM 절약)
            ws_cfg.buffer_size = 2048;  // 4096 -> 2048 (제어 JSON 및 오디오 수신에 충분)

            ESP_LOGI(TAG, "Starting WebSocket client with URI: %s (Heap before init: %uKB)",
                     s_ws_uri, (unsigned int)(esp_get_free_internal_heap_size() / 1024));
            s_ws_client = esp_websocket_client_init(&ws_cfg);
            if (s_ws_client != NULL) {
                esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)s_ws_client);
                esp_websocket_client_start(s_ws_client);
                notify_state(WIFI_WS_STATE_WS_CONNECTING, "CONNECTING TO JETSON...");
            } else {
                ESP_LOGE(TAG, "esp_websocket_client_init failed! Out of memory? (FreeHeap: %uKB)",
                         (unsigned int)(esp_get_free_internal_heap_size() / 1024));
            }
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
    cfg.dynamic_rx_buf_num = 16;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

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

    ESP_LOGI(TAG, "Wi-Fi STA started for SSID: %s (Power Save: NONE)", ssid);
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
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 250;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        ESP_LOGW(TAG, "No Wi-Fi APs found in scan.");
        return 0;
    }

    wifi_ap_record_t *records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!records) {
        ESP_LOGE(TAG, "Failed to allocate memory for AP scan records");
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
    ESP_LOGI(TAG, "Scan finished: returned %d unique APs.", count);
    return count;
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


