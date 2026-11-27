#pragma once
/* =====================================================================
 *  Pico 2 W Internet Radio - user configuration
 * ===================================================================== */

/* WiFi, the names, HTTPS checking and the Gemini settings below are only the
 * FACTORY DEFAULTS: change them on the web page (Settings), which saves them in
 * flash. Holding encoder 1 at power-on goes back to these values. */

/* ---------- WiFi ---------- */
/* Set here, or pass at build time: cmake -DWIFI_SSID=... -DWIFI_PASSWORD=... */
#ifndef WIFI_SSID
#define WIFI_SSID       "your-wifi-ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD   "your-wifi-password"
#endif

/* ---------- Web remote ---------- */
/* Control page at http://WEB_HOSTNAME.local/ (or the IP on the info screen) */
#define WEB_HOSTNAME    "picoradio"

/* Setup access point. When no WiFi is set, the network can't be joined
 * AP_AFTER_FAILS times in a row, or encoder 2 is held at power-on, the radio
 * opens its own WiFi network: join it and the settings page opens
 * (or go to http://192.168.4.1). AP_PASSWORD: 8+ characters, "" = open. */
#define AP_SSID         "picoradio"
#define AP_PASSWORD     ""
#define AP_AFTER_FAILS  3
#define AP_IDLE_MIN     10   /* nobody joined for this long: try the saved WiFi again */

/* ---------- HTTPS ---------- */
/* 1 = check station certificates against the bundled roots (ca_roots.h).
 * Set 0 if a station fails with "TLS: unknown cert authority": the audio
 * is public, so skipping the check only risks someone swapping the music. */
#define RADIO_TLS_VERIFY 1

/* ---------- Bluetooth (A2DP sink) ---------- */
#define BT_DEVICE_NAME  "Pico Radio"   /* name shown on the phone */
/* 1 = allow Secure Connections (P-256) pairing. Off by default because it fails
 * with many phones on the CYW43 ("status 0x05"); turn it on if your phone only
 * pairs with it. Forget "Pico Radio" on the phone after changing this. */
#define BT_SECURE_CONNECTIONS 1

/* ---------- Stations (factory defaults) ----------
 * This list is used until you edit stations in the web page; edits are
 * saved in flash and replace it ("Reset to defaults" brings it back).
 * http:// or https:// MP3 / AAC / AAC+ streams, or links to .pls / .m3u
 * playlists (the first stream in the playlist is played). */
typedef struct {
    const char *name;
    const char *url;
} station_t;

static const station_t STATIONS[] __attribute__((unused)) = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3" },
    { "SomaFM Indie Pop",    "http://ice1.somafm.com/indiepop-128-mp3" },
    { "SomaFM DEF CON",      "http://ice1.somafm.com/defcon-128-mp3" },
    { "SomaFM Drone Zone",   "http://ice1.somafm.com/dronezone-128-mp3" },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128" },
    { "Groove Salad AAC+",   "https://somafm.com/groovesalad64.pls" },  /* playlist -> HTTPS HE-AAC */
};
#define NUM_STATIONS ((int)(sizeof(STATIONS) / sizeof(STATIONS[0])))

/* ---------- PCM5102 (I2S) ---------- */
#define PIN_I2S_BCLK    11   /* -> BCK  */
#define PIN_I2S_DATA    10   /* -> DATA / DIN */
#define PIN_I2S_LRCK    9    /* -> LRCK / LCK (any pins, any order) */

/* ---------- ST7789 2.25" 76x284 TFT (SPI1) ---------- */
/* Driven by PIO, so any GPIOs work (in the module's pin order here). */
#define PIN_TFT_BL      12   /* backlight (BL / BLK), -1 if tied high */
#define TFT_BL_ACTIVE_LOW 1  /* 1 = the backlight is on when BL is low (some modules) */
#define PIN_TFT_CS      13
#define PIN_TFT_DC      14
#define PIN_TFT_RST     15
#define PIN_TFT_SCK     16   /* SCL */
#define PIN_TFT_MOSI    17   /* SDA */
#define TFT_SPI_HZ      40000000   /* max; the PIO divider rounds down (37.5 MHz at 150 MHz).
                                    * Lower it (e.g. 20000000) if long wires corrupt the picture. */

/* Landscape 284x76. The panel sits in the middle of the 240x320 ST7789 RAM. */
#define TFT_WIDTH       284
#define TFT_HEIGHT      76
#define TFT_X_OFFSET    18
#define TFT_Y_OFFSET    82
#define TFT_MADCTL      0x60 /* MV|MX landscape; use 0xA0 to rotate 180 deg */
#define TFT_BGR         0    /* set 1 if red and blue are swapped */
#define TFT_INVERT      0    /* 1 for panels that show inverted colours (yellow as blue, green as purple) */

/* ---------- Rotary encoders (KY-040 style) ---------- */
/* Encoder 1 = source: Internet Radio / Bluetooth (push: switch/mute) */
/* Encoder 2 = volume; push opens the station list (BT: play/pause)  */
#define PIN_ENC1_A      2
#define PIN_ENC1_B      3
#define PIN_ENC1_SW     4
#define PIN_ENC2_A      5
#define PIN_ENC2_B      6
#define PIN_ENC2_SW     7
#define ENC_STEPS_PER_DETENT 4  /* 4 for most KY-040; 2 if one click needs two clicks */
#define ENC_AB_PULLUP   0    /* 1 = internal pull-ups on S1/S2; 0 = plain input (module has its own pull-ups on +) */
#define ENC1_REVERSE    1
#define ENC2_REVERSE    1

/* ---------- AI voice assistant (Gemini Live API) ---------- */
/* Free API key: https://aistudio.google.com/apikey                   */
/* Set here, or pass at build time: cmake -DGEMINI_API_KEY=...          */
#ifndef GEMINI_API_KEY
#define GEMINI_API_KEY  "your-gemini-api-key"
#endif
#define GEMINI_MODEL    "gemini-3.8-live"
#define GEMINI_VOICE    "Kore"          /* any Gemini TTS voice name */
/* Answer language: a language name in English ("Polish", "English", "German"...)
 * or "auto" to answer in the language you speak. Added to the instructions. */
#define AI_LANGUAGE     "English"
#define AI_GREETING     1               /* 1 = the AI says hello when you switch to it */
#define AI_SYSTEM_PROMPT \
    "You are a voice assistant built into a small internet radio. " \
    "Answer briefly and naturally, usually in 1-3 sentences, " \
    "unless someone asks for more. Don't use lists or formatting."

/* ---------- INMP441 microphone (I2S) ---------- */
#define PIN_MIC_SCK     18   /* -> SCK */
#define PIN_MIC_WS      19   /* -> WS  (must be SCK + 1) */
#define PIN_MIC_SD      20   /* -> SD  ; L/R pin to GND */
#define MIC_GAIN        16   /* 1 = no gain, 16 = +24 dB (INMP441 is quiet) */

/* ---------- Audio ---------- */
#define VOLUME_MAX       32
#define VOLUME_DEFAULT   18
/* Level trim per source, in dB (0 or negative), so the sources sound equally
 * loud at the same volume. Radio stations are mastered much louder than music
 * from a phone, so the radio is turned down a bit. Adjust by ear. */
#define TRIM_RADIO_DB    (-6.0f)
#define TRIM_BT_DB       (0.0f)
#define TRIM_AI_DB       (0.0f)
#define DEFAULT_STATION  0
