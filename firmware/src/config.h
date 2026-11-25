#pragma once
#define FW_VERSION "0.1.0"

// ---- OLED (SSD1306 128x64, I2C): from your wiring diagram ----
#define PIN_OLED_SDA 41
#define PIN_OLED_SCL 42

// ---- Microphone (I2S RX) ----
// ASSUMPTION: same layout as the stock "bread-compact-wifi" XiaoZhi build.
// If the mic is silent, check these first.
#define PIN_MIC_WS   4
#define PIN_MIC_SCK  5
#define PIN_MIC_DIN  6
#define MIC_CHANNEL  I2S_CHANNEL_FMT_ONLY_LEFT   // try I2S_CHANNEL_FMT_ONLY_RIGHT if silent
#define MIC_SHIFT    14                           // 16 = quiet, 14 = x4, 13 = x8

// ---- Speaker amplifier (I2S TX) ----
#define PIN_SPK_BCLK 15
#define PIN_SPK_LRC  16
#define PIN_SPK_DOUT 7
#define PIN_SPK_EN   -1    // set to a GPIO number if the amp has an enable pin

#define PIN_LED      48    // onboard RGB LED (WS2812) as on the stock build; -1 disables
#define PIN_BUTTON   0     // BOOT button: tap = push-to-talk / interrupt

#define SAMPLE_RATE          16000
#define FRAME_SAMPLES        512      // 32 ms of audio per websocket frame
#define DEFAULT_VOLUME       70       // 0..100, relay can override
#define RELAY_PORT_DEFAULT   8765
#define PLAY_BUFFER_BYTES    (48 * 1024)
