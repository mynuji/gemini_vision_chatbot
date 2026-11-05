#include "app_config.h"
#include "settings_manager.h"
#include "time_sync.h"
#include "ui_settings.h"
#include "bsp_board.h"
#include "display_ui.h"
#include "audio_io.h"
#include "touch_input.h"
#include "wifi_ws.h"
#include "chat_state.h"
#include "audio_pipeline.h"
#include "camera_io.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MAIN_APP";

// 시계 화면 표시 타이머 상태 변수
static uint32_t s_boot_clock_timeout = 0;   // NTP 동기화 대기 최대 30초 타임아웃
static uint32_t s_boot_clock_end_time = 0;  // 동기화 완료 후 5초 표시 시간
static bool s_boot_clock_active = false;
static bool s_boot_clock_synced_seen = false;
static uint32_t s_last_ntp_retry_time = 0;  // 1분 주기 재시도 타임스탬프

static uint32_t s_idle_clock_start_time = 0;
static bool s_idle_clock_active = false;
static uint32_t s_next_eye_peek_time = 0;
static uint32_t s_eye_peek_end_time = 0;
static bool s_eye_peeking = false;

static void on_ntp_time_synced(void) {
    ESP_LOGI(TAG, "NTP Time Synced Callback received!");
    display_ui_update_clock();
    if (s_boot_clock_active) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        s_boot_clock_synced_seen = true;
        s_boot_clock_end_time = now + 5000;
        s_idle_clock_start_time = now + 5000;
        ESP_LOGI(TAG, "NTP synced during boot -> Displaying clock for 5 seconds before switching to eyes");
    }
}

/**
 * @brief 네트워크(Wi-Fi / WebSocket) 상태 변경 콜백
 */
static void on_network_state_changed(wifi_ws_state_t state, const char *info) {
    ESP_LOGI(TAG, "Network State Changed: %d (%s)", state, info);
    bool is_ko = (settings_manager_get_language() == APP_LANG_KO);

    switch (state) {
    case WIFI_WS_STATE_WIFI_CONNECTING:
        display_ui_set_status(is_ko ? "와이파이 연결 중..." : "CONNECTING WIFI...");
        break;

    case WIFI_WS_STATE_WIFI_CONNECTED:
        display_ui_set_status(is_ko ? "와이파이 연결됨! 서버 접속 중..." : "WIFI OK! WS CONN...");
        break;

    case WIFI_WS_STATE_WS_CONNECTING:
        display_ui_set_status(is_ko ? "서버 접속 중..." : "CONNECTING JETSON...");
        break;

    case WIFI_WS_STATE_WS_CONNECTED:
        ESP_LOGI(TAG, "WebSocket Connected to Jetson Xavier! Setting state to IDLE.");
        display_ui_set_status(is_ko ? "서버 연결 완료!" : "JETSON CONNECTED!");
        audio_io_play_tone(1046.50f, 150); // 연결 성공 맑은 비프음 (C6)
        chat_state_set(CHAT_STATE_IDLE);
        break;

    case WIFI_WS_STATE_DISCONNECTED:
    default:
        audio_pipeline_clear_audio();
        chat_state_set(CHAT_STATE_IDLE);
        display_ui_set_status(is_ko ? "연결 끊김" : (info ? info : "DISCONNECTED"));
        break;
    }
}

/**
 * @brief 젯슨 서버로부터 수신된 데이터 콜백
 */
static void on_websocket_data_received(uint8_t op_code, const uint8_t *data, size_t len) {
    if (op_code == 0x01 && data != NULL && len > 0) {
        // 텍스트 메시지 (JSON 제어 명령 또는 DONE, STATUS 등)
        chat_state_handle_server_text((const char *)data, len);
    } else if (op_code == 0x02 && data != NULL && len > 0) {
        // 바이너리 데이터 (AI TTS 16kHz 16-bit Mono PCM)
        audio_pipeline_feed_tts_audio(data, len);
    }
}

/**
 * @brief 대화 상태 주기적 점검 및 터치 감지 태스크
 */
static void chat_monitor_loop_task(void *pvParameters) {
    ESP_LOGI(TAG, "Chat monitor and touch loop task started.");

    uint32_t last_touch_handled_time = 0;
    uint32_t last_status_log_time = 0;

    while (1) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        bool has_event = touch_input_has_event();

        // 0. 설정 메뉴가 열려 있을 때는 메인 대화 터치 동작 중단 및 대기 타이머 일시 리셋
        if (ui_settings_is_active()) {
            if (s_idle_clock_active || s_boot_clock_active) {
                s_idle_clock_active = false;
                s_boot_clock_active = false;
                s_eye_peeking = false;
                display_ui_show_clock_mode(false);
            }
            s_idle_clock_start_time = now;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 1. 터치 감지 처리: 인터럽트 에지 이벤트 기반 1회 즉시 감지 (디바운스 600ms)
        if (has_event && (now - last_touch_handled_time > 600)) {
            ESP_LOGI(TAG, "User Touch Event Handled! Forwarding to chat_state_handle_touch()");
            if (s_boot_clock_active || s_idle_clock_active) {
                s_boot_clock_active = false;
                s_idle_clock_active = false;
                s_eye_peeking = false;
                display_ui_show_clock_mode(false);
            }
            s_idle_clock_start_time = now;

            chat_state_handle_touch();
            chat_state_activity_occurred();
            last_touch_handled_time = now;
        }

        // 1-1. 부팅 시계 화면 관리
        // - 동기화 전: 최대 30초 대기 (시간 표시 없이 '동기화 중...' 표시)
        // - 동기화 완료 시: 5초간 실제 시계 표시 후 눈 화면 전환
        // - 30초 초과 시: 눈 화면으로 전환 후 백그라운드에서 1분 주기로 동기화 계속 시도
        // - 시계 표시 중에도 'Alexa' 발화나 터치 시 즉시 전환
        if (s_boot_clock_active) {
            if (chat_state_get() != CHAT_STATE_IDLE) {
                s_boot_clock_active = false;
                display_ui_show_clock_mode(false);
                s_idle_clock_start_time = now;
            } else if (s_boot_clock_synced_seen) {
                // 이미 동기화되어 5초 타이머가 동작 중인 경우
                if (now >= s_boot_clock_end_time) {
                    s_boot_clock_active = false;
                    display_ui_show_clock_mode(false);
                    display_ui_set_emotion(UI_EMOTION_IDLE, UI_EMOTION_IDLE, NULL);
                    s_idle_clock_start_time = now;
                    ESP_LOGI(TAG, "Boot clock 5-second display completed -> Switched to Character Eyes");
                }
            } else {
                // 아직 동기화되지 않은 상태: 동기화 완료 여부 즉각 확인 또는 30초 타임아웃 검사
                if (time_sync_is_synced()) {
                    s_boot_clock_synced_seen = true;
                    s_boot_clock_end_time = now + 5000;
                    s_idle_clock_start_time = now + 5000;
                    display_ui_update_clock();
                    ESP_LOGI(TAG, "NTP synced detected during boot -> Displaying clock for 5 seconds");
                } else if (now >= s_boot_clock_timeout) {
                    // 30초 이상 동기화 지연 시: 다음 화면(눈)으로 넘어가게 한 후, 1분 주기로 NTP 동기화 시도
                    s_boot_clock_active = false;
                    display_ui_show_clock_mode(false);
                    display_ui_set_emotion(UI_EMOTION_IDLE, UI_EMOTION_IDLE, NULL);
                    s_idle_clock_start_time = now;
                    ESP_LOGW(TAG, "NTP sync boot timeout (>30s) -> Switched to Eyes. Retrying NTP every 1 minute.");
                }
            }
        }

        // 1-2. NTP 미동기화 시 1분(60초) 간격으로 NTP 재시도
        if (!time_sync_is_synced()) {
            if (now - s_last_ntp_retry_time >= 60000) {
                s_last_ntp_retry_time = now;
                ESP_LOGI(TAG, "NTP not synced yet -> Retrying NTP sync (1-min interval)...");
                time_sync_start();
            }
        }

        // 1-3. 5분 대기 초과 시 시계 표시 및 간헐적 눈 애니메이션 관리
        if (!s_boot_clock_active) {
            chat_state_t cur_state = chat_state_get();
            if (cur_state != CHAT_STATE_IDLE) {
                // 대화 진행 중(LISTENING, THINKING, SPEAKING)일 때는 시계 해제 및 대기 시간 리셋
                s_idle_clock_start_time = now;
                if (s_idle_clock_active) {
                    s_idle_clock_active = false;
                    s_eye_peeking = false;
                    display_ui_show_clock_mode(false);
                }
            } else {
                // CHAT_STATE_IDLE 상태 (Alexa 호출 대기 중)
                if (!s_idle_clock_active) {
                    // 5분(300,000ms) 초과 여부 검사
                    if (now - s_idle_clock_start_time >= 300000) {
                        s_idle_clock_active = true;
                        display_ui_show_clock_mode(true);
                        // 약 30초~1분(30,000 ~ 60,000ms) 사이 첫 번째 눈 깜빡임 시점 계산
                        s_next_eye_peek_time = now + 30000 + (esp_random() % 30000);
                        s_eye_peeking = false;
                        ESP_LOGI(TAG, "Standby idle > 5 minutes -> Switching to Clock display mode");
                    }
                } else {
                    // 시계 표시 모드 유지 중: 30초~1분 사이마다 간헐적으로 눈 애니메이션 표시
                    if (!s_eye_peeking) {
                        if (now >= s_next_eye_peek_time) {
                            s_eye_peeking = true;
                            s_eye_peek_end_time = now + 3500; // 약 3.5초 동안 눈 표시 및 깜빡임
                            display_ui_temp_hide_clock(true);
                            ESP_LOGI(TAG, "Clock peek: displaying eyes to indicate Alexa standby");
                        }
                    } else {
                        if (now >= s_eye_peek_end_time) {
                            s_eye_peeking = false;
                            s_next_eye_peek_time = now + 30000 + (esp_random() % 30000); // 다음 30~60초 후
                            display_ui_temp_hide_clock(false);
                            ESP_LOGI(TAG, "Clock peek: returning to Clock display");
                        }
                    }
                }
            }
        }

        // 2. 대화 상태 머신 타임아웃 틱 (최신 타임스탬프로 갱신 전달)
        chat_state_tick(now);

        // 3. 주기적 헬스 체크 로그 (3초 간격)
        if (now - last_status_log_time >= 3000) {
            chat_state_t st = chat_state_get();
            const char *st_name = (st == CHAT_STATE_IDLE) ? "IDLE" :
                                  (st == CHAT_STATE_LISTENING) ? "LISTENING" :
                                  (st == CHAT_STATE_THINKING) ? "THINKING" : "SPEAKING";
            ESP_LOGI(TAG, "Health Check: State=%s(%d), ClockMode=%s, WS=%s, FreeHeap=%uKB",
                     st_name, (int)st,
                     s_boot_clock_active ? "BOOT_CLOCK" : s_idle_clock_active ? (s_eye_peeking ? "EYE_PEEK" : "IDLE_CLOCK") : "EYES",
                     wifi_ws_is_connected() ? "CONNECTED" : "NO",
                     (unsigned int)(esp_get_free_internal_heap_size() / 1024));
            last_status_log_time = now;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "   AI Chatbot - Step 3 (Voice Streaming & Barge-in) ");
    ESP_LOGI(TAG, "   Build Time: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "   Target Env: %s (%s)", CONFIG_ENV_NAME, CONFIG_WS_SERVER_URI);
    ESP_LOGI(TAG, "==================================================");

    ESP_LOGI(TAG, "Initial Heap: Internal: %u KB, PSRAM: %u KB",
             (unsigned int)(esp_get_free_internal_heap_size() / 1024),
             (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    // [0] NVS 영구 설정 로드
    ESP_ERROR_CHECK(settings_manager_init());
    const app_settings_t *cfg = settings_manager_get();

    // [0-1] 타임존 환경 변수 설정 및 NTP 완료 콜백 등록
    time_sync_init();
    time_sync_set_callback(on_ntp_time_synced);

    // [1] 디스플레이 및 LVGL 초기화
    ESP_ERROR_CHECK(display_ui_init());
    display_ui_set_brightness(cfg->brightness);

    // [1-1] 부팅 즉시 시계 화면 활성화 (NTP 동기화 대기 최대 30초, 동기화 완료 시 5초 카운트다운)
    display_ui_show_clock_mode(true);
    s_boot_clock_active = true;
    s_boot_clock_synced_seen = time_sync_is_synced();
    uint32_t boot_now = (uint32_t)(esp_timer_get_time() / 1000);
    s_boot_clock_timeout = boot_now + 30000;
    s_last_ntp_retry_time = boot_now;
    if (s_boot_clock_synced_seen) {
        s_boot_clock_end_time = boot_now + 5000;
        s_idle_clock_start_time = boot_now + 5000;
    } else {
        s_boot_clock_end_time = 0;
        s_idle_clock_start_time = boot_now + 30000;
    }

    // [2] 터치 센서 초기화 및 상단 10회 터치 콜백 등록
    ESP_ERROR_CHECK(touch_input_init());
    touch_input_set_settings_trigger_cb(ui_settings_open);

    // [3] 오디오 I/O (PDM RX 마이크 + I2S TX 스피커) 초기화
    ESP_ERROR_CHECK(audio_io_init());

    // 스피커 동작 확인: '도-미-솔' 부팅 멜로디
    audio_io_play_boot_melody();

    // [4] 자동 깜빡임 태스크 시작 (이후 캐릭터 눈으로 전환될 때 동작)
    display_ui_start_auto_blink();

    // [5] 대화 상태 머신 초기화
    chat_state_init();

    // [6] 네트워크 (Wi-Fi + WebSocket) 드라이버 초기화
    ESP_LOGI(TAG, "Initializing Wi-Fi & WebSocket driver...");
    wifi_ws_set_state_callback(on_network_state_changed);
    wifi_ws_set_rx_callback(on_websocket_data_received);
    ESP_ERROR_CHECK(wifi_ws_init());

    // [7] 실시간 오디오 파이프라인 (PSRAM RingBuffers, FreeRTOS Tasks, esp-sr AFE) 초기화
    ESP_ERROR_CHECK(audio_pipeline_init());

    // [7-1] OV2640 카메라 초기화 (비전 질문용) - 실패해도 음성 챗봇은 계속 동작
    esp_err_t cam_err = camera_io_init();
    if (cam_err == ESP_OK) {
        // 부팅 시 테스트 촬영 1장으로 센서/프레임 버퍼 동작 확인
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        uint16_t w = 0, h = 0;
        if (camera_io_capture_jpeg(&jpeg, &jpeg_len, &w, &h) == ESP_OK) {
            ESP_LOGI(TAG, "Camera self-test OK: %ux%u JPEG, %u bytes", (unsigned int)w, (unsigned int)h, (unsigned int)jpeg_len);
            free(jpeg);
        } else {
            ESP_LOGW(TAG, "Camera self-test capture failed");
        }
    } else {
        ESP_LOGW(TAG, "Camera init failed (%s). Continuing without vision.", esp_err_to_name(cam_err));
    }

    // [8] 대화 상태 모니터 및 터치 루프 태스크 시작 (PSRAM 스택)
    xTaskCreatePinnedToCoreWithCaps(chat_monitor_loop_task, "chat_monitor", 4096, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // [9] Wi-Fi 및 WebSocket 접속 시작 (NVS 동적 설정 사용)
    ESP_LOGI(TAG, "Starting Wi-Fi & WebSocket connection to %s:%u...", cfg->server_ip, (unsigned int)cfg->server_port);
    char ws_uri[128];
    snprintf(ws_uri, sizeof(ws_uri), "ws://%s:%u", cfg->server_ip, (unsigned int)cfg->server_port);
    ESP_ERROR_CHECK(wifi_ws_start(cfg->wifi_ssid, cfg->wifi_pass, ws_uri));

    // [10] 모든 시스템 부팅 완료 시점 기준으로 동기화 상태 갱신
    boot_now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_boot_clock_synced_seen && time_sync_is_synced()) {
        s_boot_clock_synced_seen = true;
        s_boot_clock_end_time = boot_now + 5000;
        s_idle_clock_start_time = boot_now + 5000;
    }
    display_ui_update_clock();

    ESP_LOGI(TAG, "Initialization complete. Showing sync status/clock on GC9A01 LCD.");
}