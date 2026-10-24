#include "chat_state.h"
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

static TaskHandle_t s_preview_task_handle = NULL;
static volatile bool s_camera_preview_active = false;

static void camera_preview_task(void *pvParameters) {
    ESP_LOGW(TAG, "==========================================================");
    ESP_LOGW(TAG, ">>> CAMERA PREVIEW TASK STARTED (240x240 RGB565 0-LAG) <<<");
    ESP_LOGW(TAG, "==========================================================");

    display_ui_start_camera_preview();

    uint16_t *crop_buf = display_ui_get_preview_buffer();
    if (crop_buf == NULL) {
        ESP_LOGE(TAG, "Failed to get crop_buf for preview!");
        display_ui_stop_camera_preview();
        s_camera_preview_active = false;
        s_preview_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 1. 첫 프레임 촬영 -> 90도 회전 적용 후 즉시 LCD 표시 및 서버로 단 1번 JPEG 압축 전송
    camera_fb_t *first_fb = camera_io_get_fb(pdMS_TO_TICKS(2000));
    if (first_fb != NULL) {
        if (first_fb->format == PIXFORMAT_RGB565 && first_fb->width == 240 && first_fb->height == 240) {
            const uint16_t *src = (const uint16_t *)first_fb->buf;

            // 바이트 스왑 없이 원본 RGB565 그대로 시계방향 90도 회전 (색상 왜곡 완전 해소!)
            for (int y = 0; y < 240; y++) {
                int src_x = 239 - y;
                uint16_t *dst_row = crop_buf + (y * 240);
                for (int x = 0; x < 240; x++) {
                    int src_y = x;
                    dst_row[x] = src[src_y * 240 + src_x];
                }
            }
            display_ui_draw_preview_frame(crop_buf);

            // 젯슨 서버로 시계방향 90도 회전된 정상 각도의 JPEG 전송 (crop_buf 직결 인코딩)
            uint8_t *jpg_buf = NULL;
            size_t jpg_len = 0;
            if (fmt2jpg((uint8_t *)crop_buf, 240 * 240 * 2, 240, 240, PIXFORMAT_RGB565, 80, &jpg_buf, &jpg_len)) {
                ESP_LOGI(TAG, "First frame rotated 90 deg CW & encoded to JPEG (%u bytes). Sending to server...", (unsigned int)jpg_len);
                char meta[96];
                snprintf(meta, sizeof(meta), "{\"type\":\"image_start\",\"size\":%u}", (unsigned int)jpg_len);
                wifi_ws_send_text(meta);
                wifi_ws_send_bin(jpg_buf, jpg_len);
                free(jpg_buf);
            } else {
                ESP_LOGE(TAG, "Failed to encode first frame to JPEG");
                wifi_ws_send_text("{\"type\":\"image_failed\"}");
            }
        }
        camera_io_return_fb(first_fb);
    } else {
        ESP_LOGW(TAG, "Failed to capture first frame for server");
        wifi_ws_send_text("{\"type\":\"image_failed\"}");
    }

    // 2. AI 답변이 끝날 때까지 실시간 카메라 프리뷰 루프 실행 (디코딩 0초, 30+ FPS 버벅임 없는 스트리밍)
    uint32_t frame_count = 0;
    while (s_camera_preview_active) {
        camera_fb_t *fb = camera_io_get_fb(pdMS_TO_TICKS(100));
        if (fb != NULL) {
            if (s_camera_preview_active && fb->format == PIXFORMAT_RGB565 && fb->width == 240 && fb->height == 240) {
                const uint16_t *src = (const uint16_t *)fb->buf;
                // [16x16 블록 타일링 시계방향 90도 회전] 바이트 스왑 없이 원본 데이터 직결로 0.8ms 완료!
                for (int by = 0; by < 240; by += 16) {
                    for (int bx = 0; bx < 240; bx += 16) {
                        for (int y = by; y < by + 16; y++) {
                            int src_x = 239 - y;
                            uint16_t *dst_row = crop_buf + (y * 240);
                            for (int x = bx; x < bx + 16; x++) {
                                int src_y = x;
                                dst_row[x] = src[src_y * 240 + src_x];
                            }
                        }
                    }
                }
                if (s_camera_preview_active) {
                    display_ui_draw_preview_frame(crop_buf);
                    frame_count++;
                    if (frame_count % 30 == 0) {
                        ESP_LOGI(TAG, "Camera live preview: %u frames rendered (smooth stream)", (unsigned int)frame_count);
                    }
                }
            }
            camera_io_return_fb(fb);
        }
        vTaskDelay(1); // 1 tick 최소 지연으로 다음 센서 프레임 즉각 수신
    }

    ESP_LOGI(TAG, "Exiting camera preview loop (total %u frames rendered), restoring LVGL UI", (unsigned int)frame_count);
    display_ui_stop_camera_preview();

    switch (s_chat_state) {
        case CHAT_STATE_SPEAKING:
            display_ui_set_emotion(UI_EMOTION_SPEAKING, UI_EMOTION_SPEAKING, "SPEAKING...");
            break;
        case CHAT_STATE_THINKING:
            display_ui_set_emotion(UI_EMOTION_SLEEPY, UI_EMOTION_SLEEPY, "THINKING...");
            break;
        case CHAT_STATE_LISTENING:
            display_ui_set_emotion(UI_EMOTION_LISTENING, UI_EMOTION_LISTENING, "LISTENING...");
            break;
        case CHAT_STATE_IDLE:
        default:
            display_ui_set_emotion(UI_EMOTION_IDLE, UI_EMOTION_IDLE, "SAY 'ALEXA' OR TOUCH");
            break;
    }

    s_preview_task_handle = NULL;
    vTaskDelete(NULL);
}


void chat_state_start_camera_preview(void) {
    if (s_camera_preview_active && s_preview_task_handle != NULL) {
        return;
    }
    s_camera_preview_active = true;
    BaseType_t res = xTaskCreatePinnedToCoreWithCaps(camera_preview_task, "cam_prev", 8192, NULL, 3,
                                    &s_preview_task_handle, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera preview task! res=%d", (int)res);
        s_camera_preview_active = false;
        s_preview_task_handle = NULL;
    }
}

void chat_state_stop_camera_preview(void) {
    if (s_camera_preview_active) {
        ESP_LOGI(TAG, "Requesting camera preview to stop");
        s_camera_preview_active = false;
    }
    // 프리뷰 활성 여부와 상관없이 LCD를 눈 모양으로 즉시 복원
    display_ui_stop_camera_preview();
}

void chat_state_init(void) {
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    s_chat_state = CHAT_STATE_IDLE;
    s_session_active = false;
    s_close_session_after_speaking = false;
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_state_change_ms = s_last_activity_ms;

    display_ui_pause_auto_blink(false);
    display_ui_set_status("SAY 'ALEXA' OR TOUCH");
    ESP_LOGI(TAG, "Chat State Machine initialized (Current: IDLE)");
}

chat_state_t chat_state_get(void) {
    return s_chat_state;
}

void chat_state_set(chat_state_t new_state) {
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }

    if (s_chat_state == new_state) {
        if (new_state == CHAT_STATE_IDLE) {
            display_ui_set_status("SAY 'ALEXA' OR TOUCH");
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
            display_ui_set_emotion(UI_EMOTION_IDLE, UI_EMOTION_IDLE, "SAY 'ALEXA' OR TOUCH");
            break;

        case CHAT_STATE_LISTENING:
            chat_state_stop_camera_preview();
            s_session_active = true;
            s_listening_start_ms = now_ms;
            audio_pipeline_enable_mic_streaming(true);
            display_ui_pause_auto_blink(true);
            display_ui_set_emotion(UI_EMOTION_LISTENING, UI_EMOTION_LISTENING, "LISTENING...");
            break;

        case CHAT_STATE_THINKING:
            audio_pipeline_enable_mic_streaming(false);
            display_ui_pause_auto_blink(true);
            display_ui_set_emotion(UI_EMOTION_SLEEPY, UI_EMOTION_SLEEPY, "THINKING...");
            break;

        case CHAT_STATE_SPEAKING:
            audio_pipeline_enable_mic_streaming(false);
            display_ui_pause_auto_blink(true);
            display_ui_set_emotion(UI_EMOTION_SPEAKING, UI_EMOTION_SPEAKING, "SPEAKING...");
            break;
    }

    if (s_state_mutex != NULL) {
        xSemaphoreGive(s_state_mutex);
    }
}

void chat_state_barge_in(void) {
    ESP_LOGW(TAG, ">>> BARGE-IN TRIGGERED! Flushing audio & switching to LISTENING...");
    chat_state_stop_camera_preview();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_barge_in_ms = now_ms;
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
        display_ui_set_status("CONNECTING...");
        return;
    }

    ESP_LOGI(TAG, ">>> Wake word 'Alexa' DETECTED! Current state: %d", (int)s_chat_state);
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

    ESP_LOGI(TAG, "Server text received: %s", text);
    bool handled = false;

    // 1. JSON 제어 메시지 파싱
    if (text[0] == '{') {
        cJSON *root = cJSON_Parse(text);
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
                        chat_state_stop_camera_preview();
                        cJSON *close_item = cJSON_GetObjectItem(root, "close_session");
                        if (cJSON_IsBool(close_item) && cJSON_IsTrue(close_item)) {
                            ESP_LOGI(TAG, "Server requested session close (close_session: true)");
                            s_close_session_after_speaking = true;
                        }

                        if (audio_pipeline_is_spk_buffer_empty() && !audio_pipeline_is_speaker_playing()) {
                            // 오디오 응답 없이 바로 DONE이 온 경우(예: 무음/인식 실패 등)
                            if (s_close_session_after_speaking) {
                                ESP_LOGI(TAG, "Touch manual speech_end completed with no audio. Ending session -> IDLE");
                                s_close_session_after_speaking = false;
                                chat_state_set(CHAT_STATE_IDLE);
                            } else if (s_session_active) {
                                // 사용자가 Alexa/터치로 시작한 대화는 60초 동안 음성이 없을 때만 종료되므로 계속 LISTENING 유지!
                                chat_state_set(CHAT_STATE_LISTENING);
                            } else {
                                chat_state_set(CHAT_STATE_IDLE);
                            }
                        } else {
                            audio_pipeline_set_server_done(true);
                        }
                    } else if (strcmp(message->valuestring, "THINKING...") == 0 || strcmp(message->valuestring, "THINKING") == 0) {
                        audio_pipeline_set_discard_tts(false);
                        if (s_chat_state != CHAT_STATE_THINKING) {
                            ESP_LOGI(TAG, "Server status THINKING received -> Switching state to THINKING");
                            chat_state_set(CHAT_STATE_THINKING);
                        }
                    } else if (strcmp(message->valuestring, "SPEAKING...") == 0 || strcmp(message->valuestring, "SPEAKING") == 0) {
                        audio_pipeline_set_discard_tts(false);
                        if (s_chat_state != CHAT_STATE_SPEAKING) {
                            ESP_LOGI(TAG, "Server status SPEAKING received -> Switching state to SPEAKING");
                            chat_state_set(CHAT_STATE_SPEAKING);
                        }
                    } else {
                        display_ui_set_status(message->valuestring);
                    }
                    handled = true;
                } else if (strcmp(type->valuestring, "capture") == 0) {
                    ESP_LOGW(TAG, "==========================================================");
                    ESP_LOGW(TAG, ">>> RECEIVED CAPTURE COMMAND FROM JETSON SERVER! <<<");
                    ESP_LOGW(TAG, "==========================================================");
                    chat_state_start_camera_preview();
                    handled = true;
                } else if (strcmp(type->valuestring, "preview_stop") == 0) {
                    ESP_LOGI(TAG, "Received preview_stop command from server");
                    chat_state_stop_camera_preview();
                    handled = true;
                }
            }
            cJSON_Delete(root);
        }
    }

    // 2. 일반 문자열 처리
    if (!handled) {
        if (strcmp(text, "DONE") == 0 || strcmp(text, "END") == 0) {
            chat_state_stop_camera_preview();
            if (audio_pipeline_is_spk_buffer_empty() && !audio_pipeline_is_speaker_playing()) {
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
        } else if (strcmp(text, "CLEAR_AUDIO") == 0) {
            chat_state_stop_camera_preview();
            audio_pipeline_clear_audio();
            if (s_chat_state == CHAT_STATE_SPEAKING || s_chat_state == CHAT_STATE_THINKING) {
                if (s_session_active) {
                    chat_state_set(CHAT_STATE_LISTENING);
                } else {
                    chat_state_set(CHAT_STATE_IDLE);
                }
            }
        } else if (strncmp(text, "STATUS:", 7) == 0) {
            const char *st = text + 7;
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
            } else {
                display_ui_set_status(st);
            }
        }
    }
}

void chat_state_activity_occurred(void) {
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
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
        // 서버 응답 대기 타임아웃 (30초)
        if (elapsed >= 30000) {
            ESP_LOGW(TAG, ">>> 30 seconds of THINKING timeout! Returning to IDLE or LISTENING...");
            if (s_close_session_after_speaking) {
                s_close_session_after_speaking = false;
                chat_state_set(CHAT_STATE_IDLE);
            } else if (s_session_active) {
                chat_state_set(CHAT_STATE_LISTENING);
            } else {
                chat_state_set(CHAT_STATE_IDLE);
            }
        }
    }
}
