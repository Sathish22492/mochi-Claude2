# Mochi (XH-S3E-AI_V1)

OLED Mochi face + built-in mic/speaker, talking to an LLM (free Groq by default; Gemini, OpenRouter, Ollama or Claude also work) through a small relay on your PC / Raspberry Pi.

```
[XH-S3E-AI]  --mic audio over WiFi-->  [relay.py on PC/Pi]  --text-->  LLM API
 OLED face   <--speech audio + face--   wake word, Whisper STT, Piper TTS
```
Speech-to-text and text-to-speech run locally and free. Your API key lives only on the relay. The device never sees it.

## 1. Relay (PC or Pi, same WiFi as the device)
**Windows 11 shortcut:** double click `relay\setup.bat` (installs Python via winget if missing, downloads Piper and a voice, asks for your free Groq key, starts the relay). Next time just double click `run.bat`.

Manual steps:
1. Install Python 3.10+.
2. `cd relay && pip install -r requirements.txt`
3. Download **Piper** (release zip for your OS) and one voice (`.onnx` + `.onnx.json`).
4. Get a free Groq key at console.groq.com/keys (or see `.env.example` for other providers).
5. Copy `.env.example` to `.env`, fill in key + Piper paths.
6. `python relay.py`. It prints the IP address to give the device. Allow it through the firewall (TCP 8765) if asked.

## 2. Firmware (.bin) without installing a toolchain
1. Create a **private** GitHub repo and upload this whole folder.
2. Open the **Actions** tab, run "Build Mochi firmware" (or it runs on push), then download the `mochi-claude-factory` artifact (zip containing `mochi-claude-factory.bin`).
3. Flash it with https://web.esphome.io (Chrome/Edge): Connect, pick the port, **Install**, choose `mochi-claude-factory.bin`. It is a full image and goes at 0x0, same as your reference ROM. Do NOT choose "Prepare for first use".
4. Keep your original `bread-compact-wifi-sophia-oled.bin` so you can always go back.

## 3. First boot
The screen shows "Mochi". Press BOOT within 3 s (or let it auto-open when unconfigured) to get **WiFi setup**: join the `Mochi-Setup` WiFi on your phone, open 192.168.4.1, enter WiFi name/password and the relay PC's IP. It saves and restarts. No rebuild needed to change these later.

## 4. Using it
Say **"Hey Mochi, what's the capital of France?"**. Say just "Hey Mochi" and it answers "Yes?", then you ask. After each reply you have ~8 s to follow up without the wake word. Tap BOOT to interrupt it or to talk without the wake word.

## Troubleshooting
| Symptom | Fix |
|---|---|
| Face works, never reacts | Set `DEBUG_LEVELS=1` in `.env` and watch the mic level numbers. Flat zero: mic pins/channel wrong in `config.h`. Too low: lower `MIC_SHIFT` (13) or `MIN_RMS`. |
| Hears you, no sound out | Speaker pins in `config.h` (or `PIN_SPK_EN` if the amp needs enabling). |
| Wake word often missed | Say "Hey Mochi" clearly, add spellings Whisper produces to `WAKE_WORDS`, use `WHISPER_MODEL=small.en`. |
| Triggers on noise | Raise `MIN_RMS`. |
| Wrong pins entirely | Pins in `config.h` are my assumption (stock bread-compact-wifi layout), change and rebuild. |
