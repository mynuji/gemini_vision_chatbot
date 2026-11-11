#include "audio_pipeline.h"
#include "app_config.h"
#include "settings_manager.h"
#include "chat_state.h"
#include "audio_io.h"
#include "wifi_ws.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/idf_additions.h"

// esp-sr AFE & WakeNet & AEC 헤더
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_attr.h"

static const char *TAG = "AUDIO_PIPE";

// PSRAM 기반 링버퍼 크기 정의 (내부 SRAM 0 소모)
#define SPK_RINGBUF_SIZE (256 * 1024)
#define REF_RINGBUF_SIZE (16 * 1024)
#define TX_RINGBUF_SIZE  (128 * 1024) // 1.2초 프리버퍼 일괄 투입을 수용하기 위해 128KB로 확장
#define SPK_PREBUFFER_THRESHOLD (2 * 1024) // 약 64ms 분량 (신속한 첫 음절 출력)

static RingbufHandle_t s_spk_ringbuf = NULL;
static RingbufHandle_t s_ref_ringbuf = NULL;
static RingbufHandle_t s_tx_ringbuf  = NULL;

static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;

static volatile bool s_is_speaker_playing = false;
static volatile bool s_server_done = false;
static volatile bool s_mic_streaming_enabled = false;
static volatile bool s_pipeline_initialized = false;
static volatile size_t s_spk_buffered_bytes = 0;
static volatile uint32_t s_spk_total_played_bytes = 0;
static volatile bool s_discard_tts_audio = false;
static volatile bool s_tx_flush_requested = false;
static volatile bool s_spk_flush_requested = false;
static volatile bool s_ref_flush_requested = false;

// 발화 감지(VAD) 타이밍 제어용
static volatile uint8_t s_mic_gain = 35; // 10 ~ 60 (1.0x ~ 6.0x)
static uint32_t s_mic_stream_start_ms = 0;
static uint32_t s_last_speech_ms = 0;
static bool s_has_user_spoken = false;
static int s_vad_speech_frames = 0;

// 끼어들기(Barge-in) 시 앞 음절 손실 방지용 롤링 프리버퍼 (20프레임 = 약 640ms 분량, PSRAM에 명시적 할당)
#define BARGE_IN_PRE_CHUNKS 20
#define BARGE_IN_CHUNK_BYTES 1024
static uint8_t (*s_barge_in_prebuf)[BARGE_IN_CHUNK_BYTES] = NULL;
static size_t *s_barge_in_presizes = NULL;
static int s_barge_in_prebuf_idx = 0;
static int s_barge_in_prebuf_count = 0;

// ============================================================================
// [1] 스피커 재생 태스크 (Core 0, Priority 4)
// ============================================================================
static void spk_play_task(void *pvParameters) {
    ESP_LOGI(TAG, "spk_play_task started on Core 0");

    const size_t chunk_samples = 256; // 16ms
    const size_t chunk_bytes = chunk_samples * sizeof(int16_t);
    int16_t play_buf[chunk_samples];
    bool prebuffering = true;

    int empty_spk_count = 0;
    int prebuf_wait_count = 0;

    while (1) {
        if (s_spk_ringbuf == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_spk_flush_requested || s_discard_tts_audio) {
            s_spk_flush_requested = false;
            size_t flush_size = 0;
            void *flush_item = NULL;
            while ((flush_item = xRingbufferReceiveUpTo(s_spk_ringbuf, &flush_size, 0, 4096)) != NULL) {
                vRingbufferReturnItem(s_spk_ringbuf, flush_item);
            }
            s_spk_buffered_bytes = 0;
            prebuffering = true;
            empty_spk_count = 0;
            prebuf_wait_count = 0;
        }

        // 지터 방지용 프리버퍼링 체크
        if (prebuffering) {
            size_t cur_buffered = s_spk_buffered_bytes;

            if (cur_buffered >= SPK_PREBUFFER_THRESHOLD || (s_server_done && cur_buffered > 0)) {
                prebuffering = false;
                empty_spk_count = 0;
                prebuf_wait_count = 0;
                s_is_speaker_playing = true;
                if (chat_state_get() != CHAT_STATE_SPEAKING) {
                    chat_state_set(CHAT_STATE_SPEAKING);
                }
                // I2S 스피커 앰프(MAX98357A) Mute 해제 및 워밍업 (첫 음절 잘림 방지)
                audio_io_spk_write_silence(20);
            } else if (cur_buffered > 0) {
                // 버퍼에 데이터가 있지만 임계치(2KB)에 못 미치는 경우 (짧은 음성이거나 마지막 단락)
                // 200ms(10 * 20ms) 동안 추가 유입이 없으면 대기하지 않고 즉시 재생 시작!
                prebuf_wait_count++;
                if (prebuf_wait_count >= 10) {
                    prebuffering = false;
                    prebuf_wait_count = 0;
                    empty_spk_count = 0;
                    s_is_speaker_playing = true;
                    if (chat_state_get() != CHAT_STATE_SPEAKING) {
                        chat_state_set(CHAT_STATE_SPEAKING);
                    }
                    audio_io_spk_write_silence(20);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
            } else if (s_server_done && cur_buffered == 0) {
                // 오디오 수신 없이 서버 처리가 끝난 경우(무음/인식 실패 등) 즉시 스피커 종료 핸들러 호출
                s_server_done = false;
                s_is_speaker_playing = false;
                prebuffering = true;
                empty_spk_count = 0;
                prebuf_wait_count = 0;
                chat_state_handle_speaker_finished();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            } else {
                prebuf_wait_count = 0;
                if (chat_state_get() == CHAT_STATE_SPEAKING && cur_buffered == 0) {
                    empty_spk_count++;
                    // 서버로부터 DONE이 이미 수신된 경우 신속히 완료(40ms), 아직 미수신(문장 간 합성 지연)인 경우 최대 3초(150*20ms) 대기
                    int max_empty = s_server_done ? 2 : 150;
                    if (empty_spk_count >= max_empty) {
                        empty_spk_count = 0;
                        s_is_speaker_playing = false;
                        s_server_done = false;
                        chat_state_handle_speaker_finished();
                    }
                } else {
                    empty_spk_count = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        // 링버퍼에서 PCM 데이터 인출 (최대 50ms 대기)
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_spk_ringbuf, &item_size, pdMS_TO_TICKS(50), chunk_bytes);

        if (item != NULL && item_size > 0) {
            empty_spk_count = 0;
            chat_state_activity_occurred(); // 실제 스피커 출력 재생 중 -> 타임아웃 타이머 리셋!
            memcpy(play_buf, item, item_size);
            vRingbufferReturnItem(s_spk_ringbuf, item);
            s_spk_buffered_bytes = (s_spk_buffered_bytes >= item_size) ? (s_spk_buffered_bytes - item_size) : 0;

            size_t valid_samples = item_size / sizeof(int16_t);
            float vol_scale = (float)settings_manager_get()->volume / 100.0f;
            if (vol_scale < 0.1f) vol_scale = 0.1f;
            if (vol_scale > 1.0f) vol_scale = 1.0f;

            // 볼륨 스케일을 먼저 적용하여 AEC 에코 레퍼런스와 실제 스피커 출력의 음량을 1:1로 일치시킴
            for (size_t i = 0; i < valid_samples; i++) {
                int32_t s = (int32_t)(play_buf[i] * vol_scale);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                play_buf[i] = (int16_t)s;
            }

            // AEC 에코 레퍼런스 버퍼로 전송 (스피커로 나가는 실제 신호와 정확히 일치)
            if (s_ref_ringbuf != NULL) {
                xRingbufferSend(s_ref_ringbuf, play_buf, item_size, 0);
            }

            // I2S 스피커로 출력 (이미 스케일되었으므로 볼륨 1.0f로 출력)
            audio_io_spk_play_pcm16_mono(play_buf, valid_samples, 1.0f);
            s_spk_total_played_bytes += item_size;
        } else {
            // 버퍼가 비어있음
            // 서버 완료 신호가 왔거나, 서버로부터 3초(150 * 20ms) 동안 후속 패킷이 없으면 재생 종료
            if (s_server_done || (chat_state_get() == CHAT_STATE_SPEAKING && empty_spk_count >= 150)) {
                audio_io_spk_write_silence(20);
                s_is_speaker_playing = false;
                s_server_done = false;
                prebuffering = true;
                empty_spk_count = 0;
                chat_state_handle_speaker_finished();
            } else {
                if (chat_state_get() == CHAT_STATE_SPEAKING) {
                    empty_spk_count++;
                } else {
                    empty_spk_count = 0;
                }
                prebuffering = true;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }
}

// ============================================================================
// [2] 오디오 입력 및 AFE Feed 태스크 (Core 1, Priority 5)
// ============================================================================
static void audio_feed_task(void *pvParameters) {
    ESP_LOGI(TAG, "audio_feed_task started on Core 1");

    if (s_afe_handle == NULL || s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE not ready in audio_feed_task!");
        vTaskDelete(NULL);
        return;
    }

    int feed_chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
    int feed_channels = s_afe_handle->get_feed_channel_num(s_afe_data); // 2 channels (Mic + Ref)
    size_t chunk_bytes = feed_chunksize * sizeof(int16_t);

    int16_t *mic_buf = (int16_t *)heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *ref_buf = (int16_t *)heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *feed_buf = (int16_t *)heap_caps_malloc(feed_chunksize * feed_channels * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!mic_buf || !ref_buf || !feed_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio feed buffers!");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // 1. I2S 마이크 읽기
        size_t bytes_read = 0;
        esp_err_t ret = audio_io_mic_read(mic_buf, chunk_bytes, &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read < chunk_bytes) {
            memset(mic_buf, 0, chunk_bytes);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 2. 스피커 참조 데이터(Ref) 인출
        if (s_ref_flush_requested && s_ref_ringbuf != NULL) {
            s_ref_flush_requested = false;
            size_t flush_size = 0;
            void *flush_item = NULL;
            while ((flush_item = xRingbufferReceiveUpTo(s_ref_ringbuf, &flush_size, 0, 4096)) != NULL) {
                vRingbufferReturnItem(s_ref_ringbuf, flush_item);
            }
        }
        if (s_ref_ringbuf != NULL) {
            size_t item_size = 0;
            uint8_t *ref_item = (uint8_t *)xRingbufferReceiveUpTo(s_ref_ringbuf, &item_size, 0, chunk_bytes);
            if (ref_item != NULL && item_size > 0) {
                memcpy(ref_buf, ref_item, item_size);
                if (item_size < chunk_bytes) {
                    memset(((uint8_t *)ref_buf) + item_size, 0, chunk_bytes - item_size);
                }
                vRingbufferReturnItem(s_ref_ringbuf, ref_item);
            } else {
                memset(ref_buf, 0, chunk_bytes);
            }
        } else {
            memset(ref_buf, 0, chunk_bytes);
        }

        // [마이크 감도 개선]
        // 스피커가 출력 중일 때는 자체 스피커 소리가 마이크로 피드백되어 AEC 왜곡 및 오작동이 발생하는 것을 방지하기 위해 1.0배(원음) 유지
        // 스피커가 조용할 때(대화 청취 중 등)만 작은 목소리를 잘 수음하도록 사용자 설정 디지털 게인(1.0x ~ 6.0x) 적용
        bool is_playing = s_is_speaker_playing || (s_spk_buffered_bytes > 0);
        if (!is_playing) {
            uint8_t gain = s_mic_gain;
            if (gain < 10) gain = 10;
            if (gain > 60) gain = 60;
            for (int i = 0; i < feed_chunksize; i++) {
                int32_t val = (int32_t)mic_buf[i] * (int32_t)gain / 10;
                if (val > 32767) val = 32767;
                else if (val < -32768) val = -32768;
                mic_buf[i] = (int16_t)val;
            }
        }

        // 3. Mic와 Ref 인터리빙 ([M0, R0, M1, R1, ...])
        for (int i = 0; i < feed_chunksize; i++) {
            feed_buf[2 * i]     = mic_buf[i];
            feed_buf[2 * i + 1] = ref_buf[i];
        }

        // 4. AFE로 피딩 (AEC 에코 제거 입력)
        s_afe_handle->feed(s_afe_data, feed_buf);
    }
}

// ============================================================================
// [3] AFE Fetch, WakeNet, VAD & Barge-in 분석 태스크 (Core 1, Priority 5)
// ============================================================================
static void audio_fetch_task(void *pvParameters) {
    ESP_LOGI(TAG, "audio_fetch_task started on Core 1");

    int barge_in_confirm_count = 0;

    while (1) {
        if (s_afe_handle == NULL || s_afe_data == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        chat_state_t cur_state = chat_state_get();

        // --------------------------------------------------------------------
        // [1] WakeNet ("Alexa") 호출어 감지
        // --------------------------------------------------------------------
        if (res->wakeup_state == WAKENET_DETECTED) {
            chat_state_handle_wake_word();
        }

        // --------------------------------------------------------------------
        // [2] 스피커 출력(SPEAKING) 또는 AI 생각 중(THINKING) 끼어들기 판정 및 앞 음절 버퍼링
        // --------------------------------------------------------------------
        if (cur_state == CHAT_STATE_SPEAKING || cur_state == CHAT_STATE_THINKING) {
            // 발화 이전 프레임을 롤링 버퍼에 보관
            if (s_barge_in_prebuf != NULL && s_barge_in_presizes != NULL &&
                res->data_size > 0 && res->data_size <= BARGE_IN_CHUNK_BYTES && res->data != NULL) {
                memcpy(s_barge_in_prebuf[s_barge_in_prebuf_idx], res->data, res->data_size);
                s_barge_in_presizes[s_barge_in_prebuf_idx] = res->data_size;
                s_barge_in_prebuf_idx = (s_barge_in_prebuf_idx + 1) % BARGE_IN_PRE_CHUNKS;
                if (s_barge_in_prebuf_count < BARGE_IN_PRE_CHUNKS) {
                    s_barge_in_prebuf_count++;
                }
            }

            int16_t *samples = (int16_t *)res->data;
            int num_samples = res->data_size / sizeof(int16_t);
            int32_t peak = 0;
            for (int i = 0; i < num_samples; i++) {
                int32_t val = abs(samples[i]);
                if (val > peak) peak = val;
            }

            // 끼어들기(Barge-in) 감지 정책:
            // 1) AI 응답 생성 중(THINKING): 직전 질문 발화의 잔여 음향 및 작은 주변 소음/기침 등으로 인한
            //    불필요한 자기 중단(Self-abort) 루프를 방지하기 위해 마이크 에너지 기반 끼어들기는 비활성화합니다.
            //    (생각 중 즉각 취소는 고정밀 WakeNet 신경망 모델 'Alexa' 호출어 또는 물리적 '화면 터치'로 안전하게 동작)
            // 2) AI 음성 출력 중(SPEAKING): 스피커 재생 음향의 마이크 피드백 오작동을 방지하기 위해,
            //    AEC가 적용된 WakeNet 호출어 'Alexa' 또는 화면 터치로 즉각 중단됩니다.
            bool voice_candidate = false;
            int required_confirm_frames = 2;

            if (voice_candidate) {
                barge_in_confirm_count++;
                if (barge_in_confirm_count >= required_confirm_frames) {
                    barge_in_confirm_count = 0;
                    ESP_LOGW(TAG, ">>> VOICE BARGE-IN DETECTED in %s! (peak=%ld, prebuf_count=%d)",
                             (cur_state == CHAT_STATE_SPEAKING ? "SPEAKING" : "THINKING"),
                             (long)peak, s_barge_in_prebuf_count);
                    chat_state_handle_voice_barge_in();

                    // 앞 음절 전체를 즉시 송신 링버퍼에 투입하여 앞 단어 유실 방지
                    if (s_tx_ringbuf != NULL && s_barge_in_prebuf != NULL && s_barge_in_presizes != NULL) {
                        int start_idx = (s_barge_in_prebuf_idx - s_barge_in_prebuf_count + BARGE_IN_PRE_CHUNKS) % BARGE_IN_PRE_CHUNKS;
                        for (int k = 0; k < s_barge_in_prebuf_count; k++) {
                            int idx = (start_idx + k) % BARGE_IN_PRE_CHUNKS;
                            if (s_barge_in_presizes[idx] > 0) {
                                xRingbufferSend(s_tx_ringbuf, s_barge_in_prebuf[idx], s_barge_in_presizes[idx], 0);
                            }
                        }
                    }
                    // 현재 감지된 프레임 자체도 누락 없이 송신 버퍼에 즉시 추가
                    if (s_tx_ringbuf != NULL && res->data != NULL && res->data_size > 0) {
                        xRingbufferSend(s_tx_ringbuf, res->data, res->data_size, 0);
                    }
                    s_barge_in_prebuf_count = 0;
                }
            } else {
                barge_in_confirm_count = 0;
            }
        } else {
            barge_in_confirm_count = 0;
            s_barge_in_prebuf_count = 0;
        }

        // --------------------------------------------------------------------
        // [3] 사용자 발화 청취 중 (LISTENING) 실시간 스트리밍 & VAD 감지
        // --------------------------------------------------------------------
        if (cur_state == CHAT_STATE_LISTENING && s_mic_streaming_enabled) {
            if (res->data_size > 0 && res->data != NULL && s_tx_ringbuf != NULL) {
                xRingbufferSend(s_tx_ringbuf, res->data, res->data_size, 0); // 논블로킹
            }

            // 비프음 및 단발성 주변 소음 방지: 500ms 안정화 후 3프레임(약 96ms) 연속 음성 신호 시 실제 발화로 판정
            if (now_ms - s_mic_stream_start_ms > 500) {
                if (res->vad_state == VAD_SPEECH) {
                    s_vad_speech_frames++;
                    if (s_vad_speech_frames >= 3) {
                        s_last_speech_ms = now_ms;
                        s_has_user_spoken = true;
                        chat_state_activity_occurred(); // 60초 침묵 타이머 리셋
                    }
                } else {
                    s_vad_speech_frames = 0;
                }
            }

            if (s_has_user_spoken) {
                // 발화 종료: 침묵 650ms 이상 지속 시 speech_end 전달 (단어 간 자연스러운 호흡 보호 + 신속한 턴 전환)
                bool silence_timeout = (now_ms - s_mic_stream_start_ms > 1000) && (now_ms - s_last_speech_ms > 650);
                bool max_timeout = (now_ms - s_mic_stream_start_ms > 12000);

                if (silence_timeout || max_timeout) {
                    s_has_user_spoken = false;
                    s_vad_speech_frames = 0;
                    chat_state_handle_speech_end();
                }
            }
        }
    }
}

// ============================================================================
// [4] WebSocket 바이너리 송신 태스크 (Core 0, Priority 3)
// ============================================================================
static void ws_tx_task(void *pvParameters) {
    ESP_LOGI(TAG, "ws_tx_task started on Core 0");

    while (1) {
        if (s_tx_ringbuf == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_tx_flush_requested) {
            s_tx_flush_requested = false;
            size_t flush_size = 0;
            void *flush_item = NULL;
            while ((flush_item = xRingbufferReceiveUpTo(s_tx_ringbuf, &flush_size, 0, 4096)) != NULL) {
                vRingbufferReturnItem(s_tx_ringbuf, flush_item);
            }
        }

        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_tx_ringbuf, &item_size, pdMS_TO_TICKS(50), 1024);

        if (item != NULL && item_size > 0) {
            if (wifi_ws_is_connected()) {
                wifi_ws_send_bin(item, item_size);
            }
            vRingbufferReturnItem(s_tx_ringbuf, item);
            vTaskDelay(pdMS_TO_TICKS(2)); // Wi-Fi 드라이버 버퍼 체증 방지용 미세 지연
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// ============================================================================
// [5] 공개 API 구현
// ============================================================================
esp_err_t audio_pipeline_init(void) {
    if (s_pipeline_initialized) return ESP_OK;

    const app_settings_t *cfg = settings_manager_get();
    if (cfg && cfg->mic_gain >= 10 && cfg->mic_gain <= 60) {
        s_mic_gain = cfg->mic_gain;
    }
    ESP_LOGI(TAG, "Audio Pipeline initialized with mic gain: %u (%.1fx)", s_mic_gain, s_mic_gain / 10.0f);

    ESP_LOGI(TAG, "Initializing Audio Pipeline RingBuffers in PSRAM...");

    if (s_spk_ringbuf == NULL) {
        s_spk_ringbuf = xRingbufferCreateWithCaps(SPK_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    }
    if (s_ref_ringbuf == NULL) {
        s_ref_ringbuf = xRingbufferCreateWithCaps(REF_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    }
    if (s_tx_ringbuf == NULL) {
        s_tx_ringbuf = xRingbufferCreateWithCaps(TX_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    }

    if (!s_spk_ringbuf || !s_ref_ringbuf || !s_tx_ringbuf) {
        ESP_LOGE(TAG, "Failed to allocate audio RingBuffers in PSRAM!");
        return ESP_ERR_NO_MEM;
    }

    if (s_barge_in_prebuf == NULL) {
        s_barge_in_prebuf = (uint8_t (*)[BARGE_IN_CHUNK_BYTES])heap_caps_calloc(
            BARGE_IN_PRE_CHUNKS, BARGE_IN_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_barge_in_presizes == NULL) {
        s_barge_in_presizes = (size_t *)heap_caps_calloc(
            BARGE_IN_PRE_CHUNKS, sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_barge_in_prebuf || !s_barge_in_presizes) {
        ESP_LOGE(TAG, "Failed to allocate barge-in prebuffers in PSRAM!");
        return ESP_ERR_NO_MEM;
    }

    // esp-sr AFE 초기화
    ESP_LOGI(TAG, "Configuring esp-sr AFE with AEC & WakeNet...");
    srmodel_list_t *models = esp_srmodel_init("model");
    afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_VC, AFE_MODE_LOW_COST);

    if (afe_config != NULL) {
        afe_config->aec_init = true;
        afe_config->ns_init = true;
        afe_config->wakenet_init = true;
        afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
        afe_config->wakenet_mode = DET_MODE_90;
        afe_config->vad_init = true;
        afe_config->vad_mode = VAD_MODE_1;
        afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
        afe_config = afe_config_check(afe_config);

        s_afe_handle = esp_afe_handle_from_config(afe_config);
        if (s_afe_handle != NULL) {
            s_afe_data = s_afe_handle->create_from_config(afe_config);
            ESP_LOGI(TAG, "esp-sr AFE pipeline initialized successfully!");
        } else {
            ESP_LOGE(TAG, "Failed to get esp-sr afe_handle!");
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize afe_config!");
    }

    // Tasks 생성 (Core 0: spk_play, ws_tx / Core 1: audio_feed, audio_fetch)
    xTaskCreatePinnedToCoreWithCaps(spk_play_task, "spk_play_task", 8192, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreatePinnedToCoreWithCaps(ws_tx_task, "ws_tx_task", 6144, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreatePinnedToCoreWithCaps(audio_feed_task, "audio_feed_task", 8192, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreatePinnedToCoreWithCaps(audio_fetch_task, "audio_fetch_task", 12288, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    s_pipeline_initialized = true;
    ESP_LOGI(TAG, "Audio Pipeline initialized and tasks launched.");
    return ESP_OK;
}

void audio_pipeline_set_discard_tts(bool discard) {
    s_discard_tts_audio = discard;
    if (!discard) {
        s_spk_total_played_bytes = 0;
    }
    ESP_LOGI(TAG, "s_discard_tts_audio set to %s (played_bytes reset)", discard ? "true" : "false");
}

void audio_pipeline_feed_tts_audio(const uint8_t *data, size_t len) {
    if (!data || len == 0 || s_spk_ringbuf == NULL) {
        return;
    }

    // 인터럽트/끼어들기 이후 도착한 이전 세션의 잔여 인-플라이트(in-flight) 패킷은 완전히 폐기
    if (s_discard_tts_audio) {
        return;
    }

    chat_state_t cur_st = chat_state_get();
    // 클라이언트가 사용자 발화를 듣는 중(LISTENING)이거나 대기(IDLE) 중일 때는 오디오를 스피커에 넣지 않음
    if (cur_st == CHAT_STATE_LISTENING || cur_st == CHAT_STATE_IDLE) {
        return;
    }

    chat_state_activity_occurred(); // 활성 오디오 수신 -> 타임아웃 타이머 리셋!

    BaseType_t sent = xRingbufferSend(s_spk_ringbuf, data, len, pdMS_TO_TICKS(50));
    if (sent == pdTRUE) {
        s_spk_buffered_bytes += len;
    } else {
        ESP_LOGW(TAG, "spk_ringbuf full, dropping %d bytes", (int)len);
    }
}

void audio_pipeline_clear_audio(void) {
    ESP_LOGW(TAG, "audio_pipeline_clear_audio() called: Flushing buffers...");

    s_discard_tts_audio = true;
    s_is_speaker_playing = false;
    s_server_done = false;
    s_spk_buffered_bytes = 0;
    s_barge_in_prebuf_count = 0;

    // 해당 버퍼의 유일한 컨슈머 태스크들에 플러시 플래그 설정 (FreeRTOS Ringbuffer 멀티 컨슈머 데드락 원천 차단)
    s_spk_flush_requested = true;
    s_ref_flush_requested = true;

    // 4. 스피커 침묵
    audio_io_spk_write_silence(20);
}

void audio_pipeline_enable_mic_streaming(bool enable) {
    s_mic_streaming_enabled = enable;
    if (enable) {
        // 새 발화 청취 시작 시 송신 링버퍼 비우기를 ws_tx_task에 요청 (멀티 컨슈머 데드락 방지)
        s_tx_flush_requested = true;
        s_mic_stream_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_last_speech_ms = s_mic_stream_start_ms;
        s_has_user_spoken = false;
        s_vad_speech_frames = 0;
        ESP_LOGI(TAG, "Mic streaming enabled.");
    } else {
        ESP_LOGI(TAG, "Mic streaming disabled.");
    }
}

bool audio_pipeline_is_speaker_playing(void) {
    return s_is_speaker_playing;
}

bool audio_pipeline_is_spk_buffer_empty(void) {
    return (s_spk_buffered_bytes == 0);
}

void audio_pipeline_set_server_done(bool done) {
    s_server_done = done;
}

uint32_t audio_pipeline_get_played_bytes(void) {
    return s_spk_total_played_bytes;
}

void audio_pipeline_reset_played_bytes(void) {
    s_spk_total_played_bytes = 0;
}

void audio_pipeline_set_mic_gain(uint8_t gain) {
    if (gain < 10) gain = 10;
    if (gain > 60) gain = 60;
    s_mic_gain = gain;
    ESP_LOGI(TAG, "Mic gain updated to %u (%.1fx)", (unsigned int)gain, gain / 10.0f);
}

uint8_t audio_pipeline_get_mic_gain(void) {
    return s_mic_gain;
}
