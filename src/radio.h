#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "config.h"

typedef enum {
    RS_WIFI_CONNECTING,
    RS_CONNECTING,
    RS_BUFFERING,
    RS_PLAYING,
    RS_ERROR,
    RS_BT_WAIT,        /* Bluetooth: discoverable, no phone connected */
    RS_BT_CONNECTED,   /* Bluetooth: phone connected, paused */
    RS_BT_STREAMING,   /* Bluetooth: audio streaming */
    RS_AI_CONNECTING,  /* AI: connecting to Gemini */
    RS_AI_LISTENING,   /* AI: microphone open, waiting for speech */
    RS_AI_SPEAKING,    /* AI: playing Gemini's answer */
    RS_AI_PAUSED,      /* AI: microphone paused by the user */
    RS_WIFI_SETUP,     /* own access point open for WiFi setup */
} radio_status_t;

typedef enum { SRC_RADIO, SRC_BT, SRC_AI } audio_source_t;

/* Stream parameters shown on the status line */
typedef enum { CODEC_NONE, CODEC_MP3, CODEC_SBC, CODEC_PCM, CODEC_AAC, CODEC_AACP } codec_t;

/* Compressed format of the current internet radio stream */
typedef enum { FMT_MP3, FMT_AAC } stream_fmt_t;
typedef enum { CH_NONE, CH_MONO, CH_STEREO, CH_JOINT, CH_DUAL } chmode_t;

typedef struct {
    /* control (written by UI) */
    volatile int      req_station;
    volatile int      last_station;  /* last internet radio station (to return to) */
    volatile uint32_t tune_seq;      /* bumped on every tune / reload */
    volatile int      volume;        /* 0..VOLUME_MAX */
    volatile bool     muted;
    volatile int32_t  gain_q15;      /* derived from volume/mute */

    /* status (written by net / decoder) */
    volatile radio_status_t status;
    volatile uint32_t bitrate_kbps;
    volatile codec_t  codec;
    volatile stream_fmt_t stream_fmt;  /* set by the net task from Content-Type */
    volatile chmode_t chmode;
    volatile uint32_t sample_rate;
    volatile uint32_t buffer_pct;
    volatile int      rssi;
    volatile bool     wifi_up;
    volatile bool     ap_mode;       /* setup access point is running */
    char              ip[16];

    /* decoder flush handshake; the decoder adopts `source` on each flush */
    volatile bool     flush_req;
    volatile audio_source_t source;
    volatile uint32_t pcm_sample_rate;  /* Bluetooth / AI PCM rate */
    volatile bool     pcm_eos;          /* AI: answer complete, play the tail */
    volatile int      mic_level;        /* AI: 0..100 for the UI meter */
    volatile bool     mic_paused;       /* AI: user paused the microphone */

    /* Radio: MP3 bytes from net task. Bluetooth: PCM (L,R int16) from BTstack.
     * Always consumed by the decoder task. */
    StreamBufferHandle_t mp3_stream;

    TaskHandle_t net_task, dec_task, ui_task;
} radio_t;

extern radio_t g_radio;

#define MP3_STREAM_SIZE   (48 * 1024)   /* 3 s at 128 kbps; leaves room for TLS + AAC */
#define PREBUFFER_BYTES   (24 * 1024)

/* Bluetooth PCM buffering (4 bytes per frame, ~176 KB/s at 44.1 kHz) */
#define BT_PREBUFFER_BYTES (20 * 1024)   /* ~115 ms before starting   */
#define BT_HIGH_BYTES      (40 * 1024)   /* above this, drop samples  */

/* Pseudo station indexes that select the other sources (stations are 0..ST_MAX-1) */
#define SOURCE_BT          1000
#define SOURCE_AI          1001

void radio_init(void);
void radio_tune(int station);
void radio_set_volume(int vol);
void radio_toggle_mute(void);
/* Clears the stream buffer and makes the decoder adopt g_radio.source. */
void radio_flush_decoder(void);

/* Text shared between tasks (mutex protected). Titles are transliterated
 * from UTF-8/Latin-1 to plain ASCII for the 5x7 font. */
void     radio_set_title(const char *utf8);
void     radio_to_ascii(const char *utf8, char *out, size_t n);   /* for the 5x7 font */
uint32_t radio_get_title(char *out, size_t n);
void     radio_get_title_utf8(char *out, size_t n);   /* original text, for the web page */
void     radio_set_info(const char *msg);
void     radio_get_info(char *out, size_t n);
void     radio_set_status(radio_status_t st);
const char *radio_status_name(radio_status_t st);
