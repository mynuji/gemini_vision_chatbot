#include "camera_io.h"
#include "bsp_board.h"

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "CAMERA_IO";

// Gemini 3.5 Flash 비전 입력 최적화:
// VGA(640x480) + JPEG Quality 12 -> 약 35KB~55KB
// 무선 전송 시간(100ms 안팎)과 사물/텍스트 인식률 간의 최적 균형점
#define CAMERA_FRAME_SIZE    FRAMESIZE_VGA
#define CAMERA_JPEG_QUALITY  12        // 0~63 (낮을수록 고화질)
#define CAMERA_XCLK_FREQ_HZ  20000000

static bool s_ready = false;
static SemaphoreHandle_t s_cam_mutex = NULL;

// PSRAM 동적 힙 파편화 방지용 재사용 버퍼 (필요 시 자동 확장)
static uint8_t *s_shared_jpeg_buf = NULL;
static size_t   s_shared_jpeg_capacity = 0;

esp_err_t camera_io_init(void) {
    if (s_ready) {
        return ESP_OK;
    }

    if (s_cam_mutex == NULL) {
        s_cam_mutex = xSemaphoreCreateMutex();
        if (s_cam_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    camera_config_t config = {};
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_pclk     = CAM_PIN_PCLK;

    // 타이머/채널 1번 고정 (LCD 백라이트 또는 오디오 간섭 방지)
    config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
    config.ledc_timer   = LEDC_TIMER_1;
    config.ledc_channel = LEDC_CHANNEL_1;

    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = CAMERA_FRAME_SIZE;
    config.jpeg_quality = CAMERA_JPEG_QUALITY;
    // fb_count=1 + WHEN_EMPTY:
    // 센서 DMA가 프레임 1장을 채우면 다음 촬영 요청 시까지 캡처가 대기하여
    // 평상시 I2S 마이크/스피커, LCD DMA 및 Wi-Fi의 PSRAM 버스 대역폭을 독점하지 않음
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "Camera sensor handle not acquired");
        esp_camera_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Camera sensor detected (PID: 0x%02x, %s)", s->id.PID,
             s->id.PID == OV2640_PID ? "OV2640" : "UNKNOWN");

    if (s->id.PID == OV2640_PID) {
        // 실내 조명 기준 기본 튜닝
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_vflip(s, 0);
        s->set_hmirror(s, 0);
    }

    // 초기 재사용 캡처 버퍼 64KB 미리 확보 (PSRAM 파편화 방지)
    s_shared_jpeg_capacity = 64 * 1024;
    s_shared_jpeg_buf = (uint8_t *)heap_caps_malloc(s_shared_jpeg_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_shared_jpeg_buf == NULL) {
        ESP_LOGW(TAG, "Could not pre-allocate 64KB PSRAM capture buffer; dynamic alloc will be used");
        s_shared_jpeg_capacity = 0;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Camera driver ready (VGA JPEG q=%d). Free PSRAM: %u KB, InitTime: %lld ms",
             CAMERA_JPEG_QUALITY, (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

bool camera_io_is_ready(void) {
    return s_ready;
}

esp_err_t camera_io_capture_jpeg(uint8_t **out_jpeg, size_t *out_len,
                                 uint16_t *out_width, uint16_t *out_height) {
    if (out_jpeg == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_jpeg = NULL;
    *out_len = 0;

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cam_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
        ESP_LOGW(TAG, "Camera mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    int64_t t0 = esp_timer_get_time();

    // 1. 센서 버퍼가 차있으면 반환하여 즉시 최신 프레임 캡처 시작
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        esp_camera_fb_return(fb);
    }
    // 2. 실시간 최신 프레임 촬영
    fb = esp_camera_fb_get();

    if (fb == NULL) {
        ESP_LOGE(TAG, "Camera frame capture failed");
        ret = ESP_FAIL;
    } else if (fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        ESP_LOGE(TAG, "Invalid frame: format=%d, len=%u", fb->format, (unsigned int)fb->len);
        ret = ESP_ERR_INVALID_RESPONSE;
    } else {
        // 호출자에게 독립적인 버퍼 반환 (PSRAM)
        uint8_t *copy_buf = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for frame", (unsigned int)fb->len);
            ret = ESP_ERR_NO_MEM;
        } else {
            memcpy(copy_buf, fb->buf, fb->len);
            *out_jpeg = copy_buf;
            *out_len = fb->len;
            if (out_width)  *out_width  = (uint16_t)fb->width;
            if (out_height) *out_height = (uint16_t)fb->height;

            ESP_LOGI(TAG, "JPEG captured: %ux%u (%u bytes) in %lld ms",
                     (unsigned int)fb->width, (unsigned int)fb->height, (unsigned int)fb->len,
                     (long long)((esp_timer_get_time() - t0) / 1000));
        }
    }

    if (fb) {
        esp_camera_fb_return(fb);
    }
    xSemaphoreGive(s_cam_mutex);
    return ret;
}
