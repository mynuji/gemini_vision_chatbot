#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAT_STATE_IDLE = 0,     // 대기: 눈 깜빡임, 'Alexa' 또는 터치 대기
    CHAT_STATE_LISTENING,    // 사용자 발화 듣는 중: 마이크 음성을 Jetson으로 전송
    CHAT_STATE_THINKING,     // Jetson 답변 생성 대기: 마이크 스트리밍 중단
    CHAT_STATE_SPEAKING      // 스피커로 AI 음성 출력 중: 끼어들기(Barge-in) 감지
} chat_state_t;

/**
 * @brief 대화 상태 머신 초기화
 */
void chat_state_init(void);

/**
 * @brief 현재 대화 상태 반환
 */
chat_state_t chat_state_get(void);

/**
 * @brief 상태 변경 및 UI/표정/애니메이션 동기화
 */
void chat_state_set(chat_state_t new_state);

/**
 * @brief 터치 센서 입력 이벤트 처리 (IDLE -> 세션 시작, SPEAKING -> 끼어들기)
 */
void chat_state_handle_touch(void);

/**
 * @brief WakeNet ("Alexa") 호출어 감지 이벤트 처리
 */
void chat_state_handle_wake_word(void);

/**
 * @brief 사용자 음성 끼어들기 (Voice Barge-in) 이벤트 처리
 */
void chat_state_handle_voice_barge_in(void);

/**
 * @brief VAD 사용자 발화 종료 감지 (LISTENING -> THINKING 전환)
 */
void chat_state_handle_speech_end(void);

/**
 * @brief 젯슨 서버로부터 수신된 제어 텍스트/JSON 메시지 처리
 */
void chat_state_handle_server_text(const char *text, size_t len);

/**
 * @brief AI 스피커 버퍼 재생 완료 통보 (SPEAKING -> LISTENING 다음 턴 대기)
 */
void chat_state_handle_speaker_finished(void);

/**
 * @brief 사용자 음성 또는 터치 활동 감지 통보 (60초 무응답 타이머 리셋)
 */
void chat_state_activity_occurred(void);

/**
 * @brief 주기적 타임아웃 검사 (60초 무응답 시 세션 종료 및 IDLE 복귀)
 */
void chat_state_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
