#pragma once

#include "driver/gpio.h"

// ============================================================================
// XIAO ESP32-S3 Sense 핀 및 기본 하드웨어 구성
// ============================================================================

// [1] 디스플레이 (GC9A01 / ST7789 원형 LCD)
#define LCD_HOST      SPI2_HOST
#define LCD_CLK       7
#define LCD_MOSI      9
#define LCD_CS        2
#define LCD_DC        4
#define LCD_RST      -1
#define LCD_BL       43

// [2] 정전식 터치 센서
#define TOUCH_INT    44

// [3] 오디오 - PDM 마이크 (XIAO Sense 내장)
#define I2S_MIC_CLK  42
#define I2S_MIC_DAT  41

// [4] 오디오 - MAX98357A I2S 스피커 앰프
#define I2S_SPK_LRC   1
#define I2S_SPK_BCLK  3
#define I2S_SPK_DOUT  8

// [5] 오디오 기본 샘플링 파라미터
#define AUDIO_SAMPLE_RATE 16000

// [6] 카메라 (OV2640 / Sense 확장 보드)
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  10
#define CAM_PIN_SIOD  40
#define CAM_PIN_SIOC  39
#define CAM_PIN_D7    48
#define CAM_PIN_D6    11
#define CAM_PIN_D5    12
#define CAM_PIN_D4    14
#define CAM_PIN_D3    16
#define CAM_PIN_D2    18
#define CAM_PIN_D1    17
#define CAM_PIN_D0    15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  47
#define CAM_PIN_PCLK  13
