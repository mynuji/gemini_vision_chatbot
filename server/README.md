# Jetson Xavier Voice Agent Server

ESP32-S3 AI 챗봇 클라이언트와 실시간으로 연동되는 WebSocket 음성 대화 서버입니다.

## 기능 요약
- **WebSocket 엔드포인트**: `ws://0.0.0.0:8765`
- **음성 스트리밍 수신**: 16kHz, 16-bit, Mono PCM 바이너리 수신
- **실시간 VAD**: Silero VAD 기반 발화 시작/종료 감지
- **음성 인식 (STT)**: `whisper.cpp` 독립 프로세스 연동
- **LLM 추론**: 로컬 OpenAI 호환 엔드포인트 (`http://127.0.0.1:8080/v1`)
- **실시간 TTS**: Edge-TTS (`ko-KR-SunHiNeural`) + FFmpeg 실시간 PCM 변환 스트리밍
- **제어 패킷 및 대화 끼어들기 (Barge-in)**:
  - `{"type": "touch"}`: ESP32 화면 터치 이벤트 수신 및 AI 발화 즉시 취소
  - `{"type": "interrupt"}`: 사용자 발화 또는 터치에 의한 즉각 중단 명령 처리

## 실행 방법
```bash
python3 server/server.py
```
