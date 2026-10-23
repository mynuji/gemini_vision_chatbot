#pragma once

// ============================================================================
// AI Chatbot 네트워크 및 서버 설정 파일 (app_config.h)
// ============================================================================

// [1] 환경 정의
#define ENV_HOME    1 // 집 환경
#define ENV_OFFICE  2 // 회사 환경

// ★ 여기서 접속할 환경을 선택하세요: ENV_HOME 또는 ENV_OFFICE
#define CURRENT_ENV ENV_HOME

// [2] 오디오 볼륨 설정 (0.0f ~ 1.0f)
#define DEFAULT_VOLUME_SCALE  0.70f // 기존 대비 30% 감소된 볼륨

// [3] 각 환경별 네트워크 및 젯슨 서버 주소 설정
#if (CURRENT_ENV == ENV_HOME)
    #define CONFIG_ENV_NAME       "HOME"
    #define CONFIG_WIFI_SSID      "NUJI"
    #define CONFIG_WIFI_PASS      "99999999"
    #define CONFIG_WS_SERVER_URI  "ws://192.168.0.2:8765"

#elif (CURRENT_ENV == ENV_OFFICE)
    #define CONFIG_ENV_NAME       "HOTSPOT"
    #define CONFIG_WIFI_SSID      "IGI"
    #define CONFIG_WIFI_PASS      "99999999"
    #define CONFIG_WS_SERVER_URI  "ws://10.237.180.20:8765"

#else
    #error "Invalid CURRENT_ENV! Please choose ENV_HOME or ENV_OFFICE."
#endif
