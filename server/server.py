import asyncio
import websockets
import json
import re
import numpy as np
import torch
import time
import subprocess
import tempfile
import os
import wave
from collections import deque
import edge_tts
import base64
import urllib.request
import urllib.error
import threading

# ==========================================
# 0. 환경 변수(.env) 로딩
# ==========================================
def load_env_file(path):
    """server/.env 파일의 KEY=VALUE 항목을 환경 변수로 로드 (이미 설정된 값은 유지)"""
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip().strip('"').strip("'")
            if key and key not in os.environ:
                os.environ[key] = value

load_env_file(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))

# ==========================================
# 1. 전역 설정 및 모델 초기화
# ==========================================
print("[시스템] Silero VAD 모델 로딩 중...")
vad_model, utils = torch.hub.load(repo_or_dir='snakers4/silero-vad', model='silero_vad', force_reload=False, trust_repo=True)
vad_model.eval()

# ------------------------------------------
# Google Gemini (클라우드 LLM / 비전) 설정
# ------------------------------------------
GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "").strip()
if not GEMINI_API_KEY:
    raise SystemExit("[오류] GEMINI_API_KEY가 설정되지 않았습니다. server/.env 파일 또는 환경 변수에 설정해 주세요.")

# gemini-1.5 / 2.5 계열은 신규 사용자에게 종료(404)되어 기본값을 최신 Flash로 지정 (환경 변수로 변경 가능)
GEMINI_MODEL = os.environ.get("GEMINI_MODEL", "gemini-3.5-flash").strip()
# 주 모델이 과부하(503)/할당량 초과(429)/미지원(404)일 때 순서대로 시도할 대체 모델
GEMINI_FALLBACK_MODELS = [m.strip() for m in os.environ.get(
    "GEMINI_FALLBACK_MODELS", "gemini-3.5-flash-lite,gemini-3.8-flash"
).split(",") if m.strip() and m.strip() != GEMINI_MODEL]
# 음성 대화 응답 지연 최소화를 위한 사고(thinking) 수준: minimal | low | medium | high
GEMINI_THINKING_LEVEL = os.environ.get("GEMINI_THINKING_LEVEL", "minimal").strip()
GEMINI_MAX_OUTPUT_TOKENS = int(os.environ.get("GEMINI_MAX_OUTPUT_TOKENS", "1024"))

# ------------------------------------------
# Local LLM 설정 (127.0.0.1:8080)
# ------------------------------------------
LOCAL_LLM_URL = os.environ.get("LOCAL_LLM_URL", "http://127.0.0.1:8080").strip()
LOCAL_LLM_MODEL = os.environ.get("LOCAL_LLM_MODEL", "default").strip()

# ------------------------------------------
# OpenAI Realtime API 설정
# ------------------------------------------
OPENAI_API_KEY = os.environ.get("OPENAI_API_KEY", "").strip()
OPENAI_REALTIME_MODEL = os.environ.get("OPENAI_REALTIME_MODEL", "gpt-4o-realtime-preview-2024-12-17").strip()
OPENAI_VOICE = os.environ.get("OPENAI_VOICE", "alloy").strip()

# ------------------------------------------
# Gemini Realtime (Multimodal Live API) 설정
# ------------------------------------------
GEMINI_REALTIME_MODEL = os.environ.get("GEMINI_REALTIME_MODEL", "gemini-2.0-flash-exp").strip()
GEMINI_REALTIME_VOICE = os.environ.get("GEMINI_REALTIME_VOICE", "Aoede").strip()

print(f"[시스템] Gemini LLM 준비 완료: {GEMINI_MODEL} (대체: {', '.join(GEMINI_FALLBACK_MODELS) or '없음'})")
print(f"[시스템] Local LLM 설정: {LOCAL_LLM_URL} (모델: {LOCAL_LLM_MODEL})")
print(f"[시스템] Gemini Realtime 모델: {GEMINI_REALTIME_MODEL} (음성: {GEMINI_REALTIME_VOICE})")
print(f"[시스템] OpenAI Realtime 모델: {OPENAI_REALTIME_MODEL} (키 등록 여부: {'등록됨' if OPENAI_API_KEY else '미등록(환경변수 필요)'})")

VOICE = "ko-KR-SunHiNeural"

SAMPLE_RATE = 16000
VAD_THRESHOLD = 0.40  # TV 소음으로 인한 오작동 방지 및 음성 감도 최적화 (0.35 -> 0.40)
SILENCE_LIMIT = 0.8   # 침묵 대기 시간 단축 (1.2s -> 0.8s)으로 AI 답변 지연 400ms 단축

WHISPER_CLI_PATH = "/home/igi/work/voice_agent/whisper.cpp/build/bin/whisper-cli"
WHISPER_MODEL_PATH = "/home/igi/work/voice_agent/whisper.cpp/models/ggml-base.bin"

def clean_text_for_tts(text):
    text = re.sub(r'[\*\#\-_~`>]', '', text)
    text = re.sub(r'[^\w\s.,?!\'\"가-힣ㄱ-ㅎㅏ-ㅣ]', '', text)
    cleaned = text.strip()
    if cleaned and not cleaned[-1] in [".", "?", "!", ","]:
        cleaned += "."
    return cleaned

# ==========================================
# MP3 -> PCM (16kHz, 16-bit, Mono) 실시간 변환 함수
# ==========================================
async def convert_mp3_to_pcm(mp3_bytes):
    """FFmpeg를 사용해 Edge-TTS의 MP3 출력을 RAW PCM으로 디코딩"""
    try:
        process = await asyncio.create_subprocess_exec(
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-i", "pipe:0", 
            "-f", "s16le", "-ac", "1", "-ar", str(SAMPLE_RATE), "pipe:1",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE
        )
        stdout, stderr = await process.communicate(input=mp3_bytes)
        if process.returncode != 0:
            print(f"[경고] FFmpeg 변환 에러: {stderr.decode()}")
            return b""
        return stdout
    except Exception as e:
        print(f"[오류] PCM 변환 중 예외 발생: {e}")
        return b""

# ==========================================
# 2. whisper.cpp 독립 프로세스 실행 함수
# ==========================================
async def run_whisper_cpp(audio_np):
    t_start = time.perf_counter()
    
    if not os.path.exists(WHISPER_CLI_PATH):
        print(f"\n[오류] whisper-cli 실행 파일을 찾을 수 없습니다: {WHISPER_CLI_PATH}\n")
        return "", 0.0
        
    if not os.path.exists(WHISPER_MODEL_PATH):
        print(f"\n[오류] whisper 모델 파일을 찾을 수 없습니다: {WHISPER_MODEL_PATH}\n")
        return "", 0.0

    if audio_np is None or len(audio_np) == 0:
        return "", 0.0
        
    # 절대적인 디지털 무음 및 원거리 TV 잡음 레벨 이하는 whisper 실행 생략 (0.008 기준)
    peak_val = np.max(np.abs(audio_np))
    if peak_val < 0.008:
        return "", (time.perf_counter() - t_start) * 1000

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        
    try:
        audio_int16 = (audio_np * 32767).astype(np.int16)

        with wave.open(tmp_path, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(SAMPLE_RATE)
            wf.writeframes(audio_int16.tobytes())

        cmd = [
            WHISPER_CLI_PATH, "-m", WHISPER_MODEL_PATH, "-f", tmp_path,
            "-l", "ko", "-nt", "-bs", "1", "-bo", "1", "-t", "4"
        ]
        
        process = await asyncio.create_subprocess_exec(
            *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE
        )
        stdout, stderr = await process.communicate()
        
        result_text = ""
        if process.returncode == 0:
            output = stdout.decode("utf-8", errors="ignore")
            lines = output.splitlines()
            text_lines = []
            for line in lines:
                line_str = line.strip()
                if line_str and not line_str.startswith("whisper_") and not line_str.startswith("main:"):
                    if "]" in line_str:
                        parts = line_str.split("]", 1)
                        if len(parts) > 1 and parts[1].strip():
                            text_lines.append(parts[1].strip())
                    else:
                        text_lines.append(line_str)
            result_text = " ".join(text_lines).strip()
            
            # Whisper 한국어 환각 및 잡음 괄호 필터링
            clean_check = result_text.replace("-", "").replace(".", "").replace(" ", "").strip()
            HALLUCINATIONS = ["감사합니다", "고맙습니다", "시청해주셔서감사합니다", "MBC뉴스", "KBS뉴스", "SBS뉴스", "끝"]
            
            # (1) 괄호로 둘러싸인 비언어 잡음/환각 필터링 (예: "(아과쉬 billion lag 자리 Bong bye)", "(음악)", "[박수]" 등)
            stripped_text = result_text.strip()
            is_noise_bracket = (stripped_text.startswith("(") and stripped_text.endswith(")")) or \
                               (stripped_text.startswith("[") and stripped_text.endswith("]"))

            if is_noise_bracket:
                print(f"[서버] 비언어 잡음/괄호 환각 필터링: '{result_text}' 무시")
                result_text = ""
            elif clean_check in HALLUCINATIONS:
                print(f"[서버] 환각(Hallucination) 필터링: '{result_text}' 무시")
                result_text = ""
        else:
            print(f"\n[오류] whisper-cli 실행 중 에러 발생: {stderr.decode('utf-8', errors='ignore')}\n")
            
    except Exception as e:
        print(f"\n[오류] whisper-cpp 실행 예외 발생: {e}\n")
        result_text = ""
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
            
    asr_ms = (time.perf_counter() - t_start) * 1000
    return result_text, asr_ms

# ==========================================
# 2-1. Google Gemini 실시간 스트리밍 연동 (REST SSE)
# ==========================================
def build_gemini_payload(messages, image_bytes=None, image_mime="image/jpeg", with_thinking=True, model="gemini-3.5-flash"):
    """
    OpenAI 형식 대화 이력([{role, content}]) 및 이미지 데이터를 Gemini REST API 페이로드로 변환
    """
    system_texts = []
    contents = []
    for msg in messages:
        role = msg.get("role")
        text = (msg.get("content") or "").strip()
        if not text:
            continue
        if role == "system":
            if text not in system_texts:
                system_texts.append(text)
            continue
        g_role = "model" if role == "assistant" else "user"
        if not contents and g_role == "model":
            continue
        if contents and contents[-1]["role"] == g_role:
            contents[-1]["parts"].append({"text": text})
        else:
            contents.append({"role": g_role, "parts": [{"text": text}]})

    if image_bytes:
        b64_img = base64.b64encode(image_bytes).decode("ascii")
        img_part = {"inline_data": {"mime_type": image_mime, "data": b64_img}}
        if contents and contents[-1]["role"] == "user":
            contents[-1]["parts"].insert(0, img_part)
        else:
            contents.append({"role": "user", "parts": [img_part]})

    generation_config = {
        "maxOutputTokens": GEMINI_MAX_OUTPUT_TOKENS,
        "temperature": 0.3,
        "topP": 0.85,
    }
    if with_thinking and not model.startswith("gemini-1.") and GEMINI_THINKING_LEVEL:
        generation_config["thinkingConfig"] = {"thinkingLevel": GEMINI_THINKING_LEVEL}

    payload = {
        "contents": contents,
        "generationConfig": generation_config
    }
    if system_texts:
        payload["system_instruction"] = {"parts": [{"text": "\n".join(system_texts)}]}

    return payload

_models_without_thinking_config = set()

async def stream_gemini(messages, image_bytes=None, image_mime="image/jpeg"):
    """
    Gemini REST API SSE 스트리밍 응답 텍스트 조각을 순서대로 yield하는 비동기 제너레이터.
    별도의 google-genai SDK 없이 표준 라이브러리(urllib/threading/asyncio)로 동작하여 Python 3.8+ 모든 환경에서 지원됩니다.
    첫 토큰을 받기 전에 실패(503 과부하, 429 할당량, 404 모델 종료 등)하면 대체 모델로 자동 재시도합니다.
    """
    last_error = None

    for model in [GEMINI_MODEL] + GEMINI_FALLBACK_MODELS:
        yielded = False
        for attempt in range(2):
            with_thinking = (model not in _models_without_thinking_config)
            payload = build_gemini_payload(messages, image_bytes, image_mime, with_thinking=with_thinking, model=model)
            payload_bytes = json.dumps(payload).encode("utf-8")
            print(f"\n[Gemini 호출] Gemini 스트리밍 API 호출 시작 (모델: {model}, 사진 첨부: {'예 (' + str(len(image_bytes)) + ' bytes)' if image_bytes else '아니오'}, 대화 턴 수: {len(messages)})")
            url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:streamGenerateContent?alt=sse&key={GEMINI_API_KEY}"
            req = urllib.request.Request(url, data=payload_bytes, headers={"Content-Type": "application/json"})

            queue = asyncio.Queue()
            loop = asyncio.get_running_loop()
            stop_event = threading.Event()

            def reader_thread():
                try:
                    with urllib.request.urlopen(req, timeout=15) as resp:
                        for line in resp:
                            if stop_event.is_set():
                                break
                            line_str = line.decode("utf-8").strip()
                            if line_str.startswith("data: "):
                                try:
                                    data = json.loads(line_str[6:])
                                    for cand in data.get("candidates", []):
                                        for part in cand.get("content", {}).get("parts", []):
                                            if "text" in part and not part.get("thought", False):
                                                loop.call_soon_threadsafe(queue.put_nowait, part["text"])
                                except Exception:
                                    pass
                    loop.call_soon_threadsafe(queue.put_nowait, None)
                except Exception as e:
                    loop.call_soon_threadsafe(queue.put_nowait, e)

            thread = threading.Thread(target=reader_thread, daemon=True)
            thread.start()

            try:
                while True:
                    item = await queue.get()
                    if item is None:
                        break
                    if isinstance(item, Exception):
                        raise item
                    yielded = True
                    yield item
                if yielded:
                    return
                last_error = RuntimeError(f"{model}: 빈 응답 수신")
                print(f"\n[Gemini] {model} 빈 응답 -> 대체 모델 시도")
                break
            except asyncio.CancelledError:
                stop_event.set()
                raise
            except Exception as e:
                stop_event.set()
                if yielded:
                    # 이미 일부 답변이 음성으로 나간 뒤라면 다른 모델로 처음부터 다시 말하지 않음
                    raise
                last_error = e
                err_text = str(e).lower()
                # 모델이 사고 설정을 지원하지 않으면 사고 설정 없이 즉시 재시도
                if attempt == 0 and ("thinking" in err_text or "400" in err_text) and model not in _models_without_thinking_config:
                    print(f"\n[Gemini] {model} 사고 설정 미지원 또는 400 -> 기본 설정으로 재시도")
                    _models_without_thinking_config.add(model)
                    continue
                print(f"\n[Gemini] {model} 호출 실패 -> 대체 모델 시도: {e}")
    raise last_error or RuntimeError("사용 가능한 Gemini 모델이 없습니다.")

# ==========================================
# 2-1. 실시간 오디오 리샘플링 (16kHz <-> 24kHz)
# ==========================================
def resample_24k_to_16k(pcm24k_bytes):
    """24kHz 16-bit Mono PCM -> 16kHz 16-bit Mono PCM (3:2 초고속 선형 다운샘플링)"""
    if not pcm24k_bytes:
        return b""
    audio24 = np.frombuffer(pcm24k_bytes, dtype=np.int16)
    n_out = int(len(audio24) * 2 / 3)
    if n_out <= 0:
        return b""
    x_old = np.linspace(0, len(audio24) - 1, len(audio24))
    x_new = np.linspace(0, len(audio24) - 1, n_out)
    return np.interp(x_new, x_old, audio24).astype(np.int16).tobytes()

def resample_16k_to_24k(pcm16k_bytes):
    """16kHz 16-bit Mono PCM -> 24kHz 16-bit Mono PCM (2:3 초고속 선형 업샘플링)"""
    if not pcm16k_bytes:
        return b""
    audio16 = np.frombuffer(pcm16k_bytes, dtype=np.int16)
    n_out = int(len(audio16) * 3 / 2)
    if n_out <= 0:
        return b""
    x_old = np.linspace(0, len(audio16) - 1, len(audio16))
    x_new = np.linspace(0, len(audio16) - 1, n_out)
    return np.interp(x_new, x_old, audio16).astype(np.int16).tobytes()

# ==========================================
# 2-2. Local LLM 스트리밍 (127.0.0.1:8080)
# ==========================================
async def stream_local_llm(messages, image_bytes=None):
    """로컬 LLM (127.0.0.1:8080, OpenAI 호환 /v1/chat/completions 및 llama.cpp /completion) 스트리밍"""
    base_url = LOCAL_LLM_URL.rstrip("/")
    chat_url = f"{base_url}/v1/chat/completions"
    payload = {
        "model": LOCAL_LLM_MODEL,
        "messages": [{"role": m["role"], "content": m["content"]} for m in messages if m.get("content")],
        "stream": True,
        "max_tokens": 512,
        "temperature": 0.7
    }
    print(f"[Local LLM] 127.0.0.1:8080 요청 시작: {chat_url}")
    
    def fetch_stream():
        req = urllib.request.Request(
            chat_url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                for line in resp:
                    if not line:
                        continue
                    line_str = line.decode("utf-8", errors="ignore").strip()
                    if line_str.startswith("data: "):
                        data_str = line_str[6:].strip()
                        if data_str == "[DONE]":
                            break
                        try:
                            cj = json.loads(data_str)
                            delta = cj.get("choices", [{}])[0].get("delta", {})
                            chunk = delta.get("content", "")
                            if chunk:
                                yield chunk
                        except Exception:
                            continue
        except urllib.error.HTTPError as he:
            if he.code == 404:
                # /v1/chat/completions가 404이면 llama.cpp 기본 엔드포인트 /completion 시도
                print("[Local LLM] /v1/chat/completions 404 -> /completion 엔드포인트로 폴백 시도")
                prompt_lines = [f"{m['role']}: {m['content']}" for m in messages if m.get("content")]
                prompt_lines.append("assistant:")
                comp_payload = {
                    "prompt": "\n".join(prompt_lines),
                    "stream": True,
                    "n_predict": 512
                }
                comp_req = urllib.request.Request(
                    f"{base_url}/completion",
                    data=json.dumps(comp_payload).encode("utf-8"),
                    headers={"Content-Type": "application/json"}
                )
                with urllib.request.urlopen(comp_req, timeout=30) as c_resp:
                    for c_line in c_resp:
                        if not c_line:
                            continue
                        c_str = c_line.decode("utf-8", errors="ignore").strip()
                        if c_str.startswith("data: "):
                            try:
                                c_json = json.loads(c_str[6:].strip())
                                c_text = c_json.get("content", "")
                                if c_text:
                                    yield c_text
                            except Exception:
                                continue
            else:
                raise

    try:
        loop = asyncio.get_running_loop()
        queue = asyncio.Queue()

        def worker():
            try:
                for chk in fetch_stream():
                    asyncio.run_coroutine_threadsafe(queue.put(chk), loop).result()
            except Exception as err:
                print(f"[Local LLM 에러] {err}")
            finally:
                asyncio.run_coroutine_threadsafe(queue.put(None), loop).result()

        threading.Thread(target=worker, daemon=True).start()

        has_any_token = False
        while True:
            item = await queue.get()
            if item is None:
                break
            has_any_token = True
            yield item

        if not has_any_token:
            print("[Local LLM 경고] 수신된 토큰 없음 -> Gemini로 폴백")
            async for c in stream_gemini(messages, image_bytes=image_bytes):
                yield c

    except Exception as e:
        print(f"[Local LLM 예외] {e} -> Gemini로 대체 실행")
        async for c in stream_gemini(messages, image_bytes=image_bytes):
            yield c

# ==========================================
# 2-3. Gemini Realtime 세션 (Multimodal Live API, STT/TTS 생략 직통)
# ==========================================
class GeminiLiveSession:
    """Gemini Multimodal Live WebSocket 세션: STT/TTS 없이 음성을 즉시 실시간 송수신"""
    def __init__(self, esp_ws, api_key, model=GEMINI_REALTIME_MODEL, voice=GEMINI_REALTIME_VOICE):
        self.esp_ws = esp_ws
        self.api_key = api_key
        self.model = model
        self.voice = voice
        self.gemini_ws = None
        self.rx_task = None
        self.is_connected = False
        self.system_prompt = (
            "너의 이름은 '친구'야. 너는 음성으로 대화하는 친절하고 다정한 한국어 AI 비서야. "
            "절대 마크다운 기호(*, #, -)나 이모지를 쓰지 말고, 자연스러운 한국어 구어체로 두세 문장 이내로 간결하게 답해줘."
        )

    async def start(self):
        ws_url = (
            f"wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1alpha.GenerativeService.BidiGenerateContent"
            f"?key={self.api_key}"
        )
        print(f"[Gemini Realtime] WebSocket 연결 시도 (model: {self.model}, voice: {self.voice})...")
        try:
            self.gemini_ws = await websockets.connect(ws_url, max_size=10 * 1024 * 1024)
            setup_msg = {
                "setup": {
                    "model": f"models/{self.model}",
                    "generationConfig": {
                        "responseModalities": ["AUDIO"],
                        "speechConfig": {
                            "voiceConfig": {
                                "prebuiltVoiceConfig": {
                                    "voiceName": self.voice
                                }
                            }
                        }
                    },
                    "systemInstruction": {
                        "parts": [{"text": self.system_prompt}]
                    }
                }
            }
            await self.gemini_ws.send(json.dumps(setup_msg))
            self.is_connected = True
            print("[Gemini Realtime] 세션 Setup 완료 (STT/TTS 없는 직통 음성 스트리밍 대기)")
            self.rx_task = asyncio.create_task(self._receive_loop())
        except Exception as e:
            print(f"[Gemini Realtime] 연결 실패: {e}")
            self.is_connected = False

    async def send_audio(self, pcm_chunk_16k):
        """ESP32의 16kHz PCM 데이터를 Gemini로 즉각 전송"""
        if not self.is_connected or not self.gemini_ws:
            return
        try:
            b64_pcm = base64.b64encode(pcm_chunk_16k).decode("utf-8")
            msg = {
                "realtimeInput": {
                    "mediaChunks": [
                        {
                            "mimeType": "audio/pcm;rate=16000",
                            "data": b64_pcm
                        }
                    ]
                }
            }
            await self.gemini_ws.send(json.dumps(msg))
        except Exception as e:
            print(f"[Gemini Realtime] 오디오 송신 오류: {e}")

    async def _receive_loop(self):
        """Gemini로부터 오는 24kHz 오디오를 16kHz로 리샘플링하여 ESP32로 즉시 바이너리 전송"""
        try:
            first_chunk = True
            async for raw in self.gemini_ws:
                data = json.loads(raw)
                server_content = data.get("serverContent")
                if not server_content:
                    continue

                # 사용자가 끼어들었음을 Gemini 서버가 감지했을 때
                if server_content.get("interrupted"):
                    print("\n[Gemini Realtime] 사용자 끼어들기 감지(interrupted) -> 스피커 플러시")
                    await self.esp_ws.send(json.dumps({"type": "interrupt", "message": "CLEAR_AUDIO"}))
                    first_chunk = True
                    continue

                model_turn = server_content.get("modelTurn")
                if model_turn:
                    for part in model_turn.get("parts", []):
                        inline = part.get("inlineData")
                        if inline and inline.get("mimeType", "").startswith("audio/pcm"):
                            b64_pcm = inline.get("data", "")
                            if b64_pcm:
                                pcm24 = base64.b64decode(b64_pcm)
                                pcm16 = resample_24k_to_16k(pcm24)
                                if first_chunk:
                                    first_chunk = False
                                    await self.esp_ws.send(json.dumps({"type": "status", "message": "SPEAKING..."}))
                                await self.esp_ws.send(pcm16)

                if server_content.get("turnComplete"):
                    first_chunk = True
                    await self.esp_ws.send(json.dumps({"type": "status", "message": "DONE", "close_session": False}))

        except asyncio.CancelledError:
            pass
        except Exception as e:
            print(f"[Gemini Realtime] 수신 루프 오류: {e}")
        finally:
            self.is_connected = False

    async def close(self):
        self.is_connected = False
        if self.rx_task and not self.rx_task.done():
            self.rx_task.cancel()
            try:
                await self.rx_task
            except asyncio.CancelledError:
                pass
        if self.gemini_ws:
            try:
                await self.gemini_ws.close()
            except Exception:
                pass
            self.gemini_ws = None

# ==========================================
# 2-4. ChatGPT Realtime 세션 (OpenAI Realtime API, STT/TTS 생략 직통)
# ==========================================
class OpenAIRealtimeSession:
    """OpenAI Realtime WebSocket 세션: STT/TTS 없이 음성을 즉시 실시간 송수신"""
    def __init__(self, esp_ws, api_key, model=OPENAI_REALTIME_MODEL, voice=OPENAI_VOICE):
        self.esp_ws = esp_ws
        self.api_key = api_key
        self.model = model
        self.voice = voice
        self.openai_ws = None
        self.rx_task = None
        self.is_connected = False
        self.system_prompt = (
            "너의 이름은 '친구'야. 너는 음성으로 대화하는 친절하고 다정한 한국어 AI 비서야. "
            "절대 마크다운 기호(*, #, -)나 이모지를 쓰지 말고, 자연스러운 한국어 구어체로 두세 문장 이내로 간결하게 대답해."
        )

    async def start(self):
        if not self.api_key:
            print("[OpenAI Realtime] OPENAI_API_KEY가 설정되지 않아 연결할 수 없습니다. server/.env에 설정해 주세요.")
            await self.esp_ws.send(json.dumps({
                "type": "status",
                "message": "ERROR: OPENAI_API_KEY missing"
            }))
            return

        ws_url = f"wss://api.openai.com/v1/realtime?model={self.model}"
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "OpenAI-Beta": "realtime=v1"
        }
        print(f"[OpenAI Realtime] WebSocket 연결 시도 (model: {self.model}, voice: {self.voice})...")
        try:
            self.openai_ws = await websockets.connect(
                ws_url,
                extra_headers=headers,
                max_size=10 * 1024 * 1024
            )
            session_update = {
                "type": "session.update",
                "session": {
                    "modalities": ["audio", "text"],
                    "instructions": self.system_prompt,
                    "voice": self.voice,
                    "input_audio_format": "pcm16",
                    "output_audio_format": "pcm16",
                    "turn_detection": {
                        "type": "server_vad",
                        "threshold": 0.5,
                        "prefix_padding_ms": 300,
                        "silence_duration_ms": 500
                    }
                }
            }
            await self.openai_ws.send(json.dumps(session_update))
            self.is_connected = True
            print("[OpenAI Realtime] 세션 Setup 완료 (STT/TTS 없는 직통 음성 스트리밍 대기)")
            self.rx_task = asyncio.create_task(self._receive_loop())
        except Exception as e:
            print(f"[OpenAI Realtime] 연결 실패: {e}")
            self.is_connected = False

    async def send_audio(self, pcm_chunk_16k):
        """ESP32의 16kHz PCM을 24kHz PCM으로 업샘플링하여 OpenAI로 전송"""
        if not self.is_connected or not self.openai_ws:
            return
        try:
            pcm24 = resample_16k_to_24k(pcm_chunk_16k)
            b64_audio = base64.b64encode(pcm24).decode("utf-8")
            msg = {
                "type": "input_audio_buffer.append",
                "audio": b64_audio
            }
            await self.openai_ws.send(json.dumps(msg))
        except Exception as e:
            print(f"[OpenAI Realtime] 오디오 송신 오류: {e}")

    async def _receive_loop(self):
        """OpenAI로부터 오는 24kHz 오디오를 16kHz로 다운샘플링하여 ESP32에 전송"""
        try:
            first_chunk = True
            async for raw in self.openai_ws:
                event = json.loads(raw)
                event_type = event.get("type", "")

                # 1. 발화 시작(Barge-in / OpenAI Server VAD 감지)
                if event_type == "input_audio_buffer.speech_started":
                    print("\n[OpenAI Realtime] 사용자 발화 감지(speech_started) -> 스피커 플러시")
                    await self.esp_ws.send(json.dumps({"type": "interrupt", "message": "CLEAR_AUDIO"}))
                    first_chunk = True

                # 2. 오디오 스트리밍 델타 수신
                elif event_type == "response.audio.delta":
                    delta_b64 = event.get("delta", "")
                    if delta_b64:
                        pcm24 = base64.b64decode(delta_b64)
                        pcm16 = resample_24k_to_16k(pcm24)
                        if first_chunk:
                            first_chunk = False
                            await self.esp_ws.send(json.dumps({"type": "status", "message": "SPEAKING..."}))
                        await self.esp_ws.send(pcm16)

                # 3. 텍스트 트랜스크립트 델타 (디버그 로그)
                elif event_type == "response.audio_transcript.delta":
                    txt = event.get("delta", "")
                    print(txt, end="", flush=True)

                # 4. 발화 완료
                elif event_type == "response.audio.done":
                    print("\n[OpenAI Realtime] AI 발화 완료")
                    first_chunk = True
                    await self.esp_ws.send(json.dumps({"type": "status", "message": "DONE", "close_session": False}))

        except asyncio.CancelledError:
            pass
        except Exception as e:
            print(f"[OpenAI Realtime] 수신 루프 오류: {e}")
        finally:
            self.is_connected = False

    async def close(self):
        self.is_connected = False
        if self.rx_task and not self.rx_task.done():
            self.rx_task.cancel()
            try:
                await self.rx_task
            except asyncio.CancelledError:
                pass
        if self.openai_ws:
            try:
                await self.openai_ws.close()
            except Exception:
                pass
            self.openai_ws = None

# ==========================================
# 2-5. 통합 LLM 스트리밍 디스패처 (텍스트 기반)
# ==========================================
async def stream_llm(messages, image_bytes=None, llm_type="gemini api"):
    """ESP32에서 선택한 LLM Type에 따라 분기 스트리밍 (텍스트/TTS 모드)"""
    clean_type = (llm_type or "gemini api").strip().lower()
    print(f"[LLM 엔진 선택] 현재 활성화된 LLM Type: '{clean_type}'")
    if clean_type == "local llm":
        async for chunk in stream_local_llm(messages, image_bytes=image_bytes):
            yield chunk
    else:  # "gemini api", "gemini realtime api", "chatgpt realtime api" 등 텍스트 fallback
        async for chunk in stream_gemini(messages, image_bytes=image_bytes):
            yield chunk

VISION_KEYWORDS = (
    "앞에", "보이는", "보여", "보이니", "보이나요", "보여요", "이거", "이게", "사진", "카메라", "찍어",
    "봐봐", "봐줘", "물건", "뭐야", "무엇", "글자", "읽어줘", "어떤",
    "풍경", "모습", "색깔", "색상", "표정", "누구", "사람", "인식", "시각",
    "촬영", "화면", "영상", "비춰", "비춰줘", "보여줘", "봐", "찍어줘",
    "책", "얼굴", "사물", "앞", "이 물건", "이 책", "이 글", "이 그림"
)

def is_vision_query(text):
    """사용자 발화에 시각적 분석이 필요한 키워드가 포함되어 있는지 판정"""
    if not text:
        return False
    return any(k in text for k in VISION_KEYWORDS)

SESSION_END_KEYWORDS = (
    "그만 대화", "대화 그만", "그만하자", "그만할래", "그만할게",
    "대화 끝", "대화 종료", "이제 그만", "끝내자", "끝낼게",
    "그만 이야기", "그만 얘기", "바이바이", "잘 가", "들어가",
    "대화 멈춰", "종료해줘", "종료하자", "대화 중단", "대화 닫아",
    "그만들을래", "그만들을게"
)

def is_session_end_query(text):
    """사용자가 대화를 중단/종료하고자 하는 의도인지 판정"""
    if not text:
        return False
    no_space = text.replace(" ", "")
    for k in SESSION_END_KEYWORDS:
        if k in text or k.replace(" ", "") in no_space:
            return True
    return False

def is_meaningful_speech(text):
    """TV 소음, 비언어 잡음, 괄호 환각, 무의미한 단발성 소리(기침, 헛기침 등)를 필터링하여 유효한 사용자의 발화인지 판정"""
    if not text:
        return False
    stripped = text.strip()
    if not stripped:
        return False
    
    # 1. 비언어 잡음 또는 괄호 환각: (음악), [박수], (기침), [재채기], (아과쉬...), etc.
    if (stripped.startswith("(") and stripped.endswith(")")) or \
       (stripped.startswith("[") and stripped.endswith("]")):
        return False
    
    # 2. 한글, 영문, 숫자 외 기호 제거
    alphanumeric = re.sub(r'[^가-힣a-zA-Z0-9]', '', stripped)
    # 의미 있는 단음절 대답(네, 응, 예, 왜, 뭐 등)은 허용하되, 단순 추임새나 목가다듬기 소리("어", "음", "아")는 단독 발화로 인정하지 않음
    meaningful_single_words = {"네", "응", "예", "아니", "왜", "뭐"}
    if len(alphanumeric) < 2 and alphanumeric not in meaningful_single_words:
        return False
    
    # 3. 기침 소리, 목 가다듬기, 재채기, 웃음 등 비언어 소음 필터링
    clean_check = stripped.replace("-", "").replace(".", "").replace(" ", "").strip()
    NOISE_WORDS = {
        "기침", "기침소리", "콜록", "콜록콜록", "에취", "에헴", "켁", "헛기침",
        "숨소리", "한숨", "하품", "웃음", "웃음소리", "박수", "소리", "음", "어", "아",
        "쿨럭", "쿨럭쿨럭", "컥", "커억", "흠", "흠흠", "켁켁", "켈록"
    }
    if clean_check in NOISE_WORDS:
        return False

    # 4. Whisper 대표 환각 어구 필터링
    HALLUCINATIONS = [
        "감사합니다", "고맙습니다", "시청해주셔서감사합니다", 
        "시청해주셔서고맙습니다", "MBC뉴스", "KBS뉴스", "SBS뉴스", 
        "뉴스", "끝", "구독과좋아요", "구독", "좋아요"
    ]
    if clean_check in HALLUCINATIONS:
        return False
        
    return True

# ==========================================
# 3. LLM 응답 및 PCM 오디오 전송 파이프라인
# ==========================================
async def synthesize_tts_pcm(text, max_retries=2):
    """edge_tts 합성 및 PCM 변환 (간헐적 네트워크 오류 시 1회 재시도)"""
    for attempt in range(max_retries):
        try:
            communicate = edge_tts.Communicate(text, VOICE)
            mp3_buffer = bytearray()
            async for audio_chunk in communicate.stream():
                if audio_chunk["type"] == "audio":
                    mp3_buffer.extend(audio_chunk["data"])
            if mp3_buffer:
                pcm_data = await convert_mp3_to_pcm(bytes(mp3_buffer))
                if pcm_data:
                    return pcm_data
        except Exception as e:
            if attempt < max_retries - 1:
                await asyncio.sleep(0.1)
            else:
                print(f"\n[경고] TTS 합성 최종 실패 ('{text}'): {e}")
    return None

def calculate_remaining_sentences(sentences_info, played_bytes):
    """
    ESP32 스피커에서 실제 재생된 바이트 수(played_bytes)를 기준으로,
    사용자가 아직 듣지 못했거나 듣는 도중 중단된 문장들을 정확히 추출합니다.
    """
    if not sentences_info:
        return []
    
    cum_bytes = 0
    remaining = []
    
    for idx, entry in enumerate(sentences_info):
        pcm = entry.get("pcm")
        pcm_len = len(pcm) if pcm else 0
        if pcm_len == 0 and entry.get("text"):
            rem_entry = dict(entry)
            rem_entry["status"] = "pending"
            remaining.append(rem_entry)
            continue
            
        start_byte = cum_bytes
        end_byte = cum_bytes + pcm_len
        cum_bytes = end_byte
        
        rem_entry = dict(entry)
        rem_entry["status"] = "pending"
        
        # 1. 이 문장이 시작되기 전에 끼어들기가 발생한 경우 -> 완전히 미재생 문장
        if played_bytes <= start_byte:
            remaining.append(rem_entry)
        # 2. 이 문장 재생 도중에 끼어들기가 발생한 경우
        elif start_byte < played_bytes < end_byte:
            played_in_sent = played_bytes - start_byte
            # 문장의 65% 미만만 들었다면 문장의 핵심을 놓쳤으므로 이 문장부터 다시 재생
            if played_in_sent < 0.65 * pcm_len:
                remaining.append(rem_entry)
            # 65% 이상 들었더라도 뒤에 남은 문장이 없다면 마지막 문장을 다시 들려줌
            elif idx == len(sentences_info) - 1:
                remaining.append(rem_entry)
        # 3. 이 문장을 완전히 다 들은 경우 (played_bytes >= end_byte) -> 이미 청취 완료되었으므로 제외
        else:
            continue
            
    # 혹시 남은 문장이 계산되지 않았는데 끼어들기가 들어온 경우 마지막 문장 복원
    if not remaining and sentences_info:
        last_entry = dict(sentences_info[-1])
        last_entry["status"] = "pending"
        remaining.append(last_entry)
        
    return remaining

async def process_ai_response(websocket, user_text, chat_history, t_speech_end, asr_ms=0.0, close_session_after=False, interrupted_holder=None, image_bytes=None, llm_type="gemini api"):
    full_answer = ""
    sentences_info = []  # [{"text": str, "pcm": bytes, "status": "pending"|"sending"|"done"}]
    sentence_buffer = ""
    t_playback_start = None
    if interrupted_holder is not None:
        interrupted_holder["active_ai_response"] = {
            "user_query": user_text,
            "sentences": sentences_info,
            "full_answer": "",
            "close_session_after": close_session_after,
            "was_thinking_only": True,
            "t_playback_start": None
        }
    try:
        img_info = f" (+이미지 {len(image_bytes) // 1024}KB)" if image_bytes else ""
        print(f"\n[LLM 질의] {user_text}{img_info} (Type: {llm_type})")
        # AI 답변 생성 시작: ESP32 상태를 THINKING으로 즉시 전환하고 마이크 스트리밍을 중단시킴
        await websocket.send(json.dumps({"type": "status", "message": "THINKING..."}))
        
        chat_history.append({"role": "user", "content": user_text})
        recent_messages = [chat_history[0]] + chat_history[-12:]

        t_llm_start = time.perf_counter()
        t_first_token = None
        t_first_sentence = None

        # 지정된 LLM Type에 따른 실시간 스트리밍
        response = stream_llm(recent_messages, image_bytes=image_bytes, llm_type=llm_type)

        is_first_chunk = True
        pcm_queue = asyncio.Queue()
        tts_queue = asyncio.Queue()

        async def audio_sender():
            nonlocal t_playback_start
            chunk_len = 2048  # 64ms 분량
            try:
                first_sent = True
                while True:
                    entry = await pcm_queue.get()
                    if entry is None:
                        pcm_queue.task_done()
                        break
                    
                    # 첫 오디오 청크 전송 시 ESP32 화면을 THINKING -> SPEAKING으로 즉시 전환
                    if first_sent:
                        t_playback_start = time.perf_counter()
                        if interrupted_holder is not None and interrupted_holder.get("active_ai_response"):
                            interrupted_holder["active_ai_response"]["t_playback_start"] = t_playback_start
                        await websocket.send(json.dumps({"type": "status", "message": "SPEAKING..."}))

                    entry["status"] = "sending"
                    pcm_data = entry["pcm"]

                    # 첫 문장의 처음 4개 청크(약 256ms)는 ESP32 스피커 버퍼 즉각 워밍업을 위해 노딜레이 전송
                    # 이후에는 58ms 간격으로 전송 (64ms 재생시간에 맞춰 58ms로 전송)
                    # 재생 속도와 1:1로 정확히 동기화되어 256KB 링버퍼 오버플로우가 원천 방지되며,
                    # 긴 답변에서도 음성이 한꺼번에 뭉개지거나 드롭되지 않습니다.
                    for idx, i in enumerate(range(0, len(pcm_data), chunk_len)):
                        await websocket.send(pcm_data[i:i+chunk_len])
                        if first_sent and idx < 4:
                            continue
                        await asyncio.sleep(0.058)
                    
                    entry["status"] = "done"
                    first_sent = False
                    pcm_queue.task_done()
            except asyncio.CancelledError:
                pass

        async def tts_worker():
            try:
                while True:
                    item = await tts_queue.get()
                    if item is None:
                        await pcm_queue.put(None)
                        tts_queue.task_done()
                        break
                    clean_sent, entry = item
                    pcm_data = await synthesize_tts_pcm(clean_sent)
                    if pcm_data:
                        entry["pcm"] = pcm_data
                        await pcm_queue.put(entry)
                    tts_queue.task_done()
            except asyncio.CancelledError:
                pass

        sender_task = asyncio.create_task(audio_sender())
        tts_worker_task = asyncio.create_task(tts_worker())

        def extract_tts_chunk(buf, is_first):
            stripped = buf.strip()
            # 1. 완전한 문장 종결 부호 (. ? ! \n)가 있으면 즉시 문장 분리 (최소 2자 이상)
            for i, ch in enumerate(buf):
                if ch in [".", "?", "!", "\n"]:
                    candidate = buf[:i+1]
                    if len(candidate.strip()) >= 2:
                        return candidate, buf[i+1:]

            # 2. 첫 문장(is_first==True): 첫 응답 음성 도착(TTFA)을 1초대로 단축하기 위해
            #    쉼표(", ")나 자연스러운 연결 어미("요 ", "죠 ", "다 ", "고 ")에서 최소 3자 이상이면 신속 분리!
            if is_first:
                first_delimiters = [", ", "요 ", "죠 ", "다 ", "고 ", "는데 ", "어서 "]
                if len(stripped) >= 3:
                    for d in first_delimiters:
                        pos = buf.find(d)
                        if pos != -1 and (pos + len(d)) >= 3:
                            return buf[:pos + len(d)], buf[pos + len(d):]

            # 3. 이후 문장(is_first==False): 중간 및 뒷부분 문장이 파편화되거나 단어 중간이 잘리는 것을 방지!
            #    완전한 문장 종결 부호가 기본이며, 문장이 35자 이상 아주 길어질 때만 자연스러운 연결 어미에서 안전하게 분리
            else:
                if len(stripped) >= 35:
                    sub_delimiters = [", ", "는데 ", "어서 ", "으면 ", "지만 "]
                    for d in sub_delimiters:
                        pos = buf.find(d)
                        if pos != -1 and (pos + len(d)) >= 15:
                            return buf[:pos + len(d)], buf[pos + len(d):]

            return None, buf

        print("[AI 답변] ", end="", flush=True)
        async for text in response:
            if text:
                print(text, end="", flush=True)

                if t_first_token is None:
                    t_first_token = time.perf_counter()
                
                full_answer += text
                sentence_buffer += text
                if interrupted_holder is not None and interrupted_holder.get("active_ai_response"):
                    interrupted_holder["active_ai_response"]["full_answer"] = full_answer
                    interrupted_holder["active_ai_response"]["was_thinking_only"] = False
                
                while True:
                    complete_sent, remaining = extract_tts_chunk(sentence_buffer, is_first_chunk)
                    if complete_sent:
                        clean_sent = clean_text_for_tts(complete_sent)
                        if clean_sent and len(re.sub(r'[^가-힣a-zA-Z0-9]', '', clean_sent)) >= 1:
                            if t_first_sentence is None:
                                t_first_sentence = time.perf_counter()
                            entry = {"text": clean_sent, "pcm": None, "status": "pending"}
                            sentences_info.append(entry)
                            await tts_queue.put((clean_sent, entry))
                            is_first_chunk = False
                        sentence_buffer = remaining
                    else:
                        break

        llm_ttft_ms = ((t_first_token or time.perf_counter()) - t_llm_start) * 1000
        llm_sent_ms = ((t_first_sentence or time.perf_counter()) - t_llm_start) * 1000
        llm_gen_ms = (time.perf_counter() - t_llm_start) * 1000
        print(f"\n[LLM 속도] 첫 토큰: {llm_ttft_ms:.0f}ms | 첫 문장: {llm_sent_ms:.0f}ms | 전체 완성: {llm_gen_ms:.0f}ms")

        # 남은 잔여 버퍼 처리 (마지막 문장)
        if sentence_buffer.strip():
            clean_sent = clean_text_for_tts(sentence_buffer)
            if clean_sent and len(re.sub(r'[^가-힣a-zA-Z0-9]', '', clean_sent)) >= 1:
                entry = {"text": clean_sent, "pcm": None, "status": "pending"}
                sentences_info.append(entry)
                await tts_queue.put((clean_sent, entry))
            sentence_buffer = ""

        # TTS 작업 완료 및 모든 오디오 전송이 완료될 때까지 대기
        await tts_queue.put(None)
        await tts_worker_task
        await sender_task
                    
        chat_history.append({"role": "assistant", "content": full_answer})
        if interrupted_holder is not None:
            # 전송이 끝난 직후라도 ESP32 스피커에서 문장들이 여전히 재생 중일 수 있음!
            # 재생 중 기침/소음 등으로 끼어들기 발생 시 정확한 진도율 복원을 위해 전체 sentences와 t_playback_start 보관
            interrupted_holder["last_completed_session"] = {
                "user_query": user_text,
                "sentences": list(sentences_info),
                "full_answer": full_answer,
                "close_session_after": close_session_after,
                "was_thinking_only": False,
                "completed_at": time.perf_counter(),
                "t_playback_start": t_playback_start
            }
            interrupted_holder["active_ai_response"] = None
            # 이미 끼어들기가 발생해 session이 설정되어 있는 경우 덮어써서 지우지 않음!
            if interrupted_holder.get("session") is None:
                interrupted_holder["session"] = None

        # 끼어들기 세션이 이미 저장되어 있지 않은 정상 완료 시에만 DONE 전송
        if interrupted_holder is None or interrupted_holder.get("session") is None:
            print(f"[서버] AI 답변 전송 완료 (close_session={close_session_after}) -> DONE 전송 (클라이언트 스피커 재생 완료 대기)")
            await websocket.send(json.dumps({
                "type": "status",
                "message": "DONE",
                "close_session": close_session_after
            }))
        else:
            print(f"[서버] AI 답변 전송 완료 시점에 이미 끼어들기 감지됨 -> DONE 전송 생략 및 이어말하기 세션 유지")

    except asyncio.CancelledError:
        print(f"\n[Barge-in 감지] 이전 AI 발화 중단 및 오디오 플러시 완료")
        if 'tts_worker_task' in locals() and not tts_worker_task.done():
            tts_worker_task.cancel()
            try:
                await tts_worker_task
            except asyncio.CancelledError:
                pass
        if 'sender_task' in locals() and not sender_task.done():
            sender_task.cancel()
            try:
                await sender_task
            except asyncio.CancelledError:
                pass
        
        # 미처리된 LLM 잔여 버퍼가 있다면 문장 정보에 추가 및 합성
        if sentence_buffer.strip():
            clean_rem = clean_text_for_tts(sentence_buffer)
            if clean_rem and len(re.sub(r'[^가-힣a-zA-Z0-9]', '', clean_rem)) >= 1:
                entry = {"text": clean_rem, "pcm": None, "status": "pending"}
                try:
                    rem_pcm = await synthesize_tts_pcm(clean_rem)
                    if rem_pcm:
                        entry["pcm"] = rem_pcm
                except Exception:
                    pass
                sentences_info.append(entry)

        # 재생 경과 시간 기반으로 미청취 문장 산출
        if t_playback_start:
            est_elapsed = max(0.0, time.perf_counter() - t_playback_start - 0.1)
            est_played_bytes = int(est_elapsed * 32000)
        else:
            est_played_bytes = 0
            
        remaining_entries = calculate_remaining_sentences(sentences_info, est_played_bytes)

        # 아직 TTS 합성되지 않은 문장이 있다면 합성
        for entry in remaining_entries:
            if entry.get("pcm") is None and entry.get("text"):
                try:
                    syn_pcm = await synthesize_tts_pcm(entry["text"])
                    if syn_pcm:
                        entry["pcm"] = syn_pcm
                except Exception:
                    pass

        if interrupted_holder is not None:
            if remaining_entries:
                interrupted_holder["session"] = {
                    "user_query": user_text,
                    "sentences": list(sentences_info),
                    "remaining_entries": remaining_entries,
                    "full_answer": full_answer,
                    "close_session_after": close_session_after,
                    "was_thinking_only": False,
                    "completed_at": time.perf_counter(),
                    "t_playback_start": t_playback_start
                }
                interrupted_holder["last_completed_session"] = interrupted_holder["session"]
                rem_texts = " / ".join([e["text"] for e in remaining_entries])
                print(f"[서버] 음성 끼어들기로 중단됨: 남은 {len(remaining_entries)}개 문장 보관 완료 (이어말하기 대기): {rem_texts}")
            elif len(sentences_info) == 0:
                # 첫 문장 생성 도중(THINKING 중) 끼어들기 발생
                interrupted_holder["session"] = {
                    "user_query": user_text,
                    "sentences": [],
                    "remaining_entries": [],
                    "full_answer": "",
                    "close_session_after": close_session_after,
                    "was_thinking_only": True,
                    "completed_at": time.perf_counter(),
                    "t_playback_start": None
                }
                interrupted_holder["last_completed_session"] = interrupted_holder["session"]
                print(f"[서버] THINKING 중 끼어들기 발생: 이전 질문('{user_text}') 보관 완료")
            else:
                interrupted_holder["session"] = None
        if full_answer.strip():
            chat_history.append({"role": "assistant", "content": full_answer.strip()})
        raise
    except Exception as e:
        print(f"\n[오류] AI 처리 중 에러: {e}")
        if 'tts_worker_task' in locals() and not tts_worker_task.done():
            tts_worker_task.cancel()
            try:
                await tts_worker_task
            except asyncio.CancelledError:
                pass
        if 'sender_task' in locals() and not sender_task.done():
            sender_task.cancel()
            try:
                await sender_task
            except asyncio.CancelledError:
                pass
        
        err_msg = "인공지능 서버 연결에 실패했습니다."
        try:
            # 1. ESP32 화면에 에러 상태 표시
            await websocket.send(json.dumps({"type": "status", "message": "LLM ERROR"}))
            # 2. 에러 안내 음성 합성 및 전송 시도
            err_pcm = await synthesize_tts_pcm(err_msg, max_retries=1)
            if err_pcm:
                chunk_len = 2048
                for i in range(0, len(err_pcm), chunk_len):
                    await websocket.send(err_pcm[i:i+chunk_len])
                    await asyncio.sleep(0.060)
        except Exception as tts_err:
            print(f"[오류] 에러 안내 음성 전송 실패: {tts_err}")
        finally:
            # 에러 발생 시에도 반드시 DONE을 전송하여 ESP32가 무한 대기하지 않고 대화 상태로 복귀 보장!
            await websocket.send(json.dumps({
                "type": "status",
                "message": "DONE",
                "close_session": close_session_after
            }))

async def resume_ai_response(websocket, session_data, chat_history, interrupted_holder=None):
    """TV 소음/잡음 등으로 끼어들기가 오인식되었을 때 직전 AI 답변의 남은 부분을 이어서 말함"""
    try:
        if session_data.get("was_thinking_only"):
            user_query = session_data.get("user_query", "")
            print(f"[서버] THINKING 중단 복구: 이전 질문('{user_query}')으로 다시 답변 시작")
            await process_ai_response(
                websocket, user_query, chat_history, time.perf_counter(),
                asr_ms=0.0, close_session_after=session_data.get("close_session_after", False),
                interrupted_holder=interrupted_holder
            )
            return

        remaining_entries = session_data.get("remaining_entries", [])
        if not remaining_entries:
            print("[서버] 이어말할 남은 문장이 없습니다 -> DONE 전송")
            await websocket.send(json.dumps({
                "type": "status",
                "message": "DONE",
                "close_session": session_data.get("close_session_after", False)
            }))
            return

        remaining_text = " ".join([e["text"] for e in remaining_entries])
        print(f"\n[AI 이어말하기] {remaining_text}")
        await websocket.send(json.dumps({"type": "status", "message": "SPEAKING..."}))

        t_playback_start = None
        if interrupted_holder is not None:
            interrupted_holder["active_ai_response"] = {
                "user_query": session_data.get("user_query", ""),
                "sentences": remaining_entries,
                "full_answer": session_data.get("full_answer", ""),
                "close_session_after": session_data.get("close_session_after", False),
                "was_thinking_only": False,
                "t_playback_start": None
            }

        pcm_queue = asyncio.Queue()

        async def audio_sender():
            nonlocal t_playback_start
            chunk_len = 2048
            try:
                first_sent = True
                while True:
                    entry = await pcm_queue.get()
                    if entry is None:
                        pcm_queue.task_done()
                        break
                    if first_sent:
                        t_playback_start = time.perf_counter()
                        if interrupted_holder is not None and interrupted_holder.get("active_ai_response"):
                            interrupted_holder["active_ai_response"]["t_playback_start"] = t_playback_start
                    entry["status"] = "sending"
                    pcm_data = entry["pcm"]
                    for idx, i in enumerate(range(0, len(pcm_data), chunk_len)):
                        await websocket.send(pcm_data[i:i+chunk_len])
                        if first_sent and idx < 4:
                            continue
                        await asyncio.sleep(0.058)
                    entry["status"] = "done"
                    first_sent = False
                    pcm_queue.task_done()
            except asyncio.CancelledError:
                pass

        sender_task = asyncio.create_task(audio_sender())

        for entry in remaining_entries:
            entry["status"] = "pending"
            await pcm_queue.put(entry)

        await pcm_queue.put(None)
        await sender_task

        print(f"[서버] AI 이어말하기 완료 -> DONE 전송")
        if interrupted_holder is not None:
            interrupted_holder["last_completed_session"] = {
                "user_query": session_data.get("user_query", ""),
                "sentences": list(remaining_entries),
                "full_answer": session_data.get("full_answer", ""),
                "close_session_after": session_data.get("close_session_after", False),
                "was_thinking_only": False,
                "completed_at": time.perf_counter(),
                "t_playback_start": t_playback_start
            }
            interrupted_holder["active_ai_response"] = None
            if interrupted_holder.get("session") is None:
                interrupted_holder["session"] = None

        if interrupted_holder is None or interrupted_holder.get("session") is None:
            await websocket.send(json.dumps({
                "type": "status",
                "message": "DONE",
                "close_session": session_data.get("close_session_after", False)
            }))
        else:
            print(f"[서버] AI 이어말하기 완료 시점에 이미 끼어들기 감지됨 -> DONE 전송 생략 및 세션 유지")

    except asyncio.CancelledError:
        print("\n[Barge-in 감지] 이어말하기 도중 다시 끼어들기 발생")
        if 'sender_task' in locals() and not sender_task.done():
            sender_task.cancel()
            try:
                await sender_task
            except asyncio.CancelledError:
                pass
        
        # 다시 끼어들기가 일어났을 때도 재생 시간 기반으로 남은 문장을 보관
        if t_playback_start:
            est_elapsed = max(0.0, time.perf_counter() - t_playback_start - 0.1)
            est_played_bytes = int(est_elapsed * 32000)
        else:
            est_played_bytes = 0
        remaining = calculate_remaining_sentences(remaining_entries, est_played_bytes)
        if interrupted_holder is not None and remaining:
            interrupted_holder["session"] = {
                "user_query": session_data.get("user_query", ""),
                "sentences": list(remaining_entries),
                "remaining_entries": remaining,
                "full_answer": session_data.get("full_answer", ""),
                "close_session_after": session_data.get("close_session_after", False),
                "was_thinking_only": False,
                "completed_at": time.perf_counter(),
                "t_playback_start": t_playback_start
            }
            interrupted_holder["last_completed_session"] = interrupted_holder["session"]
            rem_texts = " / ".join([e["text"] for e in remaining])
            print(f"[서버] 이어말하기 재중단: 남은 {len(remaining)}개 문장 재보관: {rem_texts}")
        raise
    except Exception as e:
        print(f"\n[오류] AI 이어말하기 중 에러: {e}")
        await websocket.send(json.dumps({
            "type": "status",
            "message": "DONE",
            "close_session": session_data.get("close_session_after", False)
        }))

# ==========================================
# 4. 메인 웹소켓 핸들러
# ==========================================
async def handler(websocket, path=None):
    print(f"\n[클라이언트 연결됨: {getattr(websocket, 'remote_address', 'ESP32')}]")
    
    audio_buffer = []
    is_speaking = False
    silence_time = 0.0
    ai_task = None
    barge_in_counter = 0
    t_last_whisper = 0.0
    t_speech_end = 0.0
    
    # TV 소음/잡음 등으로 끼어들기 발생 시 직전 답변을 이어서 말할 수 있도록 세션 보관용 객체
    interrupted_holder = {"session": None, "last_completed_session": None, "active_ai_response": None}

    # 앞 음절 손실 방지용 프리 버퍼 (100개 = 약 1,600ms 분량 보관)
    pre_speech_buffer = deque(maxlen=100)
    rx_accum = np.array([], dtype=np.float32)
    current_llm_type = "gemini api"

    active_realtime_session = None

    async def switch_realtime_session(target_type):
        nonlocal active_realtime_session
        if active_realtime_session:
            await active_realtime_session.close()
            active_realtime_session = None

        if target_type == "gemini realtime api":
            print("[세션 전환] Gemini Realtime (Live WebSocket) 세션 초기화 중...")
            active_realtime_session = GeminiLiveSession(websocket, GEMINI_API_KEY)
            await active_realtime_session.start()
        elif target_type == "chatgpt realtime api":
            print("[세션 전환] ChatGPT Realtime (OpenAI WebSocket) 세션 초기화 중...")
            active_realtime_session = OpenAIRealtimeSession(websocket, OPENAI_API_KEY)
            await active_realtime_session.start()
        else:
            print(f"[세션 전환] 일반 음성 처리 파이프라인(VAD/Whisper/TTS) 모드: {target_type}")

    today_str = time.strftime("%Y년 %m월 %d일")
    chat_history = [
        {
            "role": "system", 
            "content": (
                f"너의 이름은 '친구'야. 오늘 날짜는 {today_str}이야. "
                "너는 음성으로 대화하는 친절하고 똑똑한 한국어 AI 비서야. "
                "절대 마크다운 기호(*, #, -)나 이모지를 쓰지 말고, 사람과 말하듯 자연스러운 구어체 문장으로만 대답해. "
                "답변은 소리 내어 읽기 좋게 보통 두세 문장 이내로 간결하게 해. "
                "사진이 함께 주어지면 그것은 네 카메라로 지금 앞을 찍은 장면이니, 보이는 것을 바탕으로 자연스럽게 설명해. "
                "사용자가 이전에 한 말을 반드시 기억하고 문맥에 맞게 대답해. "
                "사용자가 '이제 그만하자', '대화 끝내자', '잘 가' 등 대화를 종료하자고 하면, '네, 좋은 하루 보내세요. 언제든 알렉사라고 불러주세요'처럼 다정하게 작별 인사를 한두 문장으로 해."
            )
        }
    ]

    pending_image_future = None
    t_last_user_request = 0.0

    async def handle_user_turn(user_text, asr_ms=0.0, close_session_after=False):
        nonlocal pending_image_future, t_last_user_request
        now = time.perf_counter()
        if now - t_last_user_request < 1.2:
            print(f"[서버 보호] 직전 요청과 간격이 너무 짧음({now - t_last_user_request:.2f}s < 1.2s) -> 중복 요청 무시")
            return
        t_last_user_request = now

        interrupted_holder["session"] = None
        interrupted_holder["last_completed_session"] = None
        interrupted_holder["active_ai_response"] = None

        should_close = close_session_after or is_session_end_query(user_text)
        if is_session_end_query(user_text):
            print(f"\n[대화 종료 의도 감지] '{user_text}' -> 작별 인사 후 세션 종료(Alexa 대기 상태 전환) 예약")

        image_bytes = None
        if not should_close and is_vision_query(user_text):
            print(f"\n[시각 질의 감지] '{user_text}' -> ESP32 카메라 촬영 요청 전송!")
            try:
                loop = asyncio.get_running_loop()
                pending_image_future = loop.create_future()
                await websocket.send(json.dumps({"type": "capture"}))
                image_bytes = await asyncio.wait_for(asyncio.shield(pending_image_future), timeout=3.5)
                if image_bytes:
                    print(f"[시각 질의 준비 완료] 사진 첨부하여 Gemini Vision 질의 시작 ({len(image_bytes)} bytes)")
            except asyncio.TimeoutError:
                print("[시각 질의] 카메라 사진 수신 시간 초과 -> 텍스트만으로 질의 진행")
            except Exception as e:
                print(f"[시각 질의] 사진 수신 예외: {e}")
            finally:
                pending_image_future = None

        await process_ai_response(
            websocket, user_text, chat_history,
            time.perf_counter(), asr_ms,
            close_session_after=should_close,
            interrupted_holder=interrupted_holder,
            image_bytes=image_bytes,
            llm_type=current_llm_type
        )

        if should_close:
            chat_history[:] = [chat_history[0]] if chat_history else []
            print("[대화 세션 종료] 대화 이력 초기화 완료 -> 다음 Alexa 호출 대기")

    try:
        async for message in websocket:
            try:
                # ------------------------------------------------------
                # (1) 바이너리 데이터: ESP32 마이크 PCM 음성 스트리밍 (16kHz, 16bit, Mono)
                # ------------------------------------------------------
                if isinstance(message, bytes):
                    # 카메라 JPEG 사진 감지 (JPEG SOI 매직 바이트: 0xFF 0xD8)
                    if len(message) > 4 and message[:2] == b"\xff\xd8":
                        print(f"\n[카메라 수신] 📷 ESP32로부터 JPEG 사진 수신 완료 ({len(message)} bytes)")
                        # [요구사항 3] LLM에 전송하는 사진도 시계방향으로 90도 회전
                        try:
                            import io
                            from PIL import Image
                            img = Image.open(io.BytesIO(message))
                            rotated_img = img.rotate(90, expand=True) # 시계 방향 90도 회전
                            buf = io.BytesIO()
                            rotated_img.save(buf, format="JPEG", quality=90)
                            message = buf.getvalue()
                            print(f"[카메라 회전] 🔄 시계 방향 90도 회전 완료 ({rotated_img.width}x{rotated_img.height}, {len(message)} bytes)")
                        except Exception as rot_err:
                            print(f"[카메라 회전 경고] PIL 회전 처리 예외 (원형 그대로 전송): {rot_err}")

                        if pending_image_future and not pending_image_future.done():
                            pending_image_future.set_result(message)
                        continue

                    # ★ Realtime API 모드 (Gemini Live / OpenAI Realtime):
                    # STT와 TTS를 일절 거치지 않고, 수신된 16kHz PCM 오디오를 Realtime API로 즉시 직통 스트리밍!
                    if current_llm_type in ("gemini realtime api", "chatgpt realtime api"):
                        if not active_realtime_session or not active_realtime_session.is_connected:
                            await switch_realtime_session(current_llm_type)
                        if active_realtime_session and active_realtime_session.is_connected:
                            await active_realtime_session.send_audio(message)
                        continue

                    # ★ 일반 모드 (gemini api / local llm): Silero VAD 및 로컬 Whisper STT / Edge-TTS
                    new_samples = np.frombuffer(message, dtype=np.int16).astype(np.float32) / 32768.0
                    rx_accum = np.concatenate([rx_accum, new_samples]) if len(rx_accum) > 0 else new_samples
                    
                    while len(rx_accum) >= 512:
                        audio_np = rx_accum[:512]
                        rx_accum = rx_accum[512:]

                        audio_scaled = audio_np * 4.0
                        audio_tensor = torch.from_numpy(audio_scaled)

                        vad_prob = vad_model(audio_tensor, SAMPLE_RATE).item()
                        
                        is_ai_busy = ai_task and not ai_task.done()
                        current_threshold = 0.65 if is_ai_busy else VAD_THRESHOLD
                        
                        if vad_prob > current_threshold:
                            silence_time = 0.0
                            barge_in_counter += 1
                            
                            # AI 답변 출력 중 끼어들기 감지:
                            # 잔여 스피커 음향 오작동 방지: 4프레임 연속 및 발화 종료 1.0초 이후에만 끼어들기 인정
                            time_since_speech_end = time.perf_counter() - t_speech_end
                            if is_ai_busy and barge_in_counter >= 4 and time_since_speech_end > 1.0:
                                print("\n[음성 끼어들기] AI 답변 일시 중단 (세션 보관)")
                                ai_task.cancel()
                                try:
                                    await ai_task
                                except asyncio.CancelledError:
                                    pass
                                await websocket.send(json.dumps({"type": "interrupt", "message": "CLEAR_AUDIO"}))
                                barge_in_counter = 0
                            
                            if not is_speaking and (not is_ai_busy or barge_in_counter >= 2):
                                print("\n[VAD] 🎤 발화 시작 감지")
                                is_speaking = True
                                # 새 발화 시작 시 프리 버퍼의 앞 음절들을 포함
                                audio_buffer = list(pre_speech_buffer)
                                pre_speech_buffer.clear()
                                
                            if is_speaking:
                                audio_buffer.append(audio_np)
                        else:
                            barge_in_counter = 0
                            if is_speaking:
                                audio_buffer.append(audio_np)
                                silence_time += (len(audio_np) / SAMPLE_RATE)
                                
                                if silence_time > SILENCE_LIMIT:
                                    t_speech_end = time.perf_counter()
                                    print("[VAD] 🔇 발화 종료 감지. whisper.cpp 변환 중...")
                                    is_speaking = False
                                    silence_time = 0.0
                                    
                                    if audio_buffer:
                                        trim_samples = int(SAMPLE_RATE * (SILENCE_LIMIT - 0.2))
                                        full_audio = np.concatenate(audio_buffer)
                                        if len(full_audio) > trim_samples:
                                            full_audio = full_audio[:-trim_samples]
                                        audio_buffer = []
                                        pre_speech_buffer.clear()
                                        
                                        user_text, asr_ms = await run_whisper_cpp(full_audio)
                                        t_last_whisper = time.perf_counter()
                                        print(f"[음성 인식 완료] {user_text or '(인식 실패 또는 무음)'} ({asr_ms:.0f}ms)")
                                        
                                        if is_meaningful_speech(user_text):
                                            ai_task = asyncio.create_task(
                                                handle_user_turn(user_text, asr_ms)
                                            )
                                        else:
                                            # TV 소리, 잡음, 무음 등 의미 있는 발화가 아님 -> 직전 답변 이어말하기 검토
                                            if interrupted_holder.get("session") is not None:
                                                saved_session = interrupted_holder["session"]
                                                interrupted_holder["session"] = None
                                                print(f"[서버] 의미 있는 발화 없음 ('{user_text}'). 직전 AI 답변 이어서 재생!")
                                                ai_task = asyncio.create_task(
                                                    resume_ai_response(websocket, saved_session, chat_history, interrupted_holder=interrupted_holder)
                                                )
                                            else:
                                                print("[서버] 인식된 텍스트가 없어 대기 상태(DONE)를 클라이언트로 전송합니다.")
                                                await websocket.send(json.dumps({"type": "status", "message": "DONE"}))
                                    else:
                                        audio_buffer = []
                                        if interrupted_holder.get("session") is not None:
                                            saved_session = interrupted_holder["session"]
                                            interrupted_holder["session"] = None
                                            print("[서버] 오디오 버퍼 없음 -> 직전 AI 답변 이어서 재생!")
                                            ai_task = asyncio.create_task(
                                                resume_ai_response(websocket, saved_session, chat_history, interrupted_holder=interrupted_holder)
                                            )
                                        else:
                                            await websocket.send(json.dumps({"type": "status", "message": "DONE"}))
                            else:
                                pre_speech_buffer.append(audio_np)

                # ------------------------------------------------------
                # (2) 텍스트 데이터: 시스템 제어 신호(JSON) 또는 사용자 텍스트 입력
                # ------------------------------------------------------
                elif isinstance(message, str):
                    raw_text = message.strip()
                    if not raw_text:
                        continue

                    is_control_packet = False
                    if raw_text.startswith("{") and raw_text.endswith("}"):
                        try:
                            ctrl_json = json.loads(raw_text)
                            if isinstance(ctrl_json, dict) and "type" in ctrl_json:
                                is_control_packet = True
                                msg_type = ctrl_json.get("type")

                                # [A] 인터럽트 (Barge-in / 끼어들기) 명령
                                if msg_type == "interrupt":
                                    reason = ctrl_json.get("reason", "voice")
                                    if reason == "touch":
                                        print("\n[제어 명령] 👆 화면 터치로 즉각 중단 수신 -> AI 발화 완전 중단 (이어말하기 취소)")
                                        interrupted_holder["session"] = None
                                        interrupted_holder["last_completed_session"] = None
                                        interrupted_holder["active_ai_response"] = None
                                        if ai_task and not ai_task.done():
                                            ai_task.cancel()
                                            try:
                                                await ai_task
                                            except asyncio.CancelledError:
                                                pass
                                    else:
                                        print("\n[제어 명령] 🛑 클라이언트 음성 끼어들기(Barge-in) 수신 -> AI 발화 중단 및 세션 보관")
                                        played_bytes = ctrl_json.get("played_bytes")
                                        if ai_task and not ai_task.done():
                                            ai_task.cancel()
                                            try:
                                                await ai_task
                                            except asyncio.CancelledError:
                                                pass
                                        
                                        # 활성 응답 또는 직전 완료 응답에서 스피커 실제 재생 진도율에 맞는 남은 문장 추출
                                        target_session = interrupted_holder.get("active_ai_response") or interrupted_holder.get("last_completed_session") or interrupted_holder.get("session")
                                        if target_session:
                                            sentences = target_session.get("sentences") or target_session.get("remaining_entries") or []
                                            t_start = target_session.get("t_playback_start")
                                            
                                            if played_bytes is None or played_bytes <= 0:
                                                if t_start:
                                                    elapsed = max(0.0, time.perf_counter() - t_start - 0.1)
                                                    played_bytes = int(elapsed * 32000)
                                                else:
                                                    played_bytes = 0
                                                    
                                            print(f"[서버] 끼어들기 시점 스피커 재생 바이트: {played_bytes} bytes ({played_bytes/32000:.2f}초)")
                                            remaining = calculate_remaining_sentences(sentences, played_bytes)
                                            
                                            # 아직 TTS 합성되지 않은 문장이 있다면 합성 완료
                                            for entry in remaining:
                                                if entry.get("pcm") is None and entry.get("text"):
                                                    try:
                                                        syn_pcm = await synthesize_tts_pcm(entry["text"])
                                                        if syn_pcm:
                                                            entry["pcm"] = syn_pcm
                                                    except Exception:
                                                        pass
                                            
                                            if remaining:
                                                interrupted_holder["session"] = {
                                                    "user_query": target_session.get("user_query", ""),
                                                    "remaining_entries": remaining,
                                                    "full_answer": target_session.get("full_answer", ""),
                                                    "close_session_after": target_session.get("close_session_after", False),
                                                    "was_thinking_only": False
                                                }
                                                rem_texts = " / ".join([e["text"] for e in remaining])
                                                print(f"[서버] 이어말하기 세션 준비 완료 ({len(remaining)}개 문장): {rem_texts}")
                                            elif target_session.get("was_thinking_only"):
                                                interrupted_holder["session"] = {
                                                    "user_query": target_session.get("user_query", ""),
                                                    "remaining_entries": [],
                                                    "full_answer": "",
                                                    "close_session_after": target_session.get("close_session_after", False),
                                                    "was_thinking_only": True
                                                }
                                                print(f"[서버] THINKING 중단 복구 준비 완료: 이전 질문('{target_session.get('user_query')}')")
                                            else:
                                                interrupted_holder["session"] = None

                                    is_speaking = False
                                    silence_time = 0.0
                                    barge_in_counter = 0
                                    audio_buffer = []
                                    pre_speech_buffer.clear()
                                    rx_accum = np.array([], dtype=np.float32)

                                    if reason != "touch":
                                        # 클라이언트가 확실하게 청취(LISTENING) 모드로 동기화되도록 통보 (음성 끼어들기 시에만)
                                        await websocket.send(json.dumps({"type": "status", "message": "LISTENING..."}))
                                    else:
                                        # 화면 터치로 중단한 경우 세션을 대기(IDLE) 상태로 완전히 종료
                                        await websocket.send(json.dumps({"type": "status", "message": "DONE", "close_session": True}))

                                # [B] 음성 발화 완료 신호 (클라이언트 VAD 감지)
                                elif msg_type == "speech_end":
                                    is_ai_busy = ai_task and not ai_task.done()
                                    if is_ai_busy:
                                        print("\n[제어 명령] ⏱️ 클라이언트 음성 발화 완료(speech_end) 수신 (이미 AI 응답 처리 중이므로 무시)")
                                    elif time.perf_counter() - t_last_whisper < 2.0:
                                        print("\n[제어 명령] ⏱️ 클라이언트 음성 발화 완료(speech_end) 수신 (방금 음성 인식으로 이미 처리 완료됨)")
                                        if not is_ai_busy:
                                            await websocket.send(json.dumps({"type": "status", "message": "DONE"}))
                                    elif audio_buffer or pre_speech_buffer:
                                        # 사용자가 아직 말을 이어가고 있고 오디오가 0.8초(12800 샘플) 미만인 경우,
                                        # 클라이언트의 성급한 조기 speech_end에 의해 문장이 중간에 잘려 인식 실패하는 것을 방지!
                                        combined_len = sum(len(c) for c in pre_speech_buffer) + sum(len(c) for c in audio_buffer)
                                        if is_speaking and combined_len < 12800:
                                            print(f"\n[제어 명령] ⏱️ 클라이언트 speech_end 수신했으나 서버 VAD 발화 진행 중 ({combined_len/SAMPLE_RATE:.2f}s < 0.8s) -> 발화 완료 대기 유지")
                                        else:
                                            combined_chunks = list(pre_speech_buffer) + list(audio_buffer)
                                            full_audio = np.concatenate(combined_chunks) if combined_chunks else np.array([], dtype=np.float32)
                                            audio_buffer = []
                                            pre_speech_buffer.clear()
                                            is_speaking = False
                                            silence_time = 0.0

                                            peak_val = np.max(np.abs(full_audio)) if len(full_audio) > 0 else 0.0
                                            # 최소 0.5초(8000 샘플) 이상이고 최소 음량(0.008) 이상일 때 실제 발화로 처리
                                            if len(full_audio) >= 8000 and peak_val >= 0.008:
                                                print("\n[제어 명령] ⏱️ 클라이언트 음성 발화 완료(speech_end) -> whisper.cpp 실행")
                                                user_text, asr_ms = await run_whisper_cpp(full_audio)
                                                t_last_whisper = time.perf_counter()
                                                print(f"[음성 인식 완료] {user_text or '(인식 실패 또는 무음)'} ({asr_ms:.0f}ms)")
                                                
                                                if is_meaningful_speech(user_text):
                                                    ai_task = asyncio.create_task(
                                                        handle_user_turn(user_text, asr_ms)
                                                    )
                                                else:
                                                    if interrupted_holder.get("session") is not None:
                                                        saved_session = interrupted_holder["session"]
                                                        interrupted_holder["session"] = None
                                                        print(f"[서버] 의미 있는 발화 없음 ('{user_text}'). 직전 AI 답변 이어서 재생!")
                                                        ai_task = asyncio.create_task(
                                                            resume_ai_response(websocket, saved_session, chat_history, interrupted_holder=interrupted_holder)
                                                        )
                                                    else:
                                                        print("[서버] 인식된 텍스트가 없어 대기 상태(DONE)를 클라이언트로 전송합니다.")
                                                        await websocket.send(json.dumps({"type": "status", "message": "DONE"}))
                                            else:
                                                print("[서버] 유효한 음성 신호가 감지되지 않아 직전 답변 확인.")
                                                if interrupted_holder.get("session") is not None:
                                                    saved_session = interrupted_holder["session"]
                                                    interrupted_holder["session"] = None
                                                    print("[서버] 무음/미약 신호 -> 직전 AI 답변 이어서 재생!")
                                                    ai_task = asyncio.create_task(
                                                        resume_ai_response(websocket, saved_session, chat_history, interrupted_holder=interrupted_holder)
                                                    )
                                                else:
                                                    await websocket.send(json.dumps({"type": "status", "message": "DONE"}))
                                    else:
                                        print("[제어 명령] 음성 발화 완료 요청되었으나 오디오 버퍼가 비어있음")
                                        if interrupted_holder.get("session") is not None:
                                            saved_session = interrupted_holder["session"]
                                            interrupted_holder["session"] = None
                                            print("[서버] 버퍼 비어있음 -> 직전 AI 답변 이어서 재생!")
                                            ai_task = asyncio.create_task(
                                                resume_ai_response(websocket, saved_session, chat_history, interrupted_holder=interrupted_holder)
                                            )
                                        elif not is_ai_busy:
                                            await websocket.send(json.dumps({"type": "status", "message": "DONE"}))

                                # [C] 음성 호출어 ('Alexa') 감지 알림
                                elif msg_type == "wake_word":
                                    word = ctrl_json.get("word", "alexa")
                                    print(f"\n[제어 명령] 🗣️ 호출어 '{word}' 감지 -> 새 대화 시작 (이전 세션 정리)")
                                    interrupted_holder["session"] = None
                                    interrupted_holder["last_completed_session"] = None
                                    interrupted_holder["active_ai_response"] = None
                                    if ai_task and not ai_task.done():
                                        ai_task.cancel()
                                        try:
                                            await ai_task
                                        except asyncio.CancelledError:
                                            pass
                                    await websocket.send(json.dumps({"type": "interrupt", "message": "CLEAR_AUDIO"}))
                                    audio_buffer = []
                                    pre_speech_buffer.clear()
                                    is_speaking = False
                                    silence_time = 0.0

                                # [D] 화면 터치로 대화 시작 또는 수동 발화 완료
                                elif msg_type == "touch" or msg_type == "session_start":
                                    ev = ctrl_json.get("event", "start")
                                    if ev == "speech_end":
                                        print("\n[제어 명령] 👆 화면 터치로 수동 발화 완료")
                                        if ai_task and not ai_task.done():
                                            ai_task.cancel()
                                            try:
                                                await ai_task
                                            except asyncio.CancelledError:
                                                pass

                                        combined_chunks = list(pre_speech_buffer) + list(audio_buffer)
                                        if len(rx_accum) > 0:
                                            combined_chunks.append(rx_accum)
                                            rx_accum = np.array([], dtype=np.float32)

                                        full_audio = np.concatenate(combined_chunks) if combined_chunks else np.array([], dtype=np.float32)
                                        audio_buffer = []
                                        pre_speech_buffer.clear()
                                        is_speaking = False
                                        silence_time = 0.0

                                        peak_val = np.max(np.abs(full_audio)) if len(full_audio) > 0 else 0.0
                                        if len(full_audio) >= 4800 and peak_val >= 0.01:
                                            print("[제어 명령] whisper.cpp 즉시 실행")
                                            user_text, asr_ms = await run_whisper_cpp(full_audio)
                                            print(f"[음성 인식 완료] {user_text or '(인식 실패 또는 무음)'} ({asr_ms:.0f}ms)")
                                            if is_meaningful_speech(user_text):
                                                ai_task = asyncio.create_task(
                                                    handle_user_turn(user_text, asr_ms, close_session_after=True)
                                                )
                                            else:
                                                if interrupted_holder.get("session") is not None:
                                                    saved_session = interrupted_holder["session"]
                                                    interrupted_holder["session"] = None
                                                    ai_task = asyncio.create_task(
                                                        resume_ai_response(websocket, saved_session, chat_history, interrupted_holder=interrupted_holder)
                                                    )
                                                else:
                                                    print("[서버] 인식된 텍스트가 없어 대기 상태(DONE, 세션 종료)를 클라이언트로 전송합니다.")
                                                    await websocket.send(json.dumps({"type": "status", "message": "DONE", "close_session": True}))
                                        else:
                                            if interrupted_holder.get("session") is not None:
                                                saved_session = interrupted_holder["session"]
                                                interrupted_holder["session"] = None
                                                ai_task = asyncio.create_task(
                                                    resume_ai_response(websocket, saved_session, chat_history, interrupted_holder=interrupted_holder)
                                                )
                                            else:
                                                print("[서버] 유효한 음성 신호가 없어 대기 상태(DONE, 세션 종료)를 전송합니다.")
                                                await websocket.send(json.dumps({"type": "status", "message": "DONE", "close_session": True}))
                                    else:
                                        print("\n[제어 명령] 👆 화면 터치로 대화 시작")
                                        interrupted_holder["session"] = None
                                        interrupted_holder["last_completed_session"] = None
                                        interrupted_holder["active_ai_response"] = None
                                        if ai_task and not ai_task.done():
                                            ai_task.cancel()
                                            try:
                                                await ai_task
                                            except asyncio.CancelledError:
                                                pass
                                            await websocket.send(json.dumps({"type": "interrupt", "message": "CLEAR_AUDIO"}))
                                        
                                        audio_buffer = []
                                        pre_speech_buffer.clear()
                                        is_speaking = False
                                        silence_time = 0.0

                                # [E] 기타 상태 알림
                                elif msg_type == "status":
                                    print(f"[제어 명령] 상태 알림: {ctrl_json.get('message')}")

                                # [F] LLM Type 변경 설정 수신
                                elif msg_type == "set_llm_type":
                                    new_type = ctrl_json.get("llm_type", "gemini api").strip().lower()
                                    need_reinit = (new_type != current_llm_type) or (
                                        new_type in ("gemini realtime api", "chatgpt realtime api")
                                        and (not active_realtime_session or not active_realtime_session.is_connected)
                                    )
                                    current_llm_type = new_type
                                    print(f"\n[설정 수신] 클라이언트로부터 LLM Type 변경 알림 수신: '{current_llm_type}'")
                                    if need_reinit:
                                        await switch_realtime_session(current_llm_type)
                                    await websocket.send(json.dumps({
                                        "type": "status",
                                        "message": f"LLM Type set to {current_llm_type}"
                                    }))

                        except Exception as parse_err:
                            print(f"[경고] JSON 파싱 실패 ({parse_err}), 일반 텍스트로 처리합니다.")
                            is_control_packet = False

                    # ★ 순수 텍스트 질의인 경우에만 LLM 처리
                    if not is_control_packet:
                        user_text = raw_text
                        t_speech_end = time.perf_counter()
                        print(f"\n[텍스트 입력 수신 (LLM 질의)] {user_text}")
                        interrupted_holder["session"] = None
                        
                        if ai_task and not ai_task.done():
                            ai_task.cancel()
                            try:
                                await ai_task
                            except asyncio.CancelledError:
                                pass
                        
                        await websocket.send(json.dumps({"type": "interrupt", "message": "CLEAR_AUDIO"}))
                        ai_task = asyncio.create_task(
                            process_ai_response(websocket, user_text, chat_history, t_speech_end, 0.0, interrupted_holder=interrupted_holder, llm_type=current_llm_type)
                        )

            except (websockets.exceptions.ConnectionClosed, websockets.exceptions.ConnectionClosedError, websockets.exceptions.ConnectionClosedOK):
                break
            except Exception as inner_e:
                print(f"\n[오류] 메시지 처리 중 예외 발생: {inner_e}")
                is_speaking = False
                silence_time = 0.0
                audio_buffer = []

    except (websockets.exceptions.ConnectionClosed, websockets.exceptions.ConnectionClosedError, websockets.exceptions.ConnectionClosedOK):
        pass
    finally:
        if active_realtime_session:
            await active_realtime_session.close()
            active_realtime_session = None
        print("[클라이언트 연결 종료]")

async def main():
    print("실시간 음성 비서 서버(ws://0.0.0.0:8765) 시작 중 (PCM 전송 모드)...")
    server = await websockets.serve(handler, "0.0.0.0", 8765, ping_interval=None, ping_timeout=None, max_size=None)
    await server.wait_closed()

if __name__ == "__main__":
    asyncio.run(main())
