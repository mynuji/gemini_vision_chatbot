#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 실시간 오디오 파이프라인 (PSRAM RingBuffers, FreeRTOS Tasks, esp-sr AFE) 초기화 및 시작
 */
esp_err_t audio_pipeline_init(void);

/**
 * @brief 젯슨 서버로부터 수신된 TTS 바이너리 PCM 오디오 데이터 (16kHz 16-bit Mono) 공급
 */
void audio_pipeline_feed_tts_audio(const uint8_t *data, size_t len);

/**
 * @brief 스피커 버퍼 및 오디오 큐 즉시 플러시 (Barge-in / CLEAR_AUDIO)
 */
void audio_pipeline_clear_audio(void);

/**
 * @brief 인터럽트 발생 후 네트워크 잔여 인플라이트 오디오 폐기 플래그 설정
 */
void audio_pipeline_set_discard_tts(bool discard);

/**
 * @brief 마이크 오디오의 Jetson 실시간 스트리밍 활성화/비활성화
 */
void audio_pipeline_enable_mic_streaming(bool enable);

/**
 * @brief 스피커가 현재 음성을 재생 중인지 여부
 */
bool audio_pipeline_is_speaker_playing(void);

/**
 * @brief 스피커 링버퍼가 완전히 비었는지 여부
 */
bool audio_pipeline_is_spk_buffer_empty(void);

/**
 * @brief 서버 응답 완료(DONE) 플래그 설정 (재생 종료 시점 감지용)
 */
void audio_pipeline_set_server_done(bool done);

/**
 * @brief 현재 세션에서 스피커로 실제 재생된 총 PCM 바이트 수 반환
 */
uint32_t audio_pipeline_get_played_bytes(void);

/**
 * @brief 스피커 실제 재생 바이트 수 카운터 초기화
 */
void audio_pipeline_reset_played_bytes(void);

#ifdef __cplusplus
}
#endif
