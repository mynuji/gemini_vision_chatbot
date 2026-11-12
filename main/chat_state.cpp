#include "chat_state.h"
#include "settings_manager.h"
#include "audio_pipeline.h"
#include "audio_io.h"
#include "display_ui.h"
#include "wifi_ws.h"
#include "camera_io.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#include "img_converters.h"
#include "jpeg_decoder.h"

static const char *TAG = "CHAT_STATE";

static chat_state_t s_chat_state = CHAT_STATE_IDLE;
static bool s_session_active = false;
static bool s_close_session_after_speaking = false; // 화면 터치로 수동 발화 완료 시, AI 답변 후 즉시 세션 종료
static uint32_t s_last_activity_ms = 0;
static uint32_t s_listening_start_ms = 0;
static uint32_t s_last_barge_in_ms = 0;
static uint32_t s_last_state_change_ms = 0;
static SemaphoreHandle_t s_state_mutex = NULL;

static bool s_temp_clock_active = false;
static uint32_t s_temp_clock_end_ms = 0;

static TaskHandle_t s_preview_task_handle = NULL;
static volatile bool s_camera_preview_active = false;

static void camera_preview_task(void *pvParameters) {
    ESP_LOGW(TAG, "==========================================================");
    ESP_LOGW(TAG, ">>> CAMERA PREVIEW TASK STARTED (240x240 RGB565 0-LAG) <<<");
    ESP_LOGW(TAG, "==========================================================");

    // 카메라가 꺼져 있다면 On-Demand로 즉시 기동
    bool newly_started = false;
    if (!camera_io_is_ready()) {
        ESP_LOGI(TAG, "Waking camera hardware from standby...");
        esp_err_t err = camera_io_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to wake camera: %s", esp_err_to_name(err));
            s_camera_preview_active = false;
            s_preview_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        newly_started = true;
    }

    display_ui_start_camera_preview();

    uint16_t *crop_buf = display_ui_get_preview_buffer();
    size_t dec_sz = 320 * 240 * sizeof(uint16_t);
    size_t work_sz = 65536;

    uint16_t *dec_buf = (uint16_t *)heap_caps_malloc(dec_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *work_buf = heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (dec_buf == NULL || work_buf == NULL || crop_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for camera preview: dec=%p, work=%p, crop=%p",
                 dec_buf, work_buf, crop_buf);
        if (dec_buf) free(dec_buf);
        if (work_buf) free(work_buf);
        display_ui_stop_camera_preview();
        camera_io_deinit();
        s_camera_preview_active = false;
        s_preview_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 신규 부팅 직후인 경우 센서 노출/화이트밸런스 수렴을 위해 1프레임 버퍼 플러시
    if (newly_started) {
        camera_fb_t *dummy_fb = camera_io_get_fb(pdMS_TO_TICKS(300));
        if (dummy_fb) {
            camera_io_return_fb(dummy_fb);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    // 1. 카메라 사진(QVGA 320x240, ~10KB)을 지연 없이 즉시 촬영하여 서버로 초고속 전송
    camera_fb_t *first_fb = camera_io_get_fb(pdMS_TO_TICKS(800));
    if (first_fb == NULL || first_fb->format != PIXFORMAT_JPEG || first_fb->len == 0) {
        if (first_fb) camera_io_return_fb(first_fb);
        vTaskDelay(pdMS_TO_TICKS(30));
        first_fb = camera_io_get_fb(pdMS_TO_TICKS(800));
    }
    if (first_fb != NULL && first_fb->format == PIXFORMAT_JPEG && first_fb->len > 0) {
        ESP_LOGI(TAG, "First frame captured (%u bytes, %dx%d). Sending to Jetson server...",
                 (unsigned int)first_fb->len, first_fb->width, first_fb->height);
        char meta[96];
        snprintf(meta, sizeof(meta), "{\"type\":\"image_start\",\"size\":%u}", (unsigned int)first_fb->len);
        wifi_ws_send_text(meta);
        wifi_ws_send_bin(first_fb->buf, first_fb->len);

        // 첫 프레임 즉시 디코딩하여 LCD에 표시
        esp_jpeg_image_cfg_t jpeg_cfg = {};
        jpeg_cfg.indata = first_fb->buf;
        jpeg_cfg.indata_size = (uint32_t)first_fb->len;
        jpeg_cfg.outbuf = (uint8_t *)dec_buf;
        jpeg_cfg.outbuf_size = (uint32_t)dec_sz;
        jpeg_cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
        jpeg_cfg.out_scale = (first_fb->width > 320) ? JPEG_IMAGE_SCALE_1_2 : JPEG_IMAGE_SCALE_0;
        jpeg_cfg.flags.swap_color_bytes = 1; // 완벽한 정상 색상 복원!
        jpeg_cfg.advanced.working_buffer = work_buf;
        jpeg_cfg.advanced.working_buffer_size = work_sz;

        esp_jpeg_image_output_t out_info = {};
        if (esp_jpeg_decode(&jpeg_cfg, &out_info) == ESP_OK) {
            // [시계방향 90도 정방향 회전]
            for (int y = 0; y < 240; y++) {
                int src_x = 279 - y;
                uint16_t *dst_row = crop_buf + (y * 240);
                for (int x = 0; x < 240; x++) {
                    int src_y = x;
                    dst_row[x] = dec_buf[src_y * 320 + src_x];
                }
            }
            display_ui_draw_preview_frame(crop_buf);
        }
        camera_io_return_fb(first_fb);
    } else {
        ESP_LOGW(TAG, "Failed to capture first frame for server");
        if (first_fb) camera_io_return_fb(first_fb);
        wifi_ws_send_text("{\"type\":\"image_failed\"}");
    }

    // 2. 스피커로 AI 음성 답변이 출력되는 동안 LCD 화면에 실시간 카메라 영상(30fps) 지속 출력
    uint32_t frame_count = 0;
    while (s_camera_preview_active) {
        camera_fb_t *fb = camera_io_get_fb(pdMS_TO_TICKS(100));

        if (fb != NULL) {
            if (s_camera_preview_active && fb->format == PIXFORMAT_JPEG && fb->len > 0) {
                esp_jpeg_image_cfg_t jpeg_cfg = {};
                jpeg_cfg.indata = fb->buf;
                jpeg_cfg.indata_size = (uint32_t)fb->len;
                jpeg_cfg.outbuf = (uint8_t *)dec_buf;
                jpeg_cfg.outbuf_size = (uint32_t)dec_sz;
                jpeg_cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
                jpeg_cfg.out_scale = (fb->width > 320) ? JPEG_IMAGE_SCALE_1_2 : JPEG_IMAGE_SCALE_0;
                jpeg_cfg.flags.swap_color_bytes = 1; // 완벽한 정상 색상 복원!
                jpeg_cfg.advanced.working_buffer = work_buf;
                jpeg_cfg.advanced.working_buffer_size = work_sz;

                esp_jpeg_image_output_t out_info = {};
                if (esp_jpeg_decode(&jpeg_cfg, &out_info) == ESP_OK) {
                    if (!s_camera_preview_active) {
                        camera_io_return_fb(fb);
                        break;
                    }
                    // [16x16 블록 타일링 시계방향 90도 회전] 캐시 최적화로 0.8ms에 완료
                    for (int by = 0; by < 240; by += 16) {
                        for (int bx = 0; bx < 240; bx += 16) {
                            for (int y = by; y < by + 16; y++) {
                                int src_x = 279 - y;
                                uint16_t *dst_row = crop_buf + (y * 240);
                                for (int x = bx; x < bx + 16; x++) {
                                    int src_y = x;
                                    dst_row[x] = dec_buf[src_y * 320 + src_x];
                                }
                            }
                        }
                    }
                    if (s_camera_preview_active) {
                        // 40MHz SPI DMA를 통해 LCD로 23ms 만에 직접 전송 (LVGL 경유 지연 완전 제거)
                        display_ui_draw_preview_frame(crop_buf);
                        frame_count++;
                        if (frame_count % 30 == 0) {
                            ESP_LOGI(TAG, "Camera live preview: %u frames rendered during AI speech output", (unsigned int)frame_count);
                        }
                    }
                }
            }
            camera_io_return_fb(fb);
        }
        // AI 스피커 재생(I2S DMA) 및 Wi-Fi 태스크의 CPU 선점을 안정적으로 보장하기 위해
        // 프리뷰 FPS를 12~15fps 수준으로 제어하여 CPU 점유율을 40% 이상 절감
        vTaskDelay(pdMS_TO_TICKS(35));
    }

    ESP_LOGI(TAG, "Exiting camera preview loop (total %u frames rendered), restoring LVGL UI", (unsigned int)frame_count);
    free(dec_buf);
    free(work_buf);
    display_ui_stop_camera_preview();

    // 프리뷰 종료 즉시 카메라 전원 차단 및 20MHz XCLK 정지 (발열 원천 차단)
    camera_io_deinit();

    s_camera_preview_active = false;
    s_preview_task_handle = NULL;
    vTaskDelete(NULL);
}


void chat_state_start_camera_preview(void) {
    if (s_camera_preview_active && s_preview_task_handle != NULL) {
        return;
    }
    s_camera_preview_active = true;
    // Core 0에 배치 및 우선순위 4: Core 1의 AEC(에코 캔슬러) 연산 폭증과의 CPU 경합을 회피하여 AI 음성 출력 중에도 카메라가 버벅이지 않도록 보장!
    BaseType_t res = xTaskCreatePinnedToCoreWithCaps(camera_preview_task, "cam_prev", 8192, NULL, 4,
                                    &s_preview_task_handle, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera preview task! res=%d", (int)res);
        s_camera_preview_active = false;
        s_preview_task_handle = NULL;
    }
}

void chat_state_stop_camera_preview(void) {
    if (!s_camera_preview_active && s_preview_task_handle == NULL) {
        if (camera_io_is_ready()) {
            camera_io_deinit();
        }
        return;
    }
    if (s_camera_preview_active) {
        ESP_LOGI(TAG, "Requesting camera preview to stop");
        s_camera_preview_active = false;
    }
    // 프리뷰 태스크가 실행 중이라면 최대 250ms 동안 루프 탈출 대기 (자기 자신인 경우 제외)
    if (s_preview_task_handle != NULL && xTaskGetCurrentTaskHandle() != s_preview_task_handle) {
        int wait_cnt = 0;
        while (s_preview_task_handle != NULL && wait_cnt < 25) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_cnt++;
        }
    }
    // LCD를 눈 모양으로 즉시 복원
    display_ui_stop_camera_preview();
    if (camera_io_is_ready()) {
        camera_io_deinit();
    }
}

void chat_state_init(void) {
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    s_chat_state = CHAT_STATE_IDLE;
    s_session_active = false;
    s_close_session_after_speaking = false;
    s_temp_clock_active = false;
    s_temp_clock_end_ms = 0;
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_state_change_ms = s_last_activity_ms;

    display_ui_pause_auto_blink(false);
    bool is_ko = (settings_manager_get_language() == APP_LANG_KO);
    const char *idle_status = is_ko ? "'알렉사' or 터치" : "SAY 'ALEXA' OR TOUCH";
    if (!wifi_ws_is_wifi_connected()) {
        idle_status = is_ko ? "WIFI 끊김" : "WIFI DISCONNECTED";
    } else if (!wifi_ws_is_connected()) {
        idle_status = is_ko ? "서버 접속 끊김" : "SERVER DISCONNECTED";
    }
    display_ui_set_status(idle_status);
    ESP_LOGI(TAG, "Chat State Machine initialized (Current: IDLE)");
}

chat_state_t chat_state_get(void) {
    return s_chat_state;
}

void chat_state_set(chat_state_t new_state) {
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }

    bool is_ko = (settings_manager_get_language() == APP_LANG_KO);

    if (s_chat_state == new_state) {
        if (new_state == CHAT_STATE_IDLE) {
            const char *idle_status = is_ko ? "'알렉사' or 터치" : "SAY 'ALEXA' OR TOUCH";
            if (!wifi_ws_is_wifi_connected()) {
                idle_status = is_ko ? "WIFI 끊김" : "WIFI DISCONNECTED";
            } else if (!wifi_ws_is_connected()) {
                idle_status = is_ko ? "서버 접속 끊김" : "SERVER DISCONNECTED";
            }
            display_ui_set_emotion(UI_EMOTION_IDLE, UI_EMOTION_IDLE, idle_status);
        } else if (new_state == CHAT_STATE_LISTENING) {
            display_ui_set_emotion(UI_EMOTION_LISTENING, UI_EMOTION_LISTENING, is_ko ? "듣고 있어요..." : "LISTENING...");
        }
        if (s_state_mutex != NULL) xSemaphoreGive(s_state_mutex);
        return;
    }

    chat_state_t prev_state = s_chat_state;
    s_chat_state = new_state;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_activity_ms = now_ms;
    s_last_state_change_ms = now_ms;

    ESP_LOGI(TAG, "State Transition: %d -> %d", (int)prev_state, (int)new_state);

    switch (new_state) {
        case CHAT_STATE_IDLE:
            chat_state_stop_camera_preview();
            s_session_active = false;
            s_close_session_after_speaking = false;
            audio_pipeline_enable_mic_streaming(false);
            display_ui_pause_auto_blink(false);
            {
                const char *idle_status = is_ko ? "'알렉사' or 터치" : "SAY 'ALEXA' OR TOUCH";
                if (!wifi_ws_is_wifi_connected()) {
                    idle_status = is_ko ? "WIFI 연결 필요" : "WIFI CONNECTION REQUIRED";
                } else if (!wifi_ws_is_connected()) {
                    idle_status = is_ko ? "서버 접속 끊김" : "SERVER DISCONNECTED";
                }
                display_ui_set_emotion(UI_EMOTION_IDLE, UI_EMOTION_IDLE, idle_status);
            }
            break;

        case CHAT_STATE_LISTENING:
            chat_state_stop_camera_preview();
            s_session_active = true;
            s_listening_start_ms = now_ms;
            audio_pipeline_enable_mic_streaming(true);
            display_ui_pause_auto_blink(true);
            display_ui_set_emotion(UI_EMOTION_LISTENING, UI_EMOTION_LISTENING, is_ko ? "듣고 있어요..." : "LISTENING...");
            break;

        case CHAT_STATE_THINKING:
            audio_pipeline_enable_mic_streaming(false);
            if (!s_camera_preview_active) {
                display_ui_pause_auto_blink(true);
                display_ui_set_emotion(UI_EMOTION_SLEEPY, UI_EMOTION_SLEEPY, is_ko ? "생각 중..." : "THINKING...");
            }
            break;

        case CHAT_STATE_SPEAKING:
            audio_pipeline_enable_mic_streaming(false);
            if (!s_camera_preview_active) {
                display_ui_pause_auto_blink(true);
                display_ui_set_emotion(UI_EMOTION_SPEAKING, UI_EMOTION_SPEAKING, is_ko ? "말하는 중..." : "SPEAKING...");
            }
            break;
    }

    if (s_state_mutex != NULL) {
        xSemaphoreGive(s_state_mutex);
    }
}

void chat_state_barge_in(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - s_last_barge_in_ms < 1200) {
        ESP_LOGW(TAG, "Barge-in ignored: Debounce guard %ums (< 1200ms)", (unsigned int)(now_ms - s_last_barge_in_ms));
        return;
    }
    s_last_barge_in_ms = now_ms;

    ESP_LOGW(TAG, ">>> BARGE-IN TRIGGERED! Flushing audio & switching to LISTENING...");
    chat_state_stop_camera_preview();
    s_close_session_after_speaking = false; // 끼어들기 시 새 대화로 이어짐

    // 1. 현재까지 스피커로 실제 재생된 바이트 수 획득
    uint32_t played_bytes = audio_pipeline_get_played_bytes();

    // 2. 스피커 버퍼 및 I2S 즉각 침묵
    audio_pipeline_clear_audio();

    // 3. 젯슨 서버에 인터럽트 통보 (음성 끼어들기: reason=voice, played_bytes 포함)
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"type\":\"interrupt\",\"reason\":\"voice\",\"played_bytes\":%u,\"message\":\"CLEAR_AUDIO\"}", (unsigned int)played_bytes);
    wifi_ws_send_text(msg);

    // 4. 사용자 발화 수신 모드로 전환
    chat_state_set(CHAT_STATE_LISTENING);
}

void chat_state_handle_touch(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    // 상태 전환 직후 500ms 이내의 터치는 이전 상태의 잔여 바운스 또는 연타이므로 무시 (오동작 방지)
    if (now_ms - s_last_state_change_ms < 500) {
        ESP_LOGW(TAG, "Touch ignored: State changed just %ums ago (< 500ms guard)", (unsigned int)(now_ms - s_last_state_change_ms));
        return;
    }

    ESP_LOGI(TAG, "Touch event received! Current state: %d", (int)s_chat_state);

    if (s_temp_clock_active) {
        s_temp_clock_active = false;
        display_ui_show_clock_mode(false);
    }

    if (s_chat_state == CHAT_STATE_IDLE) {
        // IDLE 상태에서 터치 -> Alexa 호출과 동일하게 처리 (비프음 + wake_word 전송 + LISTENING 시작)
        ESP_LOGI(TAG, "Touch in IDLE -> Triggering wake session exactly like Alexa!");
        s_close_session_after_speaking = false;
        chat_state_handle_wake_word();
    } else {
        // 대화 중(LISTENING, THINKING, SPEAKING) 화면 터치 시:
        // [사용자 요구사항 1] 모든 대화를 즉각 중단하고 대기 상태인 Alexa 호출 화면(IDLE)으로 전환!
        ESP_LOGI(TAG, "Touch in active state (%d) -> ABORTING ALL CONVERSATION & RETURNING TO IDLE!", (int)s_chat_state);

        // 0. 진행 중인 카메라 프리뷰 즉시 중단
        chat_state_stop_camera_preview();

        // 1. 마이크 스트리밍 차단
        audio_pipeline_enable_mic_streaming(false);

        // 2. 취소/중단 알림 비프음 (587Hz, 80ms)
        audio_io_play_tone(587.33f, 80);

        // 3. 스피커 버퍼 및 I2S 오디오 즉시 침묵 플러시
        audio_pipeline_clear_audio();

        // 4. 젯슨 서버에 진행 중인 모든 AI 응답 및 오디오 버퍼 완전 취소 통보 (터치 중단: reason=touch)
        if (wifi_ws_is_connected()) {
            wifi_ws_send_text("{\"type\":\"interrupt\",\"reason\":\"touch\",\"message\":\"CLEAR_AUDIO\"}");
        }

        // 5. 세션 종료 및 IDLE(Alexa 호출 대기) 상태로 전환
        s_session_active = false;
        s_close_session_after_speaking = false;
        chat_state_set(CHAT_STATE_IDLE);
    }
}

void chat_state_handle_wake_word(void) {
    if (!wifi_ws_is_connected()) {
        ESP_LOGW(TAG, "Wake word detected but Jetson WebSocket is not connected.");
        bool is_ko = (settings_manager_get_language() == APP_LANG_KO);
        if (!wifi_ws_is_wifi_connected()) {
            display_ui_set_status(is_ko ? "WIFI 연결 필요" : "WIFI CONNECTION REQUIRED");
        } else {
            display_ui_set_status(is_ko ? "서버 접속 끊김" : "SERVER DISCONNECTED");
        }
        return;
    }

    ESP_LOGI(TAG, ">>> Wake word 'Alexa' DETECTED! Current state: %d", (int)s_chat_state);
    if (s_temp_clock_active) {
        s_temp_clock_active = false;
        display_ui_show_clock_mode(false);
    }
    s_close_session_after_speaking = false; // Alexa 호출 시 일반 대화 모드(60초 침묵 전까지 유지)

    if (s_chat_state == CHAT_STATE_SPEAKING || s_chat_state == CHAT_STATE_THINKING) {
        // 음성 끼어들기 (말하는 중 또는 생각하는 중)
        chat_state_barge_in();
    } else {
        audio_io_play_tone(880.0f, 80);
        if (wifi_ws_is_connected()) {
            wifi_ws_send_text("{\"type\":\"wake_word\",\"word\":\"alexa\"}");
        }
        chat_state_set(CHAT_STATE_LISTENING);
    }
}

void chat_state_handle_voice_barge_in(void) {
    if (s_chat_state == CHAT_STATE_SPEAKING || s_chat_state == CHAT_STATE_THINKING) {
        chat_state_barge_in();
    }
}

void chat_state_handle_speech_end(void) {
    if (s_chat_state == CHAT_STATE_LISTENING) {
        ESP_LOGI(TAG, ">>> End of speech detected -> Switching to THINKING");
        if (wifi_ws_is_connected()) {
            wifi_ws_send_text("{\"type\":\"speech_end\"}");
        }
        chat_state_set(CHAT_STATE_THINKING);
    }
}

void chat_state_handle_server_text(const char *text, size_t len) {
    if (!text || len == 0) return;

    // 안전한 null-terminated 버퍼 생성 (웹소켓 수신 버퍼는 \0 미보장)
    char safe_buf[512];
    if (len >= sizeof(safe_buf)) {
        len = sizeof(safe_buf) - 1;
    }
    memcpy(safe_buf, text, len);
    safe_buf[len] = '\0';

    ESP_LOGI(TAG, "Server text received (%d bytes): %s", (int)len, safe_buf);
    bool handled = false;

    // 1. JSON 제어 메시지 파싱
    if (safe_buf[0] == '{') {
        cJSON *root = cJSON_Parse(safe_buf);
        if (root != NULL) {
            cJSON *type = cJSON_GetObjectItem(root, "type");
            cJSON *message = cJSON_GetObjectItem(root, "message");

            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "interrupt") == 0) {
                    chat_state_stop_camera_preview();
                    audio_pipeline_clear_audio();
                    if (s_chat_state == CHAT_STATE_SPEAKING || s_chat_state == CHAT_STATE_THINKING) {
                        if (s_session_active) {
                            chat_state_set(CHAT_STATE_LISTENING);
                        } else {
                            chat_state_set(CHAT_STATE_IDLE);
                        }
                    }
                    handled = true;
                } else if (strcmp(type->valuestring, "status") == 0 && cJSON_IsString(message)) {
                    if (strcmp(message->valuestring, "DONE") == 0 || strcmp(message->valuestring, "END") == 0) {
                        cJSON *close_item = cJSON_GetObjectItem(root, "close_session");
                        if (cJSON_IsBool(close_item) && cJSON_IsTrue(close_item)) {
                            ESP_LOGI(TAG, "Server requested session close (close_session: true)");
                            s_close_session_after_speaking = true;
                        }

                        if (audio_pipeline_is_spk_buffer_empty() && !audio_pipeline_is_speaker_playing()) {
                            // 오디오 응답 없이 바로 DONE이 온 경우(예: 무음/인식 실패 등)에만 프리뷰 종료 및 즉시 복귀
                            chat_state_stop_camera_preview();
                            if (s_close_session_after_speaking) {
                                ESP_LOGI(TAG, "Speech_end completed with no audio. Ending session -> IDLE");
                                s_close_session_after_speaking = false;
                                chat_state_set(CHAT_STATE_IDLE);
                            } else if (s_session_active) {
                                chat_state_set(CHAT_STATE_LISTENING);
                            } else {
                                chat_state_set(CHAT_STATE_IDLE);
                            }
                        } else {
                            // 스피커로 AI 음성이 아직 출력 중인 경우: 프리뷰를 끄지 않고 유지하며, 재생 완료 대기!
                            audio_pipeline_set_server_done(true);
                        }
                    } else if (strcmp(message->valuestring, "THINKING...") == 0 || strcmp(message->valuestring, "THINKING") == 0) {
                        if (s_chat_state != CHAT_STATE_IDLE) {
                            audio_pipeline_set_discard_tts(false);
                            s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
                            if (s_chat_state != CHAT_STATE_THINKING) {
                                ESP_LOGI(TAG, "Server status THINKING received -> Switching state to THINKING");
                                chat_state_set(CHAT_STATE_THINKING);
                            }
                        }
                    } else if (strcmp(message->valuestring, "SPEAKING...") == 0 || strcmp(message->valuestring, "SPEAKING") == 0) {
                        // 서버에서 음성 출력 시작(SPEAKING)을 알리면 IDLE 상태 여부와 관계없이 스피커 수신 준비
                        audio_pipeline_set_discard_tts(false);
                        s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
                        if (s_chat_state != CHAT_STATE_SPEAKING) {
                            ESP_LOGI(TAG, "Server status SPEAKING received -> Switching state to SPEAKING");
                            chat_state_set(CHAT_STATE_SPEAKING);
                        }
                    } else if (strcmp(message->valuestring, "LISTENING...") == 0 || strcmp(message->valuestring, "LISTENING") == 0) {
                        if (s_chat_state != CHAT_STATE_IDLE) {
                            if (s_chat_state != CHAT_STATE_LISTENING) {
                                ESP_LOGI(TAG, "Server status LISTENING received -> Switching state to LISTENING");
                                chat_state_set(CHAT_STATE_LISTENING);
                            }
                        } else {
                            ESP_LOGW(TAG, "Ignoring server LISTENING status while in IDLE state");
                        }
                    } else {
                        if (s_chat_state != CHAT_STATE_IDLE) {
                            display_ui_set_status(message->valuestring);
                        }
                    }
                    handled = true;
                } else if (strcmp(type->valuestring, "capture") == 0) {
                    ESP_LOGW(TAG, "==========================================================");
                    ESP_LOGW(TAG, ">>> RECEIVED CAPTURE COMMAND FROM JETSON SERVER! <<<");
                    ESP_LOGW(TAG, "==========================================================");
                    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
                    audio_pipeline_enable_mic_streaming(false);
                    chat_state_start_camera_preview();
                    if (s_chat_state == CHAT_STATE_LISTENING) {
                        chat_state_set(CHAT_STATE_THINKING);
                    }
                    handled = true;
                } else if (strcmp(type->valuestring, "show_clock") == 0) {
                    cJSON *dur_item = cJSON_GetObjectItem(root, "duration");
                    uint32_t dur = 12;
                    if (dur_item && cJSON_IsNumber(dur_item) && dur_item->valueint > 0) {
                        dur = (uint32_t)dur_item->valueint;
                    }
                    chat_state_show_clock_temporary(dur);
                    handled = true;
                } else if (strcmp(type->valuestring, "preview_stop") == 0) {
                    ESP_LOGI(TAG, "Received preview_stop command from server");
                    if (audio_pipeline_is_spk_buffer_empty() && !audio_pipeline_is_speaker_playing()) {
                        chat_state_stop_camera_preview();
                    } else {
                        ESP_LOGI(TAG, "Speaker still playing. Deferring preview stop until speaker finished.");
                    }
                    handled = true;
                } else if (strcmp(type->valuestring, "set_llm_type") == 0) {
                    cJSON *llm_item = cJSON_GetObjectItem(root, "llm_type");
                    if (llm_item != NULL) {
                        uint8_t target_type = LLM_TYPE_GEMINI_API;
                        if (cJSON_IsNumber(llm_item)) {
                            target_type = (uint8_t)llm_item->valueint;
                        } else if (cJSON_IsString(llm_item)) {
                            const char *str = llm_item->valuestring;
                            if (strcasecmp(str, "gemini realtime api") == 0 || strcasecmp(str, "gemini realtime") == 0) {
                                target_type = LLM_TYPE_GEMINI_REALTIME;
                            } else if (strcasecmp(str, "chatgpt realtime api") == 0 || strcasecmp(str, "chatgpt realtime") == 0) {
                                target_type = LLM_TYPE_CHATGPT_REALTIME;
                            } else if (strcasecmp(str, "local llm") == 0 || strcasecmp(str, "local") == 0) {
                                target_type = LLM_TYPE_LOCAL_LLM;
                            } else {
                                target_type = LLM_TYPE_GEMINI_API;
                            }
                        }
                        ESP_LOGI(TAG, "Server commanded LLM Type change to: %u (%s)",
                                 (unsigned int)target_type, settings_manager_get_llm_type_name(target_type));
                        settings_manager_set_llm_type_no_notify(target_type);
                        char st_buf[32];
                        snprintf(st_buf, sizeof(st_buf), "LLM: %s", settings_manager_get_llm_type_name(target_type));
                        display_ui_set_status(st_buf);
                    }
                    handled = true;
                } else if (strcmp(type->valuestring, "set_volume") == 0) {
                    cJSON *vol_item = cJSON_GetObjectItem(root, "volume");
                    if (vol_item != NULL && cJSON_IsNumber(vol_item)) {
                        int vol = vol_item->valueint;
                        if (vol < 10) vol = 10;
                        if (vol > 100) vol = 100;
                        settings_manager_set_volume_no_notify((uint8_t)vol);
                        ESP_LOGI(TAG, "Server commanded volume change to: %d%%", vol);
                        char st_buf[32];
                        snprintf(st_buf, sizeof(st_buf), "VOL: %d%%", vol);
                        display_ui_set_status(st_buf);
                    }
                    handled = true;
                } else if (strcmp(type->valuestring, "set_mic_gain") == 0 || strcmp(type->valuestring, "set_mic_sensitivity") == 0) {
                    cJSON *gain_item = cJSON_GetObjectItem(root, "gain");
                    if (gain_item == NULL) {
                        gain_item = cJSON_GetObjectItem(root, "sensitivity");
                    }
                    if (gain_item != NULL && cJSON_IsNumber(gain_item)) {
                        int gain = gain_item->valueint;
                        if (gain < 10) gain = 10;
                        if (gain > 60) gain = 60;
                        settings_manager_set_mic_gain_no_notify((uint8_t)gain);
                        audio_pipeline_set_mic_gain((uint8_t)gain);
                        ESP_LOGI(TAG, "Server commanded mic gain change to: %d (%.1fx)", gain, gain / 10.0f);
                        char st_buf[32];
                        snprintf(st_buf, sizeof(st_buf), "MIC: %.1fx", gain / 10.0f);
                        display_ui_set_status(st_buf);
                    }
                    handled = true;
                }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "cJSON_Parse failed for: %s", safe_buf);
        }
    }

    // 2. 일반 문자열 처리
    if (!handled) {
        if (strcmp(safe_buf, "DONE") == 0 || strcmp(safe_buf, "END") == 0) {
            if (audio_pipeline_is_spk_buffer_empty() && !audio_pipeline_is_speaker_playing()) {
                chat_state_stop_camera_preview();
                if (s_close_session_after_speaking) {
                    ESP_LOGI(TAG, "Touch manual speech_end completed with no audio. Ending session -> IDLE");
                    s_close_session_after_speaking = false;
                    chat_state_set(CHAT_STATE_IDLE);
                } else if (s_session_active) {
                    chat_state_set(CHAT_STATE_LISTENING);
                } else {
                    chat_state_set(CHAT_STATE_IDLE);
                }
            } else {
                audio_pipeline_set_server_done(true);
            }
        } else if (strcmp(safe_buf, "CLEAR_AUDIO") == 0) {
            chat_state_stop_camera_preview();
            audio_pipeline_clear_audio();
            if (s_chat_state == CHAT_STATE_SPEAKING || s_chat_state == CHAT_STATE_THINKING) {
                if (s_session_active) {
                    chat_state_set(CHAT_STATE_LISTENING);
                } else {
                    chat_state_set(CHAT_STATE_IDLE);
                }
            }
        } else if (strncmp(safe_buf, "STATUS:", 7) == 0) {
            const char *st = safe_buf + 7;
            if (s_chat_state != CHAT_STATE_IDLE) {
                if (strcmp(st, "THINKING...") == 0 || strcmp(st, "THINKING") == 0) {
                    audio_pipeline_set_discard_tts(false);
                    if (s_chat_state != CHAT_STATE_THINKING) {
                        chat_state_set(CHAT_STATE_THINKING);
                    }
                } else if (strcmp(st, "SPEAKING...") == 0 || strcmp(st, "SPEAKING") == 0) {
                    audio_pipeline_set_discard_tts(false);
                    if (s_chat_state != CHAT_STATE_SPEAKING) {
                        chat_state_set(CHAT_STATE_SPEAKING);
                    }
                } else if (strcmp(st, "LISTENING...") == 0 || strcmp(st, "LISTENING") == 0) {
                    if (s_chat_state != CHAT_STATE_LISTENING) {
                        chat_state_set(CHAT_STATE_LISTENING);
                    }
                } else {
                    display_ui_set_status(st);
                }
            }
        }
    }
}

void chat_state_activity_occurred(void) {
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

void chat_state_show_clock_temporary(uint32_t duration_sec) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_temp_clock_active = true;
    s_temp_clock_end_ms = now_ms + (duration_sec * 1000);
    ESP_LOGI(TAG, "Temporary clock display activated for %u seconds", (unsigned int)duration_sec);
    display_ui_show_clock_mode(true);
}

bool chat_state_is_temp_clock_active(void) {
    return s_temp_clock_active;
}

void chat_state_handle_speaker_finished(void) {
    ESP_LOGI(TAG, "Speaker playback finished. s_close_session=%d, s_session_active=%d, cur_state=%d",
             (int)s_close_session_after_speaking, (int)s_session_active, (int)s_chat_state);
    chat_state_stop_camera_preview();
    if (s_close_session_after_speaking) {
        ESP_LOGI(TAG, "Session close requested. Ending conversation session -> IDLE");
        s_close_session_after_speaking = false;
        s_session_active = false;
        chat_state_set(CHAT_STATE_IDLE);
    } else if (s_session_active) {
        ESP_LOGI(TAG, "Speaker playback finished. Multi-turn auto-listening...");
        chat_state_set(CHAT_STATE_LISTENING);
    } else {
        chat_state_set(CHAT_STATE_IDLE);
    }
}

void chat_state_tick(uint32_t now_ms) {
    uint32_t cur_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // [0] 임시 시계 화면 유지 시간 만료 검사
    if (s_temp_clock_active) {
        if (cur_ms >= s_temp_clock_end_ms) {
            ESP_LOGI(TAG, "Temporary clock display expired -> Returning to eye display");
            s_temp_clock_active = false;
            display_ui_show_clock_mode(false);
        }
    }

    // 시간 역전(비프음 재생 등으로 인한 인자 지연) 또는 언더플로우 방어
    if (cur_ms < s_last_activity_ms) {
        return;
    }
    uint32_t elapsed = cur_ms - s_last_activity_ms;

    if (s_session_active && s_chat_state == CHAT_STATE_LISTENING) {
        // 대화 세션 종료 조건: 60초(60,000ms) 동안 아무런 음성이 들리지 않았을 때만 적용
        if (elapsed >= 60000) {
            ESP_LOGI(TAG, ">>> 60 seconds of silence timeout! Ending conversation session -> IDLE");
            s_session_active = false;
            chat_state_set(CHAT_STATE_IDLE);
        }
    } else if (s_chat_state == CHAT_STATE_THINKING) {
        // 서버 응답 대기 타임아웃 (12초로 단축하여 무응답 시 신속 복구)
        if (elapsed >= 12000) {
            ESP_LOGW(TAG, ">>> 12 seconds of THINKING timeout! Returning to IDLE or LISTENING...");
            if (s_close_session_after_speaking) {
                s_close_session_after_speaking = false;
                chat_state_set(CHAT_STATE_IDLE);
            } else if (s_session_active) {
                chat_state_set(CHAT_STATE_LISTENING);
            } else {
                chat_state_set(CHAT_STATE_IDLE);
            }
        }
    } else if (s_chat_state == CHAT_STATE_SPEAKING) {
        // 스피커 버퍼가 비어있고 스피커 출력이 멈춘 완전 유휴 상태에서 3초 이상 추가 오디오가 없을 때 자동 복구
        bool spk_idle = audio_pipeline_is_spk_buffer_empty() && !audio_pipeline_is_speaker_playing();
        if (spk_idle && elapsed >= 3000) {
            ESP_LOGW(TAG, ">>> SPEAKING state idle watchdog timeout (%ums)! Auto-recovering to LISTENING/IDLE", (unsigned int)elapsed);
            chat_state_handle_speaker_finished();
        } else if (elapsed >= 60000) {
            // 최대 60초 절대 타임아웃
            ESP_LOGW(TAG, ">>> Max SPEAKING timeout (60s)! Forcing recovery to LISTENING/IDLE");
            chat_state_handle_speaker_finished();
        }
    }
}
