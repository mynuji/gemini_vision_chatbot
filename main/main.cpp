#include "app_config.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MAIN_APP";

/**
 * @brief 네트워크(Wi-Fi / WebSocket) 상태 변경 콜백
 */
static void on_network_state_changed(wifi_ws_state_t state, const char *info) {
    ESP_LOGI(TAG, "Network State Changed: %d (%s)", state, info);

    switch (state) {
    case WIFI_WS_STATE_WIFI_CONNECTING:
        display_ui_set_status("CONNECTING WIFI...");
        break;

    case WIFI_WS_STATE_WIFI_CONNECTED:
        display_ui_set_status("WIFI OK! WS CONN...");
        break;

    case WIFI_WS_STATE_WS_CONNECTING:
        display_ui_set_status("CONNECTING JETSON...");
        break;

    case WIFI_WS_STATE_WS_CONNECTED:
        ESP_LOGI(TAG, "WebSocket Connected to Jetson Xavier! Setting state to IDLE.");
        display_ui_set_status("JETSON CONNECTED!");
        audio_io_play_tone(1046.50f, 150); // 연결 성공 맑은 비프음 (C6)
        chat_state_set(CHAT_STATE_IDLE);
        break;

    case WIFI_WS_STATE_DISCONNECTED:
    default:
        audio_pipeline_clear_audio();
        chat_state_set(CHAT_STATE_IDLE);
        display_ui_set_status(info ? info : "DISCONNECTED");
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

        // 1. 터치 감지 처리: 인터럽트 에지 이벤트 기반 1회 즉시 감지 (디바운스 600ms)
        if (has_event && (now - last_touch_handled_time > 600)) {
            ESP_LOGI(TAG, "User Touch Event Handled! Forwarding to chat_state_handle_touch()");
            chat_state_handle_touch();
            chat_state_activity_occurred();
            last_touch_handled_time = now;
        }

        // 2. 대화 상태 머신 타임아웃 틱 (최신 타임스탬프로 갱신 전달)
        chat_state_tick(now);

        // 3. 주기적 헬스 체크 로그 (3초 간격)
        if (now - last_status_log_time >= 3000) {
            chat_state_t st = chat_state_get();
            const char *st_name = (st == CHAT_STATE_IDLE) ? "IDLE" :
                                  (st == CHAT_STATE_LISTENING) ? "LISTENING" :
                                  (st == CHAT_STATE_THINKING) ? "THINKING" : "SPEAKING";
            ESP_LOGI(TAG, "Health Check: State=%s(%d), WS=%s, FreeHeap=%uKB",
                     st_name, (int)st, wifi_ws_is_connected() ? "CONNECTED" : "NO",
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

    // [1] 디스플레이 및 LVGL 초기화
    ESP_ERROR_CHECK(display_ui_init());
    display_ui_show_message("HW CHECK: LCD OK");
    vTaskDelay(pdMS_TO_TICKS(400));

    // [2] 터치 센서 초기화
    ESP_ERROR_CHECK(touch_input_init());

    // [3] 오디오 I/O (PDM RX 마이크 + I2S TX 스피커) 초기화
    display_ui_show_message("AUDIO IO CHECK...");
    ESP_ERROR_CHECK(audio_io_init());

    // 스피커 동작 확인: '도-미-솔' 부팅 멜로디
    audio_io_play_boot_melody();
    vTaskDelay(pdMS_TO_TICKS(200));

    // [4] 캐릭터 눈 표정 모드로 전환 및 자동 깜빡임 시작
    display_ui_set_emotion(UI_EMOTION_IDLE, UI_EMOTION_IDLE, "INITIALIZING...");
    display_ui_start_auto_blink();

    // [5] 대화 상태 머신 초기화
    chat_state_init();

    // [6] 네트워크 (Wi-Fi + WebSocket) 드라이버 초기화
    ESP_LOGI(TAG, "Initializing Wi-Fi & WebSocket driver...");
    display_ui_set_status("INITIALIZING WIFI...");
    wifi_ws_set_state_callback(on_network_state_changed);
    wifi_ws_set_rx_callback(on_websocket_data_received);
    ESP_ERROR_CHECK(wifi_ws_init());

    // [7] 실시간 오디오 파이프라인 (PSRAM RingBuffers, FreeRTOS Tasks, esp-sr AFE) 초기화
    display_ui_set_status("INITIALIZING AFE...");
    ESP_ERROR_CHECK(audio_pipeline_init());

    // [7-1] OV2640 카메라 초기화 (비전 질문용) - 실패해도 음성 챗봇은 계속 동작
    display_ui_set_status("INITIALIZING CAMERA...");
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

    // [9] Wi-Fi 및 WebSocket 접속 시작
    ESP_LOGI(TAG, "Starting Wi-Fi & WebSocket connection...");
    display_ui_set_status("CONNECTING WIFI...");
    ESP_ERROR_CHECK(wifi_ws_start(CONFIG_WIFI_SSID, CONFIG_WIFI_PASS, CONFIG_WS_SERVER_URI));

    ESP_LOGI(TAG, "Step 3 Initialization complete. Ready for voice interaction!");
}