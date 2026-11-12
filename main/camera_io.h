#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// OV2640 카메라 (XIAO ESP32-S3 Sense 확장 보드) 드라이버 래퍼
// - JPEG 출력, 프레임 버퍼는 PSRAM에 1장만 두어 오디오/LCD 파이프라인과의 경합 최소화
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OV2640 카메라 초기화 (JPEG / VGA 기본)
 * @return ESP_OK 성공, 실패 시 esp_camera_init()의 에러 코드
 */
esp_err_t camera_io_init(void);

/**
 * @brief 카메라 전원 차단 및 클럭 정지 (Sleep 모드)
 *
 * 사용하지 않을 때 OV2640의 20MHz XCLK 및 DMA/센서를 정지하여
 * 150~200mW 상시 전력 소모와 모듈 발열을 원천 차단합니다.
 */
esp_err_t camera_io_deinit(void);

/**
 * @brief 카메라 초기화 완료 및 동작 중 여부
 */
bool camera_io_is_ready(void);

/**
 * @brief 현재 장면을 JPEG로 촬영하여 PSRAM 버퍼로 복사해 반환
 *
 * 프레임 버퍼에 남아 있던 오래된 프레임은 버리고 새로 촬영합니다.
 * 반환된 버퍼는 호출자가 반드시 free()로 해제해야 합니다.
 *
 * @param[out] out_jpeg  JPEG 데이터 버퍼 (PSRAM)
 * @param[out] out_len   JPEG 데이터 길이 (bytes)
 * @param[out] out_width  이미지 가로 해상도 (NULL 허용)
 * @param[out] out_height 이미지 세로 해상도 (NULL 허용)
 */
#include "esp_camera.h"

esp_err_t camera_io_capture_jpeg(uint8_t **out_jpeg, size_t *out_len,
                                 uint16_t *out_width, uint16_t *out_height);

/**
 * @brief 카메라 프레임 버퍼 직접 획득 (락 획득 포함)
 *
 * 사용 후 반드시 camera_io_return_fb()를 호출해야 합니다.
 *
 * @param timeout_ticks 뮤텍스 대기 시간 (FreeRTOS ticks)
 * @return camera_fb_t* 성공 시 프레임 버퍼 포인터, 실패 시 NULL
 */
camera_fb_t *camera_io_get_fb(uint32_t timeout_ticks);

/**
 * @brief 카메라 프레임 버퍼 반환 (뮤텍스 해제 포함)
 */
void camera_io_return_fb(camera_fb_t *fb);

/**
 * @brief LLM 사진 촬영용 고해상도(VGA 640x480) 및 실시간 LCD 프리뷰용(QVGA 320x240) 동적 전환
 * @param high_res true: VGA 고화질(LLM 분석용), false: QVGA 고속(LCD 프리뷰용)
 */
void camera_io_set_high_res(bool high_res);

#ifdef __cplusplus
}
#endif
