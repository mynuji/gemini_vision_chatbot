#include "camera_io.h"
#include "bsp_board.h"

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "img_converters.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "CAMERA_IO";

// 실시간 영상 프리뷰 FPS 극대화 및 빠른 저지연 비전 질문 지원:
// OV2640 센서를 안정적인 고속 QVGA(320x240)로 상시 운용하여 해상도 전환에 따른 센서 멈춤/재동기화 지연 원천 제거
#define CAMERA_FRAME_SIZE    FRAMESIZE_QVGA
#define CAMERA_JPEG_QUALITY  10        // 0~63 (10=고화질이면서 약 10KB 크기로 초고속 Wi-Fi 전송 가능)
#define CAMERA_XCLK_FREQ_HZ  20000000

static bool s_ready = false;
static SemaphoreHandle_t s_cam_mutex = NULL;

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

    // 정상 색상(하드웨어 ISP AWB/AEC 적용) 및 단일 프레임 버퍼(fb_count=1)로 큐 적체 지연 원천 제거
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = CAMERA_FRAME_SIZE;
    config.jpeg_quality = CAMERA_JPEG_QUALITY;
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
             s->id.PID == OV2640_PID ? "OV2640" : (s->id.PID == OV5640_PID ? "OV5640" : "UNKNOWN"));

    if (s->id.PID == OV5640_PID) {
        // OV5640 발열 최적화 (0x302C bit[7:6]: 11=4x -> 00=1x)
        s->set_reg(s, 0x302C, 0xC0, 0x00);
    } else if (s->id.PID == OV2640_PID) {
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

    s_ready = true;

    ESP_LOGI(TAG, "Camera driver ready (Stable QVGA 320x240 pipeline). Free PSRAM: %u KB, InitTime: %lld ms",
             (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

void camera_io_set_high_res(bool high_res) {
    // 안정적인 30fps LCD 프리뷰 및 초고속 사진 촬영을 위해 센서는 상시 QVGA로 고정 유지
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

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "Camera frame capture failed");
        ret = ESP_FAIL;
    } else if (fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        ESP_LOGE(TAG, "Invalid frame: format=%d, len=%u", fb->format, (unsigned int)fb->len);
        ret = ESP_ERR_INVALID_RESPONSE;
    } else {
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

camera_fb_t *camera_io_get_fb(uint32_t timeout_ticks) {
    if (!s_ready || s_cam_mutex == NULL) {
        return NULL;
    }
    if (xSemaphoreTake(s_cam_mutex, (TickType_t)timeout_ticks) != pdTRUE) {
        return NULL;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(s_cam_mutex);
        return NULL;
    }
    return fb;
}

void camera_io_return_fb(camera_fb_t *fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
    if (s_cam_mutex) {
        xSemaphoreGive(s_cam_mutex);
    }
}
