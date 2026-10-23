#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 마이크(PDM RX) 및 스피커(I2S TX) 채널 초기화
 */
esp_err_t audio_io_init(void);

/**
 * @brief 마이크(PDM RX)에서 16kHz 16-bit Mono PCM 오디오 데이터 읽기
 */
esp_err_t audio_io_mic_read(void *dest, size_t size, size_t *bytes_read, TickType_t wait_ticks);

/**
 * @brief 스피커(I2S TX)로 raw 스테레오 데이터 직접 전송
 */
esp_err_t audio_io_spk_write(const void *src, size_t size, size_t *bytes_written, TickType_t wait_ticks);

/**
 * @brief 16-bit Mono PCM 데이터를 MAX98357A용 32-bit Stereo로 변환하여 스피커 재생
 * @param pcm_mono 16kHz 16-bit Mono 샘플 포인터
 * @param num_samples 샘플 수
 * @param volume_scale 볼륨 배율 (0.0f ~ 1.0f)
 */
esp_err_t audio_io_spk_play_pcm16_mono(const int16_t *pcm_mono, size_t num_samples, float volume_scale);

/**
 * @brief 팝노이즈 방지용 무음(0) 데이터 전송
 */
void audio_io_spk_write_silence(int duration_ms);

/**
 * @brief 특정 주파수의 테스트 톤 재생
 */
void audio_io_play_tone(float freq_hz, int duration_ms);

/**
 * @brief 부팅 확인용 '도-미-솔' 멜로디 재생
 */
void audio_io_play_boot_melody(void);

/**
 * @brief 마이크 채널 켜기/끄기
 */
void audio_io_enable_mic(bool enable);

/**
 * @brief 스피커 채널 켜기/끄기
 */
void audio_io_enable_spk(bool enable);

/**
 * @brief I2S 마이크(RX) 채널 핸들 반환 (esp-sr AFE 등에서 사용)
 */
i2s_chan_handle_t audio_io_get_rx_chan(void);

/**
 * @brief I2S 스피커(TX) 채널 핸들 반환
 */
i2s_chan_handle_t audio_io_get_tx_chan(void);

#ifdef __cplusplus
}
#endif
