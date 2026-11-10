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
    """KEY=VALUE 또는 export KEY=VALUE 항목을 환경 변수로 안전하게 로드"""
    if not os.path.exists(path):
        return []
    loaded_keys = []
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                # export 키워드 지원 (예: export OPENAI_API_KEY=...)
                if line.startswith("export "):
                    line = line[7:].strip()
                key, value = line.split("=", 1)
                key = key.strip()
                # 따옴표 및 공백 정리
                value = value.strip(" \t\r\n\"'")
                if key:
                    # 쉘에 환경변수가 없거나 빈 문자열인 경우 .env 값으로 설정
                    if key not in os.environ or not os.environ[key].strip():
                        os.environ[key] = value
                        loaded_keys.append(key)
    except Exception as e:
        print(f"[경고] .env 로드 중 오류 ({path}): {e}")
    return loaded_keys

# 1) server.py가 있는 폴더의 .env 탐색
_server_dir = os.path.dirname(os.path.abspath(__file__))
_keys1 = load_env_file(os.path.join(_server_dir, ".env"))
# 2) 상위 폴더(프로젝트 루트)의 .env 탐색
_root_dir = os.path.abspath(os.path.join(_server_dir, ".."))
_keys2 = load_env_file(os.path.join(_root_dir, ".env"))
# 3) 현재 실행 위치(CWD)의 .env 탐색
_keys3 = load_env_file(os.path.join(os.getcwd(), ".env"))

_all_loaded = list(set(_keys1 + _keys2 + _keys3))
if _all_loaded:
    print(f"[시스템] .env 환경 변수 로드 완료: {', '.join(_all_loaded)}")
else:
    print(f"[시스템] .env 파일을 찾지 못했거나 등록된 키가 없습니다. (탐색 경로: {_server_dir}/.env)")


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
GEMINI_REALTIME_MODEL = os.environ.get("GEMINI_REALTIME_MODEL", "gemini-2.5-flash-native-audio-latest").strip()
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
    def __init__(self, esp_ws, api_key, model=GEMINI_REALTIME_MODEL, voice=GEMINI_REALTIME_VOICE, on_switch_llm=None, on_adjust_audio=None):
        self.esp_ws = esp_ws
        self.api_key = api_key
        self.model = model
        self.voice = voice
        self.on_switch_llm = on_switch_llm
        self.on_adjust_audio = on_adjust_audio
        self.gemini_ws = None
        self.rx_task = None
        self.is_connected = False
        self.system_prompt = (
            "너의 이름은 '친구'야. 너는 음성으로 대화하는 친절하고 다정한 한국어 AI 비서야. "
            "절대 마크다운 기호(*, #, -)나 이모지를 쓰지 말고, 자연스러운 한국어 구어체로 두세 문장 이내로 간결하게 답해줘. "
            "사용자가 다른 모델이나 LLM 타입으로 바꿔달라고 하면 switch_llm_type 도구를 호출해줘. "
            "사용자가 볼륨(소리 크기)을 조절해달라고 하면 set_volume 도구를 호출해줘. "
            "사용자가 마이크 감도나 민감도를 조절해달라고 하면 set_mic_sensitivity 도구를 호출해줘."
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
                    },
                    "tools": [
                        {
                            "functionDeclarations": [
                                {
                                    "name": "switch_llm_type",
                                    "description": "사용자가 LLM 모델이나 타입을 변경/전환해 달라고 요청했을 때 호출합니다.",
                                    "parameters": {
                                        "type": "OBJECT",
                                        "properties": {
                                            "target_type": {
                                                "type": "STRING",
                                                "enum": ["gemini api", "gemini realtime api", "chatgpt realtime api", "local llm"],
                                                "description": "변경할 대상 LLM 타입"
                                            }
                                        },
                                        "required": ["target_type"]
                                    }
                                },
                                {
                                    "name": "set_volume",
                                    "description": "스피커 볼륨(소리 크기)을 변경합니다. level(10~100) 또는 direction('up', 'down', 'max', 'min')을 지정합니다.",
                                    "parameters": {
                                        "type": "OBJECT",
                                        "properties": {
                                            "level": {"type": "INTEGER", "description": "설정할 볼륨 퍼센트(10~100)"},
                                            "direction": {"type": "STRING", "enum": ["up", "down", "max", "min"], "description": "볼륨 조절 방향"}
                                        }
                                    }
                                },
                                {
                                    "name": "set_mic_sensitivity",
                                    "description": "마이크 감도(민감도/게인)를 변경합니다. level(1~6배율 또는 10~60) 또는 direction('up', 'down', 'max', 'min')을 지정합니다.",
                                    "parameters": {
                                        "type": "OBJECT",
                                        "properties": {
                                            "level": {"type": "INTEGER", "description": "설정할 마이크 감도(1~6 또는 10~60)"},
                                            "direction": {"type": "STRING", "enum": ["up", "down", "max", "min"], "description": "마이크 감도 조절 방향"}
                                        }
                                    }
                                },
                                {
                                    "name": "get_current_time",
                                    "description": "사용자가 현재 시각, 지금 몇 시인지, 날짜 및 시간을 물어볼 때 호출하여 현재 시간 정보를 얻고 화면에 시계를 표시합니다.",
                                    "parameters": {
                                        "type": "OBJECT",
                                        "properties": {}
                                    }
                                }
                            ]
                        }
                    ]
                }
            }
            await self.gemini_ws.send(json.dumps(setup_msg))
            
            # 서버의 setupComplete 응답 대기 (초기 핸드셰이크 완료 확인)
            init_resp = await asyncio.wait_for(self.gemini_ws.recv(), timeout=5.0)
            if isinstance(init_resp, bytes):
                init_resp = init_resp.decode("utf-8", errors="ignore")
            init_data = json.loads(init_resp) if init_resp else {}
            
            if "setupComplete" in init_data:
                self.is_connected = True
                print("[Gemini Realtime] 세션 Setup 완료 (STT/TTS 없는 직통 음성 스트리밍 준비 완료)")
                self.rx_task = asyncio.create_task(self._receive_loop())
            else:
                print(f"[Gemini Realtime] 예기치 않은 Setup 응답: {init_resp}")
                self.is_connected = True
                self.rx_task = asyncio.create_task(self._receive_loop())
                
        except websockets.exceptions.ConnectionClosed as cc:
            print(f"[Gemini Realtime] WebSocket 연결 거부 (Code: {cc.code}): {cc.reason}")
            self.is_connected = False
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

    async def send_text(self, text):
        """텍스트 질문을 Gemini Live 세션으로 전달"""
        if not self.is_connected or not self.gemini_ws:
            return
        try:
            client_content = {
                "clientContent": {
                    "turns": [
                        {
                            "role": "user",
                            "parts": [{"text": text}]
                        }
                    ],
                    "turnComplete": True
                }
            }
            await self.gemini_ws.send(json.dumps(client_content))
        except Exception as e:
            print(f"[Gemini Realtime] 텍스트 송신 오류: {e}")

    async def _receive_loop(self):
        """Gemini로부터 오는 24kHz 오디오를 16kHz로 리샘플링하여 ESP32로 즉시 바이너리 전송"""
        try:
            first_chunk = True
            speaking_active = False

            while self.is_connected and self.gemini_ws:
                try:
                    # AI 음성이 스트리밍 중일 때는 1.2초 동안 새 오디오가 없으면 turnComplete로 간주하는 타임아웃 적용
                    recv_timeout = 1.2 if speaking_active else None
                    raw = await asyncio.wait_for(self.gemini_ws.recv(), timeout=recv_timeout)
                except asyncio.TimeoutError:
                    if speaking_active:
                        print("\n[Gemini Realtime] AI 음성 출력 완료 감지 (침묵 1.2초 / turnComplete 누락 자동 보정) -> DONE 전송")
                        speaking_active = False
                        first_chunk = True
                        await self.esp_ws.send(json.dumps({"type": "status", "message": "DONE", "close_session": False}))
                    continue

                if isinstance(raw, bytes):
                    raw = raw.decode("utf-8", errors="ignore")
                data = json.loads(raw)
                server_content = data.get("serverContent")
                if not server_content:
                    continue

                # 사용자가 끼어들었음을 Gemini 서버가 감지했을 때
                if server_content.get("interrupted"):
                    print("\n[Gemini Realtime] 사용자 끼어들기 감지(interrupted) -> 스피커 플러시")
                    await self.esp_ws.send(json.dumps({"type": "interrupt", "message": "CLEAR_AUDIO"}))
                    first_chunk = True
                    speaking_active = False
                    continue

                # Tool Call (LLM Type 전환 등) 감지
                tool_call = server_content.get("toolCall")
                if tool_call:
                    calls = tool_call.get("functionCalls", [])
                    for fc in calls:
                        if fc.get("name") == "switch_llm_type":
                            args = fc.get("args", {})
                            target = args.get("target_type")
                            print(f"\n[Gemini Realtime Tool Call] switch_llm_type 호출 감지 -> target: '{target}'")
                            if self.on_switch_llm and target:
                                asyncio.create_task(self.on_switch_llm(target))
                            return
                        elif fc.get("name") in ("set_volume", "set_mic_sensitivity"):
                            cmd_name = fc.get("name")
                            args = fc.get("args", {})
                            print(f"\n[Gemini Realtime Tool Call] {cmd_name} 호출 감지: {args}")
                            call_id = fc.get("id")
                            if self.on_adjust_audio:
                                asyncio.create_task(self.on_adjust_audio(cmd_name, args))
                            if call_id and self.gemini_ws:
                                try:
                                    resp = {
                                        "toolResponse": {
                                            "functionResponses": [
                                                {
                                                    "response": {"output": {"status": "success"}},
                                                    "id": call_id
                                                }
                                            ]
                                        }
                                    }
                                    await self.gemini_ws.send(json.dumps(resp))
                                except Exception:
                                    pass
                        elif fc.get("name") == "get_current_time":
                            print(f"\n[Gemini Realtime Tool Call] get_current_time 호출 감지! (LCD 시계 12초 표시)")
                            asyncio.create_task(self.esp_ws.send(json.dumps({"type": "show_clock", "duration": 12})))
                            call_id = fc.get("id")
                            if call_id and self.gemini_ws:
                                try:
                                    time_speech = get_korean_time_speech()
                                    resp = {
                                        "toolResponse": {
                                            "functionResponses": [
                                                {
                                                    "response": {"output": {"current_time": time_speech}},
                                                    "id": call_id
                                                }
                                            ]
                                        }
                                    }
                                    await self.gemini_ws.send(json.dumps(resp))
                                except Exception:
                                    pass

                model_turn = server_content.get("modelTurn")
                if model_turn:
                    for part in model_turn.get("parts", []):
                        inline = part.get("inlineData")
                        if inline and inline.get("mimeType", "").startswith("audio/pcm"):
                            b64_pcm = inline.get("data", "")
                            if b64_pcm:
                                pcm24 = base64.b64decode(b64_pcm)
                                pcm16 = resample_24k_to_16k(pcm24)
                                speaking_active = True
                                if first_chunk:
                                    first_chunk = False
                                    await self.esp_ws.send(json.dumps({"type": "status", "message": "SPEAKING..."}))
                                await self.esp_ws.send(pcm16)

                if server_content.get("turnComplete"):
                    print("\n[Gemini Realtime] turnComplete 수신 -> DONE 전송")
                    first_chunk = True
                    speaking_active = False
                    await self.esp_ws.send(json.dumps({"type": "status", "message": "DONE", "close_session": False}))

        except asyncio.CancelledError:
            pass
        except websockets.exceptions.ConnectionClosed as cc:
            if "inactivity" in str(cc).lower() or getattr(cc, "code", None) == 1008:
                print("\n[Gemini Realtime] 장시간 미사용으로 클라우드 세션 만료(1008 Session closed due to client inactivity). 다음 발화 시 자동 재연결됩니다.")
            else:
                print(f"\n[Gemini Realtime] WebSocket 연결 종료 (Code: {getattr(cc, 'code', 'unknown')}): {cc}")
        except Exception as e:
            if "inactivity" in str(e).lower():
                print("\n[Gemini Realtime] 장시간 미사용으로 클라우드 세션 만료. 다음 발화 시 자동 재연결됩니다.")
            else:
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
    def __init__(self, esp_ws, api_key, model=OPENAI_REALTIME_MODEL, voice=OPENAI_VOICE, on_switch_llm=None, on_adjust_audio=None):
        self.esp_ws = esp_ws
        self.api_key = api_key
        self.model = model
        self.voice = voice
        self.on_switch_llm = on_switch_llm
        self.on_adjust_audio = on_adjust_audio
        self.openai_ws = None
        self.rx_task = None
        self.is_connected = False
        self.system_prompt = (
            "너의 이름은 '친구'야. 너는 음성으로 대화하는 친절하고 다정한 한국어 AI 비서야. "
            "절대 마크다운 기호(*, #, -)나 이모지를 쓰지 말고, 자연스러운 한국어 구어체로 두세 문장 이내로 간결하게 대답해. "
            "사용자가 다른 모델이나 LLM 타입으로 바꿔달라고 하면 switch_llm_type 도구를 호출해줘. "
            "사용자가 볼륨(소리 크기)을 조절해달라고 하면 set_volume 도구를 호출해줘. "
            "사용자가 마이크 감도나 민감도를 조절해달라고 하면 set_mic_sensitivity 도구를 호출해줘."
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
                    },
                    "tools": [
                        {
                            "type": "function",
                            "name": "switch_llm_type",
                            "description": "사용자가 LLM 모델이나 타입을 변경/전환해 달라고 요청했을 때 호출합니다.",
                            "parameters": {
                                "type": "object",
                                "properties": {
                                    "target_type": {
                                        "type": "string",
                                        "description": "변경할 LLM 종류 ('gemini api', 'gemini realtime api', 'chatgpt realtime api', 'local llm')",
                                        "enum": ["gemini api", "gemini realtime api", "chatgpt realtime api", "local llm"]
                                    }
                                },
                                "required": ["target_type"]
                            }
                        },
                        {
                            "type": "function",
                            "name": "set_volume",
                            "description": "스피커 볼륨(소리 크기)을 변경합니다. level(10~100) 또는 direction('up', 'down', 'max', 'min')을 지정합니다.",
                            "parameters": {
                                "type": "object",
                                "properties": {
                                    "level": {"type": "integer", "description": "설정할 볼륨 퍼센트(10~100)"},
                                    "direction": {"type": "string", "enum": ["up", "down", "max", "min"], "description": "볼륨 조절 방향"}
                                }
                            }
                        },
                        {
                            "type": "function",
                            "name": "set_mic_sensitivity",
                            "description": "마이크 감도(민감도/게인)를 변경합니다. level(1~6배율 또는 10~60) 또는 direction('up', 'down', 'max', 'min')을 지정합니다.",
                            "parameters": {
                                "type": "object",
                                "properties": {
                                    "level": {"type": "integer", "description": "설정할 마이크 감도(1~6 또는 10~60)"},
                                    "direction": {"type": "string", "enum": ["up", "down", "max", "min"], "description": "마이크 감도 조절 방향"}
                                }
                            }
                        },
                        {
                            "type": "function",
                            "name": "get_current_time",
                            "description": "사용자가 현재 시각, 지금 몇 시인지, 날짜 및 시간을 물어볼 때 호출하여 현재 시간 정보를 얻고 화면에 시계를 표시합니다.",
                            "parameters": {
                                "type": "object",
                                "properties": {}
                            }
                        }
                    ],
                    "tool_choice": "auto"
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

    async def commit_audio(self):
        """클라이언트 발화 종료(speech_end) 시 버퍼 확정 및 응답 생성 요청"""
        if not self.is_connected or not self.openai_ws:
            return
        try:
            await self.openai_ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
            await self.openai_ws.send(json.dumps({"type": "response.create"}))
        except Exception as e:
            print(f"[OpenAI Realtime] commit_audio 오류: {e}")

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

                # 5. Tool Call (LLM Type 전환 등) 감지
                elif event_type == "response.function_call_arguments.done":
                    func_name = event.get("name")
                    if func_name == "switch_llm_type":
                        args_raw = event.get("arguments", "{}")
                        try:
                            args_json = json.loads(args_raw)
                            target = args_json.get("target_type")
                            print(f"\n[OpenAI Realtime Tool Call] switch_llm_type 감지 -> target: '{target}'")
                            if self.on_switch_llm and target:
                                asyncio.create_task(self.on_switch_llm(target))
                        except Exception as e:
                            print(f"[OpenAI Realtime Tool Call 파싱 오류] {e}")
                    elif func_name in ("set_volume", "set_mic_sensitivity"):
                        args_raw = event.get("arguments", "{}")
                        try:
                            args_json = json.loads(args_raw)
                            print(f"\n[OpenAI Realtime Tool Call] {func_name} 감지: {args_json}")
                            if self.on_adjust_audio:
                                asyncio.create_task(self.on_adjust_audio(func_name, args_json))
                        except Exception as e:
                            print(f"[OpenAI Realtime Tool Call 파싱 오류] {e}")
                    elif func_name == "get_current_time":
                        print(f"\n[OpenAI Realtime Tool Call] get_current_time 감지! (LCD 시계 12초 표시)")
                        asyncio.create_task(self.esp_ws.send(json.dumps({"type": "show_clock", "duration": 12})))
                        call_id = event.get("call_id")
                        time_speech = get_korean_time_speech()
                        if call_id and self.openai_ws:
                            try:
                                resp = {
                                    "type": "conversation.item.create",
                                    "item": {
                                        "type": "function_call_output",
                                        "call_id": call_id,
                                        "output": json.dumps({"current_time": time_speech})
                                    }
                                }
                                asyncio.create_task(self.openai_ws.send(json.dumps(resp)))
                                asyncio.create_task(self.openai_ws.send(json.dumps({"type": "response.create"})))
                            except Exception as e:
                                print(f"[OpenAI Realtime Tool 응답 오류] {e}")

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

TIME_KEYWORDS = (
    "현재 시간", "현재시간", "지금 몇 시", "지금 몇시", "몇 시야", "몇 시예요",
    "몇 시인가요", "몇 시인지", "몇시야", "몇시예요", "몇시인가요", "몇시인지",
    "시간 알려줘", "시간 몇 시", "시간 몇시", "시간이 어떻게", "지금 시간",
    "지금시간", "현재 시각", "지금 시각", "몇 시니", "몇시니", "시간 좀 알려줘",
    "시간 말해줘", "시계 보여줘"
)

def is_time_query(text):
    """사용자 발화에 현재 시간 질의 키워드가 포함되어 있는지 판정"""
    if not text:
        return False
    no_space = text.replace(" ", "")
    for k in TIME_KEYWORDS:
        if k in text or k.replace(" ", "") in no_space:
            return True
    return False

def get_korean_time_speech():
    """현재 시스템 시간을 한국어 음성 대화용 자연스러운 구어체 문장으로 생성"""
    import datetime
    now = datetime.datetime.now()
    ampm = "오전" if now.hour < 12 else "오후"
    h = now.hour % 12
    if h == 0:
        h = 12
    m = now.minute
    return f"현재 시간은 {ampm} {h}시 {m}분입니다."

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

def parse_llm_switch_command(user_text):
    """
    사용자의 발화에서 LLM Type 변경 의도를 분석합니다.
    변경 요청일 경우 (target_type, display_name, response_text)를 반환하고,
    아닐 경우 (None, None, None)을 반환합니다.
    """
    if not user_text:
        return None, None, None

    text = user_text.lower().strip()
    clean_text = re.sub(r'[^\w\s가-힣]', ' ', text)
    no_space_text = re.sub(r'[\s_]+', '', clean_text)

    # 1. 전환/변경 관련 의도 키워드 검사 (한국어 및 영어)
    switch_action_keywords = [
        "바꿔", "변경", "전환", "교체", "선택", "설정", "바꾸", "바꿀", "바꿈",
        "쓸래", "사용할래", "쓰자", "사용하자", "적용", "스위치", "switch", "change",
        "시작해", "해줘", "해봐", "써줘", "사용해", "부탁", "넘어가", "골라", "틀어줘",
        "set", "use", "turn on", "enable"
    ]
    has_action = any(kw in clean_text or kw in no_space_text for kw in switch_action_keywords)
    has_switch_syntax = (
        has_action
        or any(k in clean_text for k in ["으로", "로", "모드", "타입", "모델", "엔진", "api", "에이피아이", "mode", "type", "llm", "엘엘엠"])
        or any(k in no_space_text for k in ["으로", "로", "모드", "타입", "모델", "엔진", "api", "에이피아이", "mode", "type", "llm", "엘엘엠"])
    )

    # 2. 타겟 모델 식별 키워드
    is_gemini = any(k in clean_text or k in no_space_text for k in ["gemini", "제미나이", "제미니"])
    is_realtime = any(k in clean_text or k in no_space_text for k in ["realtime", "리얼타임", "실시간", "라이브", "live", "리얼", "타임"]) and (
        any(k in no_space_text for k in ["realtime", "리얼타임", "실시간", "live", "라이브"])
    )
    is_chatgpt = any(k in clean_text or k in no_space_text for k in ["chatgpt", "챗지피티", "챗gpt", "지피티", "gpt", "오픈ai", "openai"])
    is_local = any(k in clean_text or k in no_space_text for k in ["local", "로컬", "라마", "llama", "오프라인", "내부모델", "로컬서버"])

    # [1] 기본 LLM / Gemini 일반 API
    # "기본 LLM으로 전환", "기본 모델로 변경", "기본으로", "gemini 일반 api", "제미나이 일반", "일반 api" 등
    is_default_keyword = any(k in no_space_text for k in [
        "기본llm", "기본엘엘엠", "기본모델", "원래모델", "기본으로", "원래대로",
        "일반llm", "일반엘엘엠", "일반api", "기본api", "일반제미나이", "제미나이일반",
        "일반gemini", "gemini일반", "제미니일반", "일반제미니"
    ]) or (
        ("기본" in clean_text or "원래" in clean_text) and any(m in clean_text for m in ["llm", "엘엘엠", "모델", "api", "모드"])
    ) or (
        is_gemini and any(k in clean_text for k in ["일반", "기본", "플래시", "flash"])
    )

    if is_default_keyword:
        if has_switch_syntax or has_action:
            return (
                "gemini api",
                "Gemini API",
                "LLM을 기본 Gemini API로 변경했습니다. Gemini 2.5 Flash 모델로 대답할게요."
            )

    # [2] ChatGPT Realtime API
    if is_chatgpt and not (is_gemini or is_local):
        if is_realtime or has_switch_syntax:
            return (
                "chatgpt realtime api",
                "ChatGPT Realtime API",
                "LLM을 ChatGPT Realtime API로 변경했습니다. 이제 ChatGPT 실시간 음성으로 대화해요."
            )

    # [3] Gemini Realtime API (Gemini+Realtime 이거나, ChatGPT/Local 언급 없는 단독 실시간/리얼타임 전환 요청)
    if is_realtime:
        if is_gemini or (not is_chatgpt and not is_local and has_switch_syntax):
            return (
                "gemini realtime api",
                "Gemini Realtime API",
                "LLM을 Gemini Realtime API로 변경했습니다. 이제 실시간 음성 스트리밍으로 대화해요."
            )

    # [4] Local LLM (127.0.0.1:8080)
    if is_local:
        if has_switch_syntax or any(k in clean_text for k in ["llm", "엘엘엠", "모델", "서버"]):
            return (
                "local llm",
                "Local LLM",
                "LLM을 Local LLM으로 변경했습니다. 로컬 서버 모델로 응답해 드릴게요."
            )

    # [5] 일반 Gemini API (플래시/클라우드/일반/기본)
    if is_gemini and not is_realtime:
        if has_switch_syntax or any(k in clean_text for k in ["api", "기본", "일반", "플래시", "flash", "클라우드"]):
            return (
                "gemini api",
                "Gemini API",
                "LLM을 기본 Gemini API로 변경했습니다. Gemini 2.5 Flash 모델로 대답할게요."
            )

    # [6] 기타 기본/원래 복귀 의도
    if any(k in no_space_text for k in ["기본으로", "원래대로", "기본모델로", "원래모델로"]):
        if has_switch_syntax:
            return (
                "gemini api",
                "Gemini API",
                "LLM을 기본 Gemini API로 변경했습니다. Gemini 2.5 Flash 모델로 대답할게요."
            )

    return None, None, None

def extract_number_from_text(text):
    """텍스트에서 숫자(아라비아 숫자 또는 한글 수사)를 안전하게 추출"""
    # 1. 아라비아 숫자 우선 (예: "80", "5", "100")
    m = re.search(r'(\d+)', text)
    if m:
        return int(m.group(1))

    # 2. 십 단위 복합 수사
    tens = {
        "백": 100, "구십": 90, "팔십": 80, "칠십": 70, "육십": 60,
        "오십": 50, "사십": 40, "삼십": 30, "이십": 20, "십": 10
    }
    for k, v in tens.items():
        if k in text:
            return v

    # 3. 고유어 수사
    singles = {
        "하나": 1, "둘": 2, "셋": 3, "넷": 4, "다섯": 5, "여섯": 6,
        "일곱": 7, "여덟": 8, "아홉": 9, "열": 10
    }
    for k, v in singles.items():
        if k in text:
            return v

    # 4. 한자어 수사는 다른 단어의 부분 일치(예: '마이크'의 '이', '게인'의 '이')를 방지하기 위해 조사/단위 접미사 결합 시에만 매칭
    m_single = re.search(r'([일이삼사오육칠팔구])(?:로|으로|단계|레벨|배)', text)
    if m_single:
        s_map = {"일": 1, "이": 2, "삼": 3, "사": 4, "오": 5, "육": 6, "칠": 7, "팔": 8, "구": 9}
        return s_map.get(m_single.group(1))

    return None

def parse_audio_settings_command(user_text, cur_vol=70, cur_gain=35):
    """
    사용자의 발화에서 볼륨 또는 마이크 감도 조절 명령을 분석합니다.
    (cmd_type, new_val, resp_msg) 또는 (None, None, None) 반환
    cmd_type: 'volume' | 'mic_gain'
    """
    if not user_text:
        return None, None, None

    text = user_text.lower().strip()

    # [1] 마이크 감도 조절
    if "마이크" in text or "mic" in text or "게인" in text or ("감도" in text and "볼륨" not in text) or ("민감도" in text):
        # 1) 최대/최소
        if any(k in text for k in ["최대", "제일 높", "가장 높", "제일 크게", "끝까지"]):
            new_gain = 60
            return "mic_gain", new_gain, "마이크 감도를 최대인 6배로 설정했습니다."
        if any(k in text for k in ["최소", "제일 낮", "가장 낮", "제일 작게"]):
            new_gain = 10
            return "mic_gain", new_gain, "마이크 감도를 최소인 1배로 설정했습니다."

        # 2) 올리기 / 민감하게 (키워드가 있으면 수치 추출보다 증감 의도 우선)
        if any(k in text for k in ["올려", "키워", "높여", "더 크게", "민감하게", "잘 안 들려", "더 잘"]):
            new_gain = min(60, cur_gain + 10)
            return "mic_gain", new_gain, f"마이크 감도를 {new_gain / 10:.1f}배로 올렸습니다."

        # 3) 내리기 / 둔하게
        if any(k in text for k in ["내려", "줄여", "낮춰", "더 작게", "둔하게"]):
            new_gain = max(10, cur_gain - 10)
            return "mic_gain", new_gain, f"마이크 감도를 {new_gain / 10:.1f}배로 낮췄습니다."

        # 4) 직접 숫자 지정
        num = extract_number_from_text(text)
        if num is not None:
            if 1 <= num <= 6:
                new_gain = num * 10
            elif 10 <= num <= 60:
                new_gain = num
            else:
                new_gain = max(10, min(60, num))
            return "mic_gain", new_gain, f"마이크 감도를 {new_gain / 10:.1f}배로 설정했습니다."

    # [2] 볼륨 (소리 크기) 조절
    if any(k in text for k in ["볼륨", "소리", "음량", "목소리", "volume"]):
        # 1) 최대/최소
        if any(k in text for k in ["최대", "제일 크게", "가장 크게", "끝까지", "맥스", "max"]):
            new_vol = 100
            return "volume", new_vol, "볼륨을 최대인 100%로 설정했습니다."
        if any(k in text for k in ["최소", "제일 작게", "가장 작게", "최저", "min"]):
            new_vol = 10
            return "volume", new_vol, "볼륨을 최소인 10%로 설정했습니다."

        # 2) 올리기
        if any(k in text for k in ["올려", "키워", "크게", "높여", "더 크게", "작아", "업", "더 커", "안 들려"]):
            new_vol = min(100, cur_vol + 15)
            return "volume", new_vol, f"볼륨을 {new_vol}%로 올렸습니다."

        # 3) 내리기
        if any(k in text for k in ["내려", "줄여", "작게", "낮춰", "더 작게", "너무 커", "시끄러", "다운"]):
            new_vol = max(10, cur_vol - 15)
            return "volume", new_vol, f"볼륨을 {new_vol}%로 줄였습니다."

        # 4) 직접 숫자 지정 ("볼륨 80", "소리 50으로", "볼륨 오십")
        num = extract_number_from_text(text)
        if num is not None:
            new_vol = max(10, min(100, num))
            return "volume", new_vol, f"볼륨을 {new_vol}%로 설정했습니다."

    return None, None, None

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

async def process_ai_response(websocket, user_text, chat_history, t_speech_end, asr_ms=0.0, close_session_after=False, interrupted_holder=None, image_bytes=None, llm_type="gemini api", direct_answer=None):
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

        if direct_answer:
            async def _direct_gen():
                yield direct_answer
            response = _direct_gen()
        else:
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
    current_volume = 70
    current_mic_gain = 35
    current_brightness = 80
    current_ntp_server = "time.google.com (Google)"
    current_timezone = "Seoul (GMT+9)"
    current_timezone_posix = "KST-9"
    current_language = "ko"
    client_settings = {
        "volume": current_volume,
        "mic_gain": current_mic_gain,
        "brightness": current_brightness,
        "llm_type": current_llm_type,
        "ntp_server": current_ntp_server,
        "timezone": current_timezone,
        "timezone_posix": current_timezone_posix,
        "language": current_language
    }

    active_realtime_session = None
    t_last_rt_retry = 0.0

    async def adjust_audio_settings(cmd_type, target_val_or_dir, custom_resp=None):
        nonlocal current_volume, current_mic_gain
        if cmd_type in ("set_volume", "volume"):
            new_vol = current_volume
            if isinstance(target_val_or_dir, dict):
                lvl = target_val_or_dir.get("level")
                direction = target_val_or_dir.get("direction")
                if lvl is not None:
                    new_vol = max(10, min(100, int(lvl)))
                elif direction == "up":
                    new_vol = min(100, current_volume + 15)
                elif direction == "down":
                    new_vol = max(10, current_volume - 15)
                elif direction == "max":
                    new_vol = 100
                elif direction == "min":
                    new_vol = 10
            elif isinstance(target_val_or_dir, (int, float)):
                new_vol = max(10, min(100, int(target_val_or_dir)))
            elif target_val_or_dir == "up":
                new_vol = min(100, current_volume + 15)
            elif target_val_or_dir == "down":
                new_vol = max(10, current_volume - 15)
            elif target_val_or_dir == "max":
                new_vol = 100
            elif target_val_or_dir == "min":
                new_vol = 10

            current_volume = new_vol
            client_settings["volume"] = current_volume
            print(f"\n[오디오 설정 변경] 볼륨 -> {current_volume}%")
            try:
                await websocket.send(json.dumps({
                    "type": "set_volume",
                    "volume": current_volume
                }))
            except Exception as e:
                print(f"[설정 전송 오류] set_volume 전송 실패: {e}")

            resp_msg = custom_resp or f"볼륨을 {current_volume}%로 설정했습니다."

        elif cmd_type in ("set_mic_gain", "set_mic_sensitivity", "mic_gain"):
            new_gain = current_mic_gain
            if isinstance(target_val_or_dir, dict):
                lvl = target_val_or_dir.get("level")
                direction = target_val_or_dir.get("direction")
                if lvl is not None:
                    lvl_int = int(lvl)
                    if 1 <= lvl_int <= 6:
                        new_gain = lvl_int * 10
                    else:
                        new_gain = max(10, min(60, lvl_int))
                elif direction == "up":
                    new_gain = min(60, current_mic_gain + 10)
                elif direction == "down":
                    new_gain = max(10, current_mic_gain - 10)
                elif direction == "max":
                    new_gain = 60
                elif direction == "min":
                    new_gain = 10
            elif isinstance(target_val_or_dir, (int, float)):
                val_int = int(target_val_or_dir)
                if 1 <= val_int <= 6:
                    new_gain = val_int * 10
                else:
                    new_gain = max(10, min(60, val_int))
            elif target_val_or_dir == "up":
                new_gain = min(60, current_mic_gain + 10)
            elif target_val_or_dir == "down":
                new_gain = max(10, current_mic_gain - 10)
            elif target_val_or_dir == "max":
                new_gain = 60
            elif target_val_or_dir == "min":
                new_gain = 10

            current_mic_gain = new_gain
            client_settings["mic_gain"] = current_mic_gain
            print(f"\n[오디오 설정 변경] 마이크 감도 -> {current_mic_gain} ({current_mic_gain / 10:.1f}x)")
            try:
                await websocket.send(json.dumps({
                    "type": "set_mic_gain",
                    "gain": current_mic_gain
                }))
            except Exception as e:
                print(f"[설정 전송 오류] set_mic_gain 전송 실패: {e}")

            resp_msg = custom_resp or f"마이크 감도를 {current_mic_gain / 10:.1f}배로 설정했습니다."
        else:
            return

        # AI 음성 안내(TTS) 및 완료 상태 전송
        try:
            await websocket.send(json.dumps({"type": "status", "message": "SPEAKING..."}))
            pcm = await synthesize_tts_pcm(resp_msg)
            if pcm:
                await websocket.send(pcm)
            await websocket.send(json.dumps({"type": "status", "message": "DONE", "close_session": False}))
        except Exception as e:
            print(f"[안내 음성 오류] {e}")

    async def switch_and_notify_llm(target_type, display_name=None, response_text=None):
        nonlocal current_llm_type, active_realtime_session
        name_map = {
            "gemini api": "Gemini API",
            "gemini realtime api": "Gemini Realtime API",
            "chatgpt realtime api": "ChatGPT Realtime API",
            "local llm": "Local LLM"
        }
        disp_name = display_name or name_map.get(target_type, target_type)
        resp_msg = response_text or f"LLM을 {disp_name}으로 변경했습니다."

        print(f"\n[LLM Type 전환 처리 시작] 대상: '{target_type}' ({disp_name})")

        # 1. ESP32로 변경 명령 전송 (NVS 영구 저장용)
        try:
            await websocket.send(json.dumps({
                "type": "set_llm_type",
                "llm_type": target_type
            }))
        except Exception as e:
            print(f"[설정 전송 오류] ESP32 set_llm_type 전송 실패: {e}")

        # 2. ESP32 상태 표시줄 갱신
        try:
            await websocket.send(json.dumps({
                "type": "status",
                "message": f"LLM: {disp_name}"
            }))
        except Exception:
            pass

        # 3. 기존 Realtime 세션이 있다면 종료
        if active_realtime_session:
            await active_realtime_session.close()
            active_realtime_session = None

        current_llm_type = target_type
        client_settings["llm_type"] = current_llm_type
        print(f"[LLM Type 전환 완료] 서버 current_llm_type = '{current_llm_type}'")

        # 4. 사용자에게 어떤 LLM으로 변경했는지 AI 음성(TTS) 및 화면 안내
        try:
            await websocket.send(json.dumps({"type": "status", "message": "SPEAKING..."}))
            pcm = await synthesize_tts_pcm(resp_msg)
            if pcm:
                await websocket.send(pcm)
            await websocket.send(json.dumps({"type": "status", "message": "DONE", "close_session": False}))
        except Exception as tts_err:
            print(f"[안내 음성 합성 오류] {tts_err}")

        # 5. 새 타입이 Realtime인 경우 실시간 세션 즉시 연결
        if current_llm_type in ("gemini realtime api", "chatgpt realtime api"):
            await switch_realtime_session(current_llm_type)

    async def switch_realtime_session(target_type):
        nonlocal active_realtime_session, t_last_rt_retry
        now = time.perf_counter()
        if now - t_last_rt_retry < 3.0:
            return  # 3초 이내 중복 재연결 방지
        t_last_rt_retry = now

        if active_realtime_session:
            await active_realtime_session.close()
            active_realtime_session = None

        if target_type == "gemini realtime api":
            print("[세션 전환] Gemini Realtime (Live WebSocket) 세션 초기화 중...")
            active_realtime_session = GeminiLiveSession(websocket, GEMINI_API_KEY, on_switch_llm=switch_and_notify_llm, on_adjust_audio=adjust_audio_settings)
            await active_realtime_session.start()
        elif target_type == "chatgpt realtime api":
            print("[세션 전환] ChatGPT Realtime (OpenAI WebSocket) 세션 초기화 중...")
            active_realtime_session = OpenAIRealtimeSession(websocket, OPENAI_API_KEY, on_switch_llm=switch_and_notify_llm, on_adjust_audio=adjust_audio_settings)
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
                "사용자가 모델 변경이나 볼륨 등 기기 설정을 요청하면 시스템 제어기가 음성 명령을 감지하여 직접 처리하므로 '설정을 변경할 수 없다'는 거절성 답변은 하지 마. "
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

        # LLM Type 변경 음성 명령 감지
        sw_target, sw_name, sw_resp = parse_llm_switch_command(user_text)
        if sw_target:
            print(f"\n[음성 명령 감지] LLM 변경 요청: '{user_text}' -> '{sw_target}' ({sw_name})")
            await switch_and_notify_llm(sw_target, sw_name, sw_resp)
            return

        # 볼륨 / 마이크 감도 조절 음성 명령 감지
        aud_cmd, aud_val, aud_resp = parse_audio_settings_command(user_text, current_volume, current_mic_gain)
        if aud_cmd:
            print(f"\n[음성 명령 감지] 오디오 설정 변경: '{user_text}' -> {aud_cmd}={aud_val}")
            await adjust_audio_settings(aud_cmd, aud_val, aud_resp)
            return

        interrupted_holder["session"] = None
        interrupted_holder["last_completed_session"] = None
        interrupted_holder["active_ai_response"] = None

        should_close = close_session_after or is_session_end_query(user_text)
        if is_session_end_query(user_text):
            print(f"\n[대화 종료 의도 감지] '{user_text}' -> 작별 인사 후 세션 종료(Alexa 대기 상태 전환) 예약")

        # 현재 시간 질의 감지 -> ESP32 LCD 시계 표시 명령 및 정확한 시간 답변
        if is_time_query(user_text):
            print(f"\n[시간 질의 감지] '{user_text}' -> ESP32 LCD 시계 표시 명령 전송!")
            try:
                await websocket.send(json.dumps({"type": "show_clock", "duration": 12}))
            except Exception as e:
                print(f"[경고] show_clock 명령 전송 실패: {e}")

            # 시각 분석이 필요한 복합 질문이 아니고 순수 시간 질의인 경우 즉시 정확한 시간 음성 응답
            if not is_vision_query(user_text) and len(user_text.strip()) <= 25:
                time_speech = get_korean_time_speech()
                print(f"[시간 즉시 응답] {time_speech}")
                await process_ai_response(
                    websocket, user_text, chat_history,
                    time.perf_counter(), asr_ms,
                    close_session_after=should_close,
                    interrupted_holder=interrupted_holder,
                    direct_answer=time_speech
                )
                return

        image_bytes = None
        if not should_close and is_vision_query(user_text):
            print(f"\n[시각 질의 감지] '{user_text}' -> ESP32 카메라 촬영 요청 전송!")
            try:
                loop = asyncio.get_running_loop()
                pending_image_future = loop.create_future()
                await websocket.send(json.dumps({"type": "capture"}))
                image_bytes = await asyncio.wait_for(asyncio.shield(pending_image_future), timeout=3.5)
                if image_bytes:
                    print(f"[시각 질의 준비 완료] 사진 첨부하여 {current_llm_type} 시각 질의 시작 ({len(image_bytes)} bytes)")
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
                                    if current_llm_type in ("gemini realtime api", "chatgpt realtime api"):
                                        print(f"\n[제어 명령] ⏱️ 클라이언트 음성 발화 완료(speech_end) 수신 ({current_llm_type} 턴 완료 전송)")
                                        if current_llm_type == "gemini realtime api":
                                            if active_realtime_session and active_realtime_session.is_connected:
                                                # Gemini Multimodal Live API VAD는 발화 직후 500ms(16,000 bytes)의 침묵 구간을 수신해야
                                                # 사용자 발화 턴이 끝났음을 인지하고 modelTurn AI 음성 생성을 시작합니다.
                                                silence_chunk = b"\x00" * 16000
                                                await active_realtime_session.send_audio(silence_chunk)
                                        elif current_llm_type == "chatgpt realtime api":
                                            if active_realtime_session and active_realtime_session.is_connected:
                                                await active_realtime_session.commit_audio()
                                        continue

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

                                    if current_llm_type in ("gemini realtime api", "chatgpt realtime api"):
                                        if not active_realtime_session or not active_realtime_session.is_connected:
                                            asyncio.create_task(switch_realtime_session(current_llm_type))

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

                                # [G] 클라이언트 전체 설정 동기화 수신 (LLM, Volume, Mic Gain, Brightness, NTP, TZ, Lang 등)
                                elif msg_type in ("client_info", "settings_update"):
                                    if "volume" in ctrl_json:
                                        current_volume = int(ctrl_json["volume"])
                                        client_settings["volume"] = current_volume
                                    if "mic_gain" in ctrl_json:
                                        current_mic_gain = int(ctrl_json["mic_gain"])
                                        client_settings["mic_gain"] = current_mic_gain
                                    if "brightness" in ctrl_json:
                                        current_brightness = int(ctrl_json["brightness"])
                                        client_settings["brightness"] = current_brightness
                                    if "llm_type" in ctrl_json:
                                        new_type = str(ctrl_json["llm_type"]).strip().lower()
                                        need_reinit = (new_type != current_llm_type) or (
                                            new_type in ("gemini realtime api", "chatgpt realtime api")
                                            and (not active_realtime_session or not active_realtime_session.is_connected)
                                        )
                                        current_llm_type = new_type
                                        client_settings["llm_type"] = current_llm_type
                                        if need_reinit:
                                            asyncio.create_task(switch_realtime_session(current_llm_type))
                                    if "ntp_server" in ctrl_json:
                                        current_ntp_server = str(ctrl_json["ntp_server"])
                                        client_settings["ntp_server"] = current_ntp_server
                                    if "ntp_server_host" in ctrl_json:
                                        client_settings["ntp_server_host"] = str(ctrl_json["ntp_server_host"])
                                    if "timezone" in ctrl_json:
                                        current_timezone = str(ctrl_json["timezone"])
                                        client_settings["timezone"] = current_timezone
                                    if "timezone_posix" in ctrl_json:
                                        current_timezone_posix = str(ctrl_json["timezone_posix"])
                                        client_settings["timezone_posix"] = current_timezone_posix
                                    if "language" in ctrl_json:
                                        current_language = str(ctrl_json["language"])
                                        client_settings["language"] = current_language
                                        if current_language == "en":
                                            VOICE = "en-US-JennyNeural"
                                        else:
                                            VOICE = "ko-KR-SunHiNeural"

                                    print("\n=======================================================")
                                    print(" [클라이언트 설정 동기화] ESP32 최신 설정값 수신 완료:")
                                    print(f"  • 볼륨(Volume)        : {client_settings.get('volume')}%")
                                    print(f"  • 마이크 감도(Gain)   : {client_settings.get('mic_gain')} ({client_settings.get('mic_gain', 35)/10:.1f}x)")
                                    print(f"  • 화면 밝기(Brightness): {client_settings.get('brightness')}%")
                                    print(f"  • LLM 타입(LLM Type)  : {client_settings.get('llm_type')}")
                                    print(f"  • NTP 서버(NTP Server): {client_settings.get('ntp_server')} ({client_settings.get('ntp_server_host', '')})")
                                    print(f"  • 타임존(Timezone)    : {client_settings.get('timezone')} ({client_settings.get('timezone_posix', '')})")
                                    print(f"  • 언어 설정(Language) : {client_settings.get('language')} (TTS Voice: {VOICE})")
                                    print("=======================================================")

                        except Exception as parse_err:
                            print(f"[경고] JSON 파싱 실패 ({parse_err}), 일반 텍스트로 처리합니다.")
                            is_control_packet = False

                    # ★ 순수 텍스트 질의인 경우에만 LLM 처리
                    if not is_control_packet:
                        user_text = raw_text
                        t_speech_end = time.perf_counter()
                        print(f"\n[텍스트 입력 수신 (LLM 질의)] {user_text}")

                        # LLM Type 변경 명령 감지
                        sw_target, sw_name, sw_resp = parse_llm_switch_command(user_text)
                        if sw_target:
                            print(f"\n[텍스트 명령 감지] LLM 변경 요청: '{user_text}' -> '{sw_target}' ({sw_name})")
                            await switch_and_notify_llm(sw_target, sw_name, sw_resp)
                            continue

                        # 볼륨 / 마이크 감도 조절 명령 감지
                        aud_cmd, aud_val, aud_resp = parse_audio_settings_command(user_text, current_volume, current_mic_gain)
                        if aud_cmd:
                            print(f"\n[텍스트 명령 감지] 오디오 설정 변경: '{user_text}' -> {aud_cmd}={aud_val}")
                            await adjust_audio_settings(aud_cmd, aud_val, aud_resp)
                            continue

                        # 텍스트 시간 질의 감지
                        if is_time_query(user_text):
                            print(f"\n[텍스트 시간 질의] '{user_text}' -> ESP32 LCD 시계 표시 명령(12초) 전송!")
                            try:
                                await websocket.send(json.dumps({"type": "show_clock", "duration": 12}))
                            except Exception:
                                pass
                            if len(user_text.strip()) <= 25:
                                time_speech = get_korean_time_speech()
                                ai_task = asyncio.create_task(
                                    process_ai_response(websocket, user_text, chat_history, t_speech_end, 0.0, interrupted_holder=interrupted_holder, direct_answer=time_speech)
                                )
                                continue

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
