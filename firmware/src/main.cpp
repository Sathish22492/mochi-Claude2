// Mochi-Claude firmware: cute OLED face + mic/speaker thin client for a Claude relay.
// Device streams mic audio to the relay over a WebSocket; the relay does wake word,
// speech-to-text, Claude, text-to-speech, and streams speech audio + face commands back.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "config.h"

// ------------------------------------------------------------------ globals
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
WebSocketsClient ws;
Preferences prefs;
StreamBufferHandle_t playBuf;

enum Face : uint8_t { F_BOOT, F_IDLE, F_LISTEN, F_THINK, F_SPEAK, F_NOLINK, F_SETUP };
enum Emo : uint8_t { E_NEUTRAL, E_HAPPY, E_SAD, E_SURPRISED, E_SLEEPY, E_LOVE, E_ANGRY };

volatile Face faceState = F_BOOT;
volatile Emo emo = E_NEUTRAL;
volatile bool wifiOk = false;       // false = still joining WiFi, true = WiFi up, waiting for relay
volatile uint32_t emoClearAt = 0;
volatile bool speaking = false;     // audio is arriving / playing -> mic is muted
volatile bool audioEnded = false;   // relay said the last sentence was sent
volatile bool flushReq = false;     // ask play task to drop buffered audio
volatile bool wsUp = false;
volatile uint32_t lastAudioMs = 0;
volatile uint32_t micResumeAt = 0;
volatile float mouthLevel = 0;
volatile int volumePct = DEFAULT_VOLUME;

String cfgSsid, cfgPass, cfgHost;
uint16_t cfgPort = RELAY_PORT_DEFAULT;

// ------------------------------------------------------------------ status LED
static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  if (PIN_LED >= 0) neopixelWrite(PIN_LED, r, g, b);
}

// ------------------------------------------------------------------ face drawing
static void drawHeart(int cx, int cy, int s) {
  u8g2.drawDisc(cx - s / 2 - 1, cy - 3, s / 2 + 1);
  u8g2.drawDisc(cx + s / 2 + 1, cy - 3, s / 2 + 1);
  u8g2.drawTriangle(cx - s - 2, cy - 1, cx + s + 2, cy - 1, cx, cy + s + 2);
}

static void drawEye(int cx, int cy, int w, int h, Emo e, bool left, bool blink) {
  if (e == E_HAPPY) {  // ^ ^
    for (int d = 0; d < 3; d++) {
      u8g2.drawLine(cx - 12, cy + 6 + d, cx, cy - 6 + d);
      u8g2.drawLine(cx, cy - 6 + d, cx + 12, cy + 6 + d);
    }
    return;
  }
  if (e == E_LOVE) { drawHeart(cx, cy, 9); return; }
  if (blink) { u8g2.drawRBox(cx - w / 2, cy - 2, w, 4, 2); return; }
  u8g2.drawRBox(cx - w / 2, cy - h / 2, w, h, 8);
  u8g2.setDrawColor(0);
  if (e == E_SLEEPY) {
    u8g2.drawBox(cx - w / 2 - 1, cy - h / 2 - 1, w + 2, h / 2 + 1);
  } else if (e == E_SAD || e == E_ANGRY) {
    // sad: outer top corners droop; angry: inner top corners drop
    bool cutRight = (e == E_ANGRY) ? left : !left;
    int x0 = cutRight ? cx + w / 2 + 1 : cx - w / 2 - 1;
    int x1 = cutRight ? cx + w / 2 - (w * 7) / 10 : cx - w / 2 + (w * 7) / 10;
    int y0 = cy - h / 2 - 1;
    u8g2.drawTriangle(x0, y0, x1, y0, x0, y0 + 13);
  }
  u8g2.setDrawColor(1);
}

static void drawMouth(Face f, Emo e, bool spk) {
  const int mx = 64;
  if (spk) {
    int mh = 2 + (int)(mouthLevel * 14.0f);
    u8g2.drawFilledEllipse(mx, 49, 9, mh);
    return;
  }
  if (f == F_LISTEN) { u8g2.drawCircle(mx, 50, 3); return; }
  if (f == F_THINK) { u8g2.drawHLine(mx - 6, 50, 12); return; }
  switch (e) {
    case E_HAPPY:     u8g2.drawDisc(mx, 41, 10, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT); break;
    case E_SAD:       u8g2.drawCircle(mx, 55, 8, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT); break;
    case E_SURPRISED: u8g2.drawCircle(mx, 50, 5); break;
    case E_SLEEPY:    u8g2.drawHLine(mx - 5, 50, 10); break;
    case E_ANGRY:     u8g2.drawHLine(mx - 8, 50, 16); break;
    default:          u8g2.drawCircle(mx, 43, 7, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT); break;
  }
}

static void drawFace() {
  uint32_t now = millis();
  Face f = faceState;
  u8g2.clearBuffer();

  if (f == F_SETUP) {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "WiFi setup mode");
    u8g2.drawStr(0, 26, "1. Join WiFi:");
    u8g2.drawStr(0, 38, "   Mochi-Setup");
    u8g2.drawStr(0, 54, "2. Open 192.168.4.1");
    u8g2.sendBuffer();
    return;
  }
  if (f == F_BOOT) {
    u8g2.setFont(u8g2_font_helvB14_tr);
    u8g2.drawStr(34, 30, "Mochi");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(8, 52, "BOOT btn = WiFi setup");
    u8g2.sendBuffer();
    return;
  }

  bool spk = speaking;
  Emo e = emo;
  if (e != E_NEUTRAL && !spk && emoClearAt && now > emoClearAt) { emo = E_NEUTRAL; e = E_NEUTRAL; }
  if (f == F_NOLINK) e = E_SLEEPY;

  static uint32_t nextBlink = 0, blinkEnd = 0;
  if (now > nextBlink) { blinkEnd = now + 140; nextBlink = now + random(2200, 5500); }
  bool blink = now < blinkEnd;

  int w = 22, h = 28, dx = 0, dy = 0;
  if (f == F_LISTEN && !spk) { w = 24; h = 32; }
  if (f == F_THINK && !spk) { dx = (int)(sinf(now / 450.0f) * 9.0f); dy = -2; }
  if (e == E_SURPRISED) { w = 26; h = 34; }

  drawEye(38 + dx, 24 + dy, w, h, e, true, blink);
  drawEye(90 + dx, 24 + dy, w, h, e, false, blink);
  drawMouth(f, e, spk);

  if (f == F_THINK && !spk) {
    int ph = (now / 300) % 4;
    for (int i = 0; i < 3; i++) if (i < ph) u8g2.drawDisc(54 + i * 10, 60, 2);
  }
  if (f == F_NOLINK) {
    u8g2.setFont(u8g2_font_5x7_tr);
    if (!wifiOk) {
      u8g2.drawStr(30, 63, "joining WiFi...");
    } else {
      String line = "relay " + cfgHost + ":" + String(cfgPort);
      u8g2.drawStr(0, 63, line.c_str());
    }
  }
  if (e == E_SLEEPY && f != F_NOLINK) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(108, 10, "z");
  }
  u8g2.sendBuffer();
}

static void displayTask(void*) {
  for (;;) {
    drawFace();
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

// ------------------------------------------------------------------ audio
static void initMic() {
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = MIC_CHANNEL;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_MIC_SCK;
  pins.ws_io_num = PIN_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_MIC_DIN;
  i2s_set_pin(I2S_NUM_0, &pins);
}

static void initSpeaker() {
  if (PIN_SPK_EN >= 0) { pinMode(PIN_SPK_EN, OUTPUT); digitalWrite(PIN_SPK_EN, HIGH); }
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;  // we write interleaved stereo (mono duplicated)
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true;  // silence on underrun instead of buzzing
  i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_SPK_BCLK;
  pins.ws_io_num = PIN_SPK_LRC;
  pins.data_out_num = PIN_SPK_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);
}

static void endSpeaking(uint32_t now) {
  speaking = false;
  audioEnded = false;
  micResumeAt = now + 400;
  emoClearAt = now + 4000;
}

static void playTask(void*) {
  static int16_t mono[256];
  static int16_t st[512];
  for (;;) {
    if (flushReq) {
      while (xStreamBufferReceive(playBuf, mono, sizeof(mono), 0) > 0) {}
      i2s_zero_dma_buffer(I2S_NUM_1);
      endSpeaking(millis());
      flushReq = false;
    }
    size_t n = xStreamBufferReceive(playBuf, mono, sizeof(mono), pdMS_TO_TICKS(40));
    uint32_t now = millis();
    if (n >= 2) {
      size_t ns = n / 2;
      int vol = volumePct;
      float acc = 0;
      for (size_t i = 0; i < ns; i++) {
        int32_t s = ((int32_t)mono[i] * vol) / 100;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        st[2 * i] = st[2 * i + 1] = (int16_t)s;
        acc += (float)mono[i] * (float)mono[i];
      }
      float lv = sqrtf(acc / ns) / 6000.0f;
      if (lv > 1.0f) lv = 1.0f;
      mouthLevel = mouthLevel * 0.5f + lv * 0.5f;
      size_t wr = 0;
      i2s_write(I2S_NUM_1, st, ns * 4, &wr, portMAX_DELAY);
      lastAudioMs = now;
    } else {
      mouthLevel = mouthLevel * 0.85f;
      if (speaking) {
        if (audioEnded && now - lastAudioMs > 400) {
          i2s_zero_dma_buffer(I2S_NUM_1);
          endSpeaking(now);
        } else if (now - lastAudioMs > 6000) {
          endSpeaking(now);  // safety: relay never finished
        }
      }
    }
  }
}

// ------------------------------------------------------------------ websocket
static Emo parseEmo(const char* s) {
  if (!strcmp(s, "happy")) return E_HAPPY;
  if (!strcmp(s, "sad")) return E_SAD;
  if (!strcmp(s, "surprised")) return E_SURPRISED;
  if (!strcmp(s, "sleepy")) return E_SLEEPY;
  if (!strcmp(s, "love")) return E_LOVE;
  if (!strcmp(s, "angry")) return E_ANGRY;
  return E_NEUTRAL;
}

static void handleJson(uint8_t* payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  const char* t = doc["t"] | "";
  const char* v = doc["v"] | "";
  if (!strcmp(t, "state")) {
    if (!strcmp(v, "listening")) faceState = F_LISTEN;
    else if (!strcmp(v, "thinking")) faceState = F_THINK;
    else if (!strcmp(v, "speaking")) faceState = F_SPEAK;
    else faceState = F_IDLE;
  } else if (!strcmp(t, "emotion")) {
    emo = parseEmo(v);
    emoClearAt = 0;
  } else if (!strcmp(t, "audio_end")) {
    audioEnded = true;
  } else if (!strcmp(t, "volume")) {
    int vol = doc["v"] | DEFAULT_VOLUME;
    volumePct = constrain(vol, 0, 100);
  } else if (!strcmp(t, "stop")) {
    flushReq = true;
  }
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsUp = true;
      faceState = F_IDLE;
      setLed(0, 40, 0);
      Serial.println("[ws] connected to relay");
      ws.sendTXT("{\"t\":\"hello\",\"fw\":\"" FW_VERSION "\"}");
      break;
    case WStype_DISCONNECTED:
      wsUp = false;
      faceState = F_NOLINK;
      flushReq = true;
      setLed(40, 25, 0);
      Serial.println("[ws] not connected to relay (is relay.py running? right IP? firewall?)");
      break;
    case WStype_TEXT:
      handleJson(payload, length);
      break;
    case WStype_BIN:
      if (length >= 2) {
        speaking = true;
        audioEnded = false;
        lastAudioMs = millis();
        xStreamBufferSend(playBuf, payload, length & ~(size_t)1, 0);
      }
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------------ setup portal
static void loadConfig() {
  prefs.begin("mochi", true);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgHost = prefs.getString("host", "");
  cfgPort = prefs.getUShort("port", RELAY_PORT_DEFAULT);
  prefs.end();
}

[[noreturn]] static void runSetupPortal() {
  faceState = F_SETUP;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Mochi-Setup");
  static WebServer server(80);
  server.on("/", HTTP_GET, []() {
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<body style='font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em'>"
                 "<h2>Mochi setup</h2><form method=post action=/save>"
                 "WiFi name<br><input name=ssid style='width:100%;padding:8px' value='");
    h += cfgSsid;
    h += F("'><br><br>WiFi password<br><input name=pass type=password style='width:100%;padding:8px'>"
           "<br><br>Relay computer IP address (e.g. 192.168.1.50)<br>"
           "<input name=host style='width:100%;padding:8px' value='");
    h += cfgHost;
    h += F("'><br><br>Relay port<br><input name=port style='width:100%;padding:8px' value='");
    h += String(cfgPort);
    h += F("'><br><br><button style='padding:10px 20px'>Save and restart</button></form>");
    server.send(200, "text/html", h);
  });
  server.on("/save", HTTP_POST, []() {
    prefs.begin("mochi", false);
    if (server.arg("ssid").length()) prefs.putString("ssid", server.arg("ssid"));
    if (server.arg("pass").length()) prefs.putString("pass", server.arg("pass"));
    prefs.putString("host", server.arg("host"));
    int p = server.arg("port").toInt();
    prefs.putUShort("port", p > 0 ? p : RELAY_PORT_DEFAULT);
    prefs.end();
    server.send(200, "text/html", "<body style='font-family:sans-serif'><h3>Saved. Restarting...</h3>");
    delay(1000);
    ESP.restart();
  });
  server.begin();
  for (;;) {
    server.handleClient();
    delay(5);
  }
}

// ------------------------------------------------------------------ button
static void handleButton() {
  static bool last = true;
  static uint32_t pressT = 0;
  bool b = digitalRead(PIN_BUTTON);
  if (!b && last) pressT = millis();
  if (b && !last) {
    uint32_t d = millis() - pressT;
    if (d > 30 && d < 1500) {
      flushReq = true;  // interrupt any speech
      if (wsUp) ws.sendTXT("{\"t\":\"ptt\"}");
    }
  }
  last = b;
}

// ------------------------------------------------------------------ arduino
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  u8g2.begin();
  Wire.setClock(400000);
  xTaskCreatePinnedToCore(displayTask, "face", 4096, NULL, 1, NULL, 0);

  loadConfig();

  // 3 s window to press BOOT for setup (don't hold it while powering on!)
  faceState = F_BOOT;
  uint32_t t0 = millis();
  bool wantSetup = false;
  while (millis() - t0 < 3000) {
    if (digitalRead(PIN_BUTTON) == LOW) { wantSetup = true; break; }
    delay(20);
  }
  if (wantSetup || cfgSsid.isEmpty() || cfgHost.isEmpty()) runSetupPortal();

  faceState = F_NOLINK;
  wifiOk = false;
  setLed(40, 0, 0);
  Serial.printf("[wifi] joining '%s'\n", cfgSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] failed, opening setup portal");
    runSetupPortal();  // wrong password, 5 GHz-only network, etc.
  }
  wifiOk = true;
  setLed(40, 25, 0);
  Serial.printf("[wifi] connected, device IP %s, relay %s:%u\n",
                WiFi.localIP().toString().c_str(), cfgHost.c_str(), cfgPort);

  playBuf = xStreamBufferCreate(PLAY_BUFFER_BYTES, 1);
  initMic();
  initSpeaker();
  xTaskCreatePinnedToCore(playTask, "play", 4096, NULL, 3, NULL, 1);

  ws.begin(cfgHost.c_str(), cfgPort, "/ws");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 3000, 2);
}

void loop() {
  static int32_t raw[FRAME_SAMPLES];
  static int16_t pcm[FRAME_SAMPLES];

  ws.loop();
  handleButton();

  size_t got = 0;
  i2s_read(I2S_NUM_0, raw, sizeof(raw), &got, pdMS_TO_TICKS(100));
  size_t n = got / 4;
  if (n == 0 || !wsUp || speaking || millis() < micResumeAt) return;
  for (size_t i = 0; i < n; i++) {
    int32_t s = raw[i] >> MIC_SHIFT;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    pcm[i] = (int16_t)s;
  }
  ws.sendBIN((uint8_t*)pcm, n * 2);
}
