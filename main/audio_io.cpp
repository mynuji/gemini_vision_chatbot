#include "audio_io.h"
#include "bsp_board.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "AUDIO_IO";

static i2s_chan_handle_t s_rx_chan = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;

esp_err_t audio_io_init(void) {
    ESP_LOGI(TAG, "Initializing I2S Audio Channels (RX PDM Mic + TX STD Speaker)...");

    // [1] 마이크 채널 (PDM RX)
    if (s_rx_chan == NULL) {
        i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        rx_chan_cfg.dma_desc_num = 8;
        rx_chan_cfg.dma_frame_num = 512;
        rx_chan_cfg.auto_clear = true;

        esp_err_t ret = i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_chan);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(ret));
            return ret;
        }

        i2s_pdm_rx_config_t pdm_rx_cfg = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .clk = (gpio_num_t)I2S_MIC_CLK,
                .din = (gpio_num_t)I2S_MIC_DAT,
                .invert_flags = {.clk_inv = false}
            }
        };
        pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;

        ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_rx_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
        ESP_LOGI(TAG, "I2S PDM RX Mic initialized.");
    }

    // [2] 스피커 채널 (Standard TX for MAX98357A)
    if (s_tx_chan == NULL) {
        i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        tx_chan_cfg.dma_desc_num = 8;
        tx_chan_cfg.dma_frame_num = 512;
        tx_chan_cfg.auto_clear = true;

        esp_err_t ret = i2s_new_channel(&tx_chan_cfg, &s_tx_chan, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(ret));
            return ret;
        }

        i2s_std_config_t std_tx_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)I2S_SPK_BCLK,
                .ws = (gpio_num_t)I2S_SPK_LRC,
                .dout = (gpio_num_t)I2S_SPK_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}
            }
        };

        ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_tx_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
        ESP_LOGI(TAG, "I2S STD TX Speaker initialized.");
    }

    return ESP_OK;
}

esp_err_t audio_io_mic_read(void *dest, size_t size, size_t *bytes_read, TickType_t wait_ticks) {
    if (s_rx_chan == NULL) return ESP_ERR_INVALID_STATE;
    return i2s_channel_read(s_rx_chan, dest, size, bytes_read, wait_ticks);
}

esp_err_t audio_io_spk_write(const void *src, size_t size, size_t *bytes_written, TickType_t wait_ticks) {
    if (s_tx_chan == NULL) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(s_tx_chan, src, size, bytes_written, wait_ticks);
}

esp_err_t audio_io_spk_play_pcm16_mono(const int16_t *pcm_mono, size_t num_samples, float volume_scale) {
    if (s_tx_chan == NULL || pcm_mono == NULL || num_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t stereo_buf[256 * 2]; // 256 프레임 단위로 변환 버퍼
    size_t offset = 0;
    size_t written = 0;

    if (volume_scale < 0.0f) volume_scale = 0.0f;
    if (volume_scale > 1.0f) volume_scale = 1.0f;

    while (offset < num_samples) {
        size_t chunk = (num_samples - offset > 256) ? 256 : (num_samples - offset);
        for (size_t i = 0; i < chunk; i++) {
            int32_t sample = (int32_t)(pcm_mono[offset + i] * volume_scale);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            int32_t sample32 = sample << 16;
            stereo_buf[i * 2] = sample32;     // Left
            stereo_buf[i * 2 + 1] = sample32; // Right
        }

        esp_err_t err = i2s_channel_write(s_tx_chan, stereo_buf, chunk * sizeof(int32_t) * 2, &written, portMAX_DELAY);
        if (err != ESP_OK) return err;
        offset += chunk;
    }

    return ESP_OK;
}

void audio_io_spk_write_silence(int duration_ms) {
    if (s_tx_chan == NULL || duration_ms <= 0) return;

    int total_bytes = (AUDIO_SAMPLE_RATE * sizeof(int32_t) * 2 * duration_ms) / 1000;
    uint8_t zero_buf[512] = {0};
    size_t written = 0;

    for (int i = 0; i < total_bytes; i += 512) {
        int bytes_to_write = (total_bytes - i > 512) ? 512 : (total_bytes - i);
        i2s_channel_write(s_tx_chan, zero_buf, bytes_to_write, &written, portMAX_DELAY);
    }
}

void audio_io_play_tone(float freq_hz, int duration_ms) {
    if (s_tx_chan == NULL || duration_ms <= 0 || freq_hz <= 0.0f) return;

    int total_samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    int ramp_samples = 240; // 약 15ms 페이드인/아웃
    if (ramp_samples > total_samples / 2) ramp_samples = total_samples / 2;

    int32_t buf[256]; // 128 stereo samples
    size_t written = 0;

    for (int i = 0; i < total_samples; i += 128) {
        int frames = (total_samples - i > 128) ? 128 : (total_samples - i);
        for (int j = 0; j < frames; j++) {
            int global_i = i + j;
            float envelope = 1.0f;
            if (global_i < ramp_samples) {
                envelope = (float)global_i / (float)ramp_samples;
            } else if (global_i > total_samples - ramp_samples) {
                envelope = (float)(total_samples - global_i) / (float)ramp_samples;
            }

            int16_t val = (int16_t)(3800.0f * envelope * sinf(2.0f * 3.14159265f * freq_hz * global_i / (float)AUDIO_SAMPLE_RATE));
            int32_t val32 = ((int32_t)val) << 16;
            buf[j * 2] = val32;
            buf[j * 2 + 1] = val32;
        }
        i2s_channel_write(s_tx_chan, buf, frames * sizeof(int32_t) * 2, &written, portMAX_DELAY);
    }
    // '뚝' 팝노이즈 방지용 무음 버퍼 플러시
    audio_io_spk_write_silence(25);
}

void audio_io_play_boot_melody(void) {
    audio_io_spk_write_silence(50);
    audio_io_play_tone(523.25f, 200); // 도 (C5)
    audio_io_play_tone(659.25f, 200); // 미 (E5)
    audio_io_play_tone(783.99f, 300); // 솔 (G5)
    audio_io_spk_write_silence(100);
}

void audio_io_enable_mic(bool enable) {
    if (s_rx_chan != NULL) {
        if (enable) i2s_channel_enable(s_rx_chan);
        else i2s_channel_disable(s_rx_chan);
    }
}

void audio_io_enable_spk(bool enable) {
    if (s_tx_chan != NULL) {
        if (enable) i2s_channel_enable(s_tx_chan);
        else i2s_channel_disable(s_tx_chan);
    }
}

i2s_chan_handle_t audio_io_get_rx_chan(void) {
    return s_rx_chan;
}

i2s_chan_handle_t audio_io_get_tx_chan(void) {
    return s_tx_chan;
}
