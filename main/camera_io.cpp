#include "camera_io.h"
#include "bsp_board.h"

#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "CAMERA_IO";

// Seeed Studio XIAO ESP32-S3 Sense 최적화:
// 고속 QVGA(320x240) 해상도, 고화질 JPEG (quality 10~12)
// PSRAM 더블 버퍼링(fb_count=2)과 CAMERA_GRAB_LATEST로 끊김 없는 30fps 프리뷰 및 지연 없는 최신 사진 획득
#define CAMERA_FRAME_SIZE    FRAMESIZE_QVGA
#define CAMERA_JPEG_QUALITY  10
#define CAMERA_XCLK_FREQ_HZ  20000000

static bool s_initialized = false;
static bool s_in_standby = false;
static SemaphoreHandle_t s_cam_mutex = NULL;

esp_err_t camera_io_init(void) {
    if (s_cam_mutex == NULL) {
        s_cam_mutex = xSemaphoreCreateMutex();
        if (s_cam_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_cam_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "camera_io_init: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    if (s_initialized) {
        // 이미 초기화되어 있다면 standby에서 복귀만 수행
        xSemaphoreGive(s_cam_mutex);
        return camera_io_wake();
    }

    camera_config_t config = {};
    config.pin_pwdn     = CAM_PIN_PWDN;   // XIAO Sense: -1 (하드웨어 PWDN 없음)
    config.pin_reset    = CAM_PIN_RESET;  // XIAO Sense: -1 (하드웨어 RESET 없음)
    config.pin_xclk     = CAM_PIN_XCLK;   // GPIO 10
    config.pin_sccb_sda = CAM_PIN_SIOD;   // GPIO 40
    config.pin_sccb_scl = CAM_PIN_SIOC;   // GPIO 39
    config.pin_d7       = CAM_PIN_D7;     // GPIO 48
    config.pin_d6       = CAM_PIN_D6;     // GPIO 11
    config.pin_d5       = CAM_PIN_D5;     // GPIO 12
    config.pin_d4       = CAM_PIN_D4;     // GPIO 14
    config.pin_d3       = CAM_PIN_D3;     // GPIO 16
    config.pin_d2       = CAM_PIN_D2;     // GPIO 18
    config.pin_d1       = CAM_PIN_D1;     // GPIO 17
    config.pin_d0       = CAM_PIN_D0;     // GPIO 15
    config.pin_vsync    = CAM_PIN_VSYNC;  // GPIO 38
    config.pin_href     = CAM_PIN_HREF;   // GPIO 47
    config.pin_pclk     = CAM_PIN_PCLK;   // GPIO 13

    config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
    config.ledc_timer   = LEDC_TIMER_1;
    config.ledc_channel = LEDC_CHANNEL_1;

    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = CAMERA_FRAME_SIZE;
    config.jpeg_quality = CAMERA_JPEG_QUALITY;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
        xSemaphoreGive(s_cam_mutex);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "Camera sensor handle not acquired");
        esp_camera_deinit();
        xSemaphoreGive(s_cam_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Camera sensor detected (PID: 0x%02x, %s)", s->id.PID,
             s->id.PID == OV2640_PID ? "OV2640" : (s->id.PID == OV3660_PID ? "OV3660" : (s->id.PID == OV5640_PID ? "OV5640" : "UNKNOWN")));

    // Seeed Studio XIAO ESP32S3 Sense 센서별 하드웨어 최적화 세팅
    if (s->id.PID == OV3660_PID) {
        ESP_LOGI(TAG, "Applying official tuning for XIAO ESP32S3 Sense (OV3660)");
        s->set_vflip(s, 1);       // Seeed Studio XIAO 렌즈 마운트 기준 상하 반전 정상화
        s->set_hmirror(s, 0);     // 좌우 미러링 정상화
        s->set_brightness(s, 1);  // 실내 밝기 보정
        s->set_saturation(s, -2); // OV3660 특유의 과포화 색감 자연스럽게 교정
        s->set_whitebal(s, 1);    // AWB(자동 화이트밸런스) 활성화
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
    } else if (s->id.PID == OV2640_PID) {
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_vflip(s, 0);
        s->set_hmirror(s, 0);
    } else if (s->id.PID == OV5640_PID) {
        s->set_reg(s, 0x302C, 0xC0, 0x00);
    }

    s_initialized = true;
    s_in_standby = false;
    xSemaphoreGive(s_cam_mutex);

    ESP_LOGI(TAG, "Camera driver ready (XIAO ESP32S3 Sense pipeline). Free PSRAM: %u KB, InitTime: %lld ms",
             (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

esp_err_t camera_io_standby(void) {
    if (!s_initialized) {
        return ESP_OK;
    }
    if (s_cam_mutex && xSemaphoreTake(s_cam_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "camera_io_standby: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (s_in_standby) {
        if (s_cam_mutex) xSemaphoreGive(s_cam_mutex);
        return ESP_OK;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        if (s->id.PID == OV3660_PID) {
            // Seeed Studio XIAO ESP32-S3 Sense OV3660 Software Power Down:
            // 센서 내부 ISP, ADC, 픽셀 어레이 전원을 차단하여 발열과 소비 전력을 0으로 정지
            s->set_reg(s, 0x3008, 0x40, 0x40);
        } else if (s->id.PID == OV2640_PID) {
            s->set_reg(s, 0xFF, 0xFF, 0x01); // Bank SENSOR
            s->set_reg(s, 0x09, 0x10, 0x10); // COM2 Soft Sleep
        } else if (s->id.PID == OV5640_PID) {
            s->set_reg(s, 0x3008, 0x40, 0x40);
        }
    }

    s_in_standby = true;
    if (s_cam_mutex) xSemaphoreGive(s_cam_mutex);

    ESP_LOGI(TAG, "Camera entered low-power software standby (Heat & ADC halted)");
    return ESP_OK;
}

esp_err_t camera_io_wake(void) {
    if (!s_initialized) {
        return camera_io_init();
    }
    if (s_cam_mutex && xSemaphoreTake(s_cam_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "camera_io_wake: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_in_standby) {
        if (s_cam_mutex) xSemaphoreGive(s_cam_mutex);
        return ESP_OK;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        if (s->id.PID == OV3660_PID) {
            // OV3660 Software Wakeup (Power Down 해제)
            s->set_reg(s, 0x3008, 0x40, 0x00);
        } else if (s->id.PID == OV2640_PID) {
            s->set_reg(s, 0xFF, 0xFF, 0x01);
            s->set_reg(s, 0x09, 0x10, 0x00);
        } else if (s->id.PID == OV5640_PID) {
            s->set_reg(s, 0x3008, 0x40, 0x00);
        }
        // 센서 PLL 및 클럭 복원 안정화 대기 (30ms)
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    // 슬립 이전에 버퍼에 남아있던 잔여/불완전 프레임 플러시
    camera_fb_t *dummy = esp_camera_fb_get();
    if (dummy) {
        esp_camera_fb_return(dummy);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_in_standby = false;
    if (s_cam_mutex) xSemaphoreGive(s_cam_mutex);

    ESP_LOGI(TAG, "Camera woken from software standby (Ready for capture)");
    return ESP_OK;
}

esp_err_t camera_io_deinit(void) {
    // XIAO ESP32-S3 Sense는 하드웨어 PWDN 핀이 없으므로,
    // 드라이버를 파괴하지 않고 안전한 소프트웨어 Standby를 수행하여 센서 발열을 차단합니다.
    return camera_io_standby();
}

void camera_io_set_high_res(bool high_res) {
    // 안정적인 30fps LCD 프리뷰 및 초고속 사진 촬영을 위해 센서는 상시 QVGA로 고정 유지
}

bool camera_io_is_ready(void) {
    return s_initialized && !s_in_standby;
}

bool camera_io_is_initialized(void) {
    return s_initialized;
}

esp_err_t camera_io_capture_jpeg(uint8_t **out_jpeg, size_t *out_len,
                                 uint16_t *out_width, uint16_t *out_height) {
    if (out_jpeg == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_jpeg = NULL;
    *out_len = 0;

    bool was_in_standby = !camera_io_is_ready();
    if (was_in_standby) {
        ESP_LOGI(TAG, "camera_io_capture_jpeg: waking camera on-demand...");
        esp_err_t err = camera_io_wake();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (xSemaphoreTake(s_cam_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
        ESP_LOGW(TAG, "Camera mutex timeout");
        if (was_in_standby) {
            camera_io_standby();
        }
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

    // 단독 호출로 깨웠던 경우, 촬영 완료 즉시 Standby로 복귀하여 발열 차단
    if (was_in_standby) {
        camera_io_standby();
    }

    return ret;
}

camera_fb_t *camera_io_get_fb(uint32_t timeout_ticks) {
    if (!s_initialized || s_in_standby || s_cam_mutex == NULL) {
        return NULL;
    }
    if (xSemaphoreTake(s_cam_mutex, (TickType_t)timeout_ticks) != pdTRUE) {
        return NULL;
    }
    if (!s_initialized || s_in_standby) {
        xSemaphoreGive(s_cam_mutex);
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
