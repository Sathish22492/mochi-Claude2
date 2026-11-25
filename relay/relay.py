#!/usr/bin/env python3
"""Mochi relay: bridges the ESP32 device to a free (or paid) LLM.

device mic audio --> VAD --> faster-whisper (local STT) --> "Hey Mochi" check
   --> LLM (free Groq / Gemini / OpenRouter / local Ollama, or Claude) --> Piper (local TTS)
   --> audio + face commands back to device
Audio never leaves your network; only the text of your question goes to the LLM provider.
"""
import asyncio
import difflib
import json
import logging
import os
import re
import socket
import string
import tempfile
import time
import wave
from collections import deque
from datetime import datetime
from math import gcd

import numpy as np
import websockets
from dotenv import load_dotenv
from faster_whisper import WhisperModel
from scipy.signal import resample_poly

load_dotenv()
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
log = logging.getLogger("mochi")

# ------------------------------------------------------------------ config
def env(name, default):
    return os.environ.get(name, default)

HOST = env("RELAY_HOST", "0.0.0.0")
PORT = int(env("RELAY_PORT", "8765"))
PROVIDER = env("LLM_PROVIDER", "groq").lower()        # groq | gemini | openrouter | ollama | custom | claude
PROVIDERS = {                                           # OpenAI-compatible endpoints
    "groq":       ("https://api.groq.com/openai/v1", "GROQ_API_KEY", "llama-3.3-70b-versatile"),
    "gemini":     ("https://generativelanguage.googleapis.com/v1beta/openai/", "GEMINI_API_KEY", "gemini-2.5-flash"),
    "openrouter": ("https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", "meta-llama/llama-3.3-70b-instruct:free"),
    "ollama":     ("http://localhost:11434/v1", "", "llama3.2:3b"),
    "custom":     (env("LLM_BASE_URL", ""), "LLM_API_KEY", ""),
    "claude":     ("", "ANTHROPIC_API_KEY", "claude-haiku-5-5"),
}
MODEL = env("LLM_MODEL", "") or PROVIDERS.get(PROVIDER, ("", "", ""))[2]
MAX_TOKENS = int(env("LLM_MAX_TOKENS", "300"))
WHISPER_MODEL = env("WHISPER_MODEL", "base.en")
WHISPER_LANG = env("WHISPER_LANG", "en") or None      # empty = auto-detect
PIPER_BIN = env("PIPER_BIN", "piper")
PIPER_VOICE = env("PIPER_VOICE", "en_US-lessac-medium.onnx")
WAKE_WORDS = {w.strip() for w in env("WAKE_WORDS", "mochi,mochy,mochie,mocha,moji,mochee,mouchi").split(",") if w.strip()}
WAKE_TARGET = env("WAKE_TARGET", "mochi")
FOLLOWUP_WINDOW = float(env("FOLLOWUP_WINDOW", "8"))   # seconds after a reply you can talk without the wake word
VOLUME = int(env("DEVICE_VOLUME", "70"))
PERSONA_EXTRA = env("PERSONA_EXTRA", "")
MIN_RMS = float(env("MIN_RMS", "400"))                 # raise if it triggers on noise, lower if it ignores you
END_SILENCE = float(env("END_SILENCE", "0.9"))
DEBUG_LEVELS = env("DEBUG_LEVELS", "0") == "1"

SAMPLE_RATE = 16000
BYTES_PER_SEC = SAMPLE_RATE * 2
GREETINGS = {"hey", "hi", "hello", "hay", "okay", "ok", "oh", "yo", "hei", "a"}

SYSTEM_PROMPT = """You are Mochi, a small, friendly desk companion with a cute face on a tiny screen. \
Everything you write is spoken aloud by a speaker, so: keep replies short (one to three sentences unless asked for more), \
use plain spoken language, and never use markdown, lists, emoji, or URLs. \
Start every reply with exactly one emotion tag in square brackets, chosen from \
[neutral] [happy] [sad] [surprised] [sleepy] [love] [angry], for example: "[happy] Sure, I can help with that." \
Be warm, curious and honest. You cannot browse the web or control other devices; if asked, say so briefly. \
If you are not sure about something, say so rather than guessing."""

# ------------------------------------------------------------------ helpers
EMOJI = re.compile("[\U0001F300-\U0001FAFF\u2600-\u27BF\uFE0F]")
TAG = re.compile(r"^\s*\[(\w+)\]\s*")
SENT_END = re.compile(r"(?<=[.!?…])\s+|\n+")
EMOTIONS = {"neutral", "happy", "sad", "surprised", "sleepy", "love", "angry"}


def clean_for_speech(text: str) -> str:
    text = TAG.sub("", text)
    text = re.sub(r"\[\w+\]", "", text)
    text = re.sub(r"[*_#`~>|]", "", text)
    text = EMOJI.sub("", text)
    return re.sub(r"\s+", " ", text).strip()


def pop_sentence(buf: str, min_chars: int = 12):
    for m in SENT_END.finditer(buf):
        if m.start() >= min_chars:
            return buf[: m.start()], buf[m.end():]
    return None, buf


def find_wake(text: str):
    """Return the text after the wake word, '' if only the wake word was said, None if no wake word."""
    toks = text.split()
    clean = [t.strip(string.punctuation + "…“”‘’").lower() for t in toks]
    for i, c in enumerate(clean[:4]):
        if not c:
            continue
        consumed = 0
        if c in WAKE_WORDS or difflib.SequenceMatcher(None, c, WAKE_TARGET).ratio() >= 0.8:
            consumed = 1
        elif i + 1 < len(clean) and difflib.SequenceMatcher(None, c + clean[i + 1], WAKE_TARGET).ratio() >= 0.8:
            consumed = 2  # "mo chi"
        if consumed and (i == 0 or clean[i - 1] in GREETINGS):
            return " ".join(toks[i + consumed:]).lstrip(",.!? ")
    return None


def lan_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "your-computer-ip"


# ------------------------------------------------------------------ voice activity detection
class Vad:
    def __init__(self):
        self.reset()

    def reset(self):
        self.pre = deque(maxlen=10)
        self.buf = []
        self.active = False
        self.start_cnt = 0
        self.speech_s = 0.0
        self.silence_s = 0.0
        self.noise = 200.0
        self._last_dbg = 0.0

    def feed(self, frame: bytes):
        if len(frame) < 2:
            return None
        x = np.frombuffer(frame[: len(frame) // 2 * 2], dtype=np.int16).astype(np.float32)
        rms = float(np.sqrt(np.mean(x * x)))
        dur = len(x) / SAMPLE_RATE
        thr = max(self.noise * 3.0, MIN_RMS)
        if DEBUG_LEVELS and time.time() - self._last_dbg > 1.0:
            self._last_dbg = time.time()
            log.info("mic level rms=%.0f threshold=%.0f", rms, thr)

        if not self.active:
            self.pre.append(frame)
            if rms > thr:
                self.start_cnt += 1
            else:
                self.start_cnt = 0
                self.noise = 0.97 * self.noise + 0.03 * rms
            if self.start_cnt >= 2:
                self.active = True
                self.buf = list(self.pre)
                self.pre.clear()
                self.speech_s = 0.0
                self.silence_s = 0.0
            return None

        self.buf.append(frame)
        if rms > thr * 0.7:
            self.silence_s = 0.0
            self.speech_s += dur
        else:
            self.silence_s += dur
        total = sum(len(b) for b in self.buf) / BYTES_PER_SEC
        if self.silence_s >= END_SILENCE or total >= 15.0:
            utter = b"".join(self.buf)
            speech = self.speech_s
            self.reset_keep_noise()
            return utter if speech >= 0.35 else None
        return None

    def reset_keep_noise(self):
        n = self.noise
        self.reset()
        self.noise = n


# ------------------------------------------------------------------ speech-to-text
class Stt:
    def __init__(self):
        log.info("loading Whisper model '%s' (first run downloads it)...", WHISPER_MODEL)
        self.model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")

    def _run(self, pcm: bytes) -> str:
        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
        segments, _ = self.model.transcribe(
            audio,
            language=WHISPER_LANG,
            beam_size=1,
            vad_filter=True,
            condition_on_previous_text=False,
            initial_prompt="Hey Mochi.",
        )
        out = []
        for s in segments:
            if s.no_speech_prob < 0.6:
                out.append(s.text.strip())
        return " ".join(out).strip()

    async def transcribe(self, pcm: bytes) -> str:
        return await asyncio.get_running_loop().run_in_executor(None, self._run, pcm)


# ------------------------------------------------------------------ text-to-speech (Piper, persistent process)
class Tts:
    def __init__(self):
        self.proc = None
        self.lock = asyncio.Lock()
        self.tmp = tempfile.mkdtemp(prefix="mochi_tts_")

    async def _start(self):
        self.proc = await asyncio.create_subprocess_exec(
            PIPER_BIN, "--model", PIPER_VOICE, "--output_dir", self.tmp,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
        )

    async def synth(self, text: str) -> np.ndarray:
        async with self.lock:
            if self.proc is None or self.proc.returncode is not None:
                await self._start()
            self.proc.stdin.write((text.replace("\n", " ") + "\n").encode("utf-8"))
            await self.proc.stdin.drain()
            path = None
            for _ in range(6):
                line = await asyncio.wait_for(self.proc.stdout.readline(), timeout=40)
                if not line:
                    self.proc = None
                    raise RuntimeError("Piper exited unexpectedly (check PIPER_BIN / PIPER_VOICE)")
                s = line.decode("utf-8", errors="ignore").strip()
                if s.lower().endswith(".wav"):
                    path = s
                    break
            if not path:
                raise RuntimeError("Piper produced no audio file")
        with wave.open(path, "rb") as w:
            sr, ch = w.getframerate(), w.getnchannels()
            raw = w.readframes(w.getnframes())
        try:
            os.remove(path)
        except OSError:
            pass
        audio = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        if ch > 1:
            audio = audio.reshape(-1, ch).mean(axis=1)
        if sr != SAMPLE_RATE:
            g = gcd(SAMPLE_RATE, sr)
            audio = resample_poly(audio, SAMPLE_RATE // g, sr // g)
        return np.clip(audio, -32768, 32767).astype(np.int16)


# ------------------------------------------------------------------ LLM (provider-agnostic)
class Llm:
    def __init__(self):
        if PROVIDER not in PROVIDERS:
            raise SystemExit(f"Unknown LLM_PROVIDER '{PROVIDER}'. Use one of: {', '.join(PROVIDERS)}")
        base_url, key_env, _ = PROVIDERS[PROVIDER]
        key = os.environ.get(key_env, "") if key_env else "ollama"
        if key_env and not key:
            raise SystemExit(f"{key_env} is not set. Put it in relay/.env (see .env.example).")
        if not MODEL:
            raise SystemExit("LLM_MODEL is not set. Put a model name in relay/.env.")
        if PROVIDER == "claude":
            from anthropic import AsyncAnthropic
            self.client = AsyncAnthropic(api_key=key)
        else:
            from openai import AsyncOpenAI
            self.client = AsyncOpenAI(base_url=base_url, api_key=key)
        log.info("LLM: %s / %s", PROVIDER, MODEL)

    async def stream(self, system: str, messages: list):
        """Yield pieces of reply text as they arrive."""
        if PROVIDER == "claude":
            async with self.client.messages.stream(
                model=MODEL, max_tokens=MAX_TOKENS, system=system, messages=messages
            ) as st:
                async for t in st.text_stream:
                    yield t
        else:
            st = await self.client.chat.completions.create(
                model=MODEL, max_tokens=MAX_TOKENS, stream=True,
                messages=[{"role": "system", "content": system}] + messages,
            )
            async for ev in st:
                if ev.choices and ev.choices[0].delta.content:
                    yield ev.choices[0].delta.content


# ------------------------------------------------------------------ one device session
class Session:
    def __init__(self, ws, stt: Stt, tts: Tts, llm: Llm):
        self.ws, self.stt, self.tts, self.llm = ws, stt, tts, llm
        self.vad = Vad()
        self.history = []
        self.task = None
        self.busy = False
        self.ptt = False
        self.awake_until = 0.0
        self.window_token = 0
        self.t0 = None
        self.sent_bytes = 0

    async def send_json(self, **kw):
        try:
            await self.ws.send(json.dumps(kw))
        except websockets.ConnectionClosed:
            pass

    async def run(self):
        try:
            async for msg in self.ws:
                if isinstance(msg, bytes):
                    if self.busy:
                        continue
                    utter = self.vad.feed(msg)
                    if utter:
                        self.busy = True
                        self.task = asyncio.create_task(self.handle_utterance(utter))
                else:
                    await self.on_text(msg)
        except websockets.ConnectionClosed:
            pass
        finally:
            if self.task:
                self.task.cancel()

    async def on_text(self, msg: str):
        try:
            d = json.loads(msg)
        except ValueError:
            return
        t = d.get("t")
        if t == "hello":
            log.info("device connected (firmware %s)", d.get("fw"))
            await self.send_json(t="volume", v=VOLUME)
            await self.send_json(t="state", v="idle")
        elif t == "ptt":
            log.info("push-to-talk")
            if self.task and not self.task.done():
                self.task.cancel()
            self.busy = False
            self.vad.reset_keep_noise()
            self.ptt = True
            await self.send_json(t="stop")
            await self.send_json(t="state", v="listening")

    # ---- audio out, paced so the device buffer never overflows
    async def send_pcm(self, pcm: bytes):
        loop = asyncio.get_running_loop()
        chunk = 1024
        for i in range(0, len(pcm), chunk):
            c = pcm[i: i + chunk]
            now = loop.time()
            if self.t0 is None or now - self.t0 > self.sent_bytes / BYTES_PER_SEC + 0.05:
                self.t0, self.sent_bytes = now, 0          # device ran dry: restart the clock
            ahead = self.sent_bytes / BYTES_PER_SEC - (now - self.t0)
            if ahead > 0.6:
                await asyncio.sleep(ahead - 0.6)
            await self.ws.send(c)
            self.sent_bytes += len(c)

    async def handle_utterance(self, pcm: bytes):
        try:
            text = await self.stt.transcribe(pcm)
            if not text:
                return
            log.info("heard: %s", text)

            now = time.time()
            query = None
            if self.ptt or now < self.awake_until:
                query = text
                w = find_wake(text)
                if w is not None:
                    query = w
            else:
                query = find_wake(text)
            self.ptt = False
            if query is None:
                return                                   # not for me
            if not query.strip():
                await self.send_json(t="emotion", v="surprised")
                await self.send_json(t="state", v="speaking")
                pcm_out = await self.tts.synth("Yes?")
                await self.send_pcm(pcm_out.tobytes())
                await self.send_json(t="audio_end")
                self.open_window()
                await self.send_json(t="state", v="listening")
                return

            await self.send_json(t="state", v="thinking")
            await self.ask_llm(query)
            self.open_window()
            await self.send_json(t="state", v="listening")
            asyncio.create_task(self.close_window_later(self.window_token))
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("error while handling utterance")
            try:
                await self.send_json(t="emotion", v="sad")
                await self.send_json(t="state", v="idle")
            except Exception:
                pass
        finally:
            self.busy = False
            self.vad.reset_keep_noise()

    def open_window(self):
        self.awake_until = time.time() + FOLLOWUP_WINDOW
        self.window_token += 1

    async def close_window_later(self, token: int):
        await asyncio.sleep(FOLLOWUP_WINDOW + 3.0)   # + time for the reply to finish playing
        if token == self.window_token and not self.busy:
            await self.send_json(t="state", v="idle")

    # ---- LLM -> sentences -> speech
    async def ask_llm(self, user_text: str):
        q: asyncio.Queue = asyncio.Queue()
        reply_parts = []
        system = SYSTEM_PROMPT + f"\nThe current local date and time is {datetime.now().strftime('%A, %d %B %Y, %H:%M')}."
        if PERSONA_EXTRA:
            system += "\n" + PERSONA_EXTRA
        messages = self.history[-12:] + [{"role": "user", "content": user_text}]

        async def producer():
            buf, tag_done = "", False
            try:
                async for chunk in self.llm.stream(system, messages):
                    buf += chunk
                    if not tag_done:
                        stripped = buf.lstrip()
                        if stripped.startswith("["):
                            if "]" not in stripped:
                                continue
                            m = TAG.match(buf)
                            if m:
                                emo = m.group(1).lower()
                                await self.send_json(t="emotion", v=emo if emo in EMOTIONS else "neutral")
                                buf = buf[m.end():]
                        tag_done = True
                    while True:
                        sent, buf = pop_sentence(buf)
                        if sent is None:
                            break
                        reply_parts.append(sent)
                        await q.put(sent)
                if buf.strip():
                    reply_parts.append(buf)
                    await q.put(buf)
            except asyncio.CancelledError:
                raise
            except Exception as e:  # network, auth, rate limit...
                log.error("LLM error (%s): %s", PROVIDER, e)
                await self.send_json(t="emotion", v="sad")
                await q.put("Sorry, I could not reach my brain just now.")
            finally:
                await q.put(None)

        async def consumer():
            first = True
            while True:
                item = await q.get()
                if item is None:
                    break
                text = clean_for_speech(item)
                if not text:
                    continue
                log.info("say: %s", text)
                pcm = await self.tts.synth(text)
                if first:
                    await self.send_json(t="state", v="speaking")
                    first = False
                await self.send_pcm(pcm.tobytes())

        prod = asyncio.create_task(producer())
        try:
            await consumer()
        finally:
            prod.cancel()
        await self.send_json(t="audio_end")
        said = clean_for_speech(" ".join(reply_parts))
        if said:
            self.history += [{"role": "user", "content": user_text}, {"role": "assistant", "content": said}]
            self.history = self.history[-12:]


# ------------------------------------------------------------------ main
async def main():
    llm = Llm()
    stt = Stt()
    tts = Tts()
    log.info("warming up text-to-speech...")
    await tts.synth("Hello")

    async def handler(ws):
        await Session(ws, stt, tts, llm).run()

    async with websockets.serve(handler, HOST, PORT, max_size=2**20, ping_interval=None):
        log.info("Relay ready. In the device setup page enter  IP: %s   port: %d", lan_ip(), PORT)
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
