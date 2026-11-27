/*
 * Decoder task (pinned to core 1, also owns the audio DMA IRQ).
 *
 *  Radio source:     MP3 / AAC bytes -> minimp3 / Helix AAC -> volume -> I2S buffers
 *  Bluetooth source: PCM frames (already SBC-decoded) -> volume -> I2S buffers
 *  AI source:        PCM frames (Gemini 24 kHz voice)  -> volume -> I2S buffers
 *
 * Both arrive through g_radio.mp3_stream. The active source is switched
 * only on a flush request, so the two formats never mix.
 */
#include <string.h>
#include <math.h>
#include "radio.h"
#include "audio_i2s.h"
#include "aac_dec.h"
#include "tasks.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define INBUF_SIZE       (8 * 1024)
#define MIN_DECODE_BYTES 2048

static mp3dec_t s_mp3;
static uint8_t  s_in[INBUF_SIZE];
/* decoded PCM, shared by MP3 (1152 x 2) and AAC (up to 2048 x 2 with SBR) */
static int16_t  s_pcm[AAC_MAX_FRAMES * 2];

static uint32_t s_cur_hz = 44100;
static size_t   s_inlen;
static bool     s_playing;
static int      s_starve;
static stream_fmt_t s_fmt = FMT_MP3;
static uint32_t s_aac_kbps_x16;      /* smoothed AAC bitrate (x16) */

static void set_rate(uint32_t hz) {
    if (hz && hz != s_cur_hz) {
        s_cur_hz = hz;
        audio_set_sample_rate(hz);
    }
    g_radio.sample_rate = s_cur_hz;
}

/* per-source level trim (config.h TRIM_*_DB), Q15 */
static int32_t source_trim(void) {
    static int32_t trim[3];
    if (!trim[0]) {
        trim[SRC_RADIO] = (int32_t)(32767.0f * powf(10.0f, TRIM_RADIO_DB / 20.0f));
        trim[SRC_BT]    = (int32_t)(32767.0f * powf(10.0f, TRIM_BT_DB / 20.0f));
        trim[SRC_AI]    = (int32_t)(32767.0f * powf(10.0f, TRIM_AI_DB / 20.0f));
    }
    unsigned s = (unsigned)g_radio.source;
    return s < 3 ? trim[s] : 32767;
}

/* VU level of a block: before the volume, after the source trim, so the meter
 * reads the same for equally loud sources */
static void measure(audio_buf_t *b) {
    audio_measure(b);
    int32_t t = source_trim();
    b->rms_l = (uint16_t)((b->rms_l * t) >> 15);
    b->rms_r = (uint16_t)((b->rms_r * t) >> 15);
}

static void apply_gain_stereo(uint32_t *frames, uint32_t n) {
    int32_t g = (g_radio.gain_q15 * source_trim()) >> 15;    /* volume x trim */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = frames[i];
        int32_t l = ((int16_t)(f & 0xFFFF) * g) >> 15;
        int32_t r = ((int16_t)(f >> 16) * g) >> 15;
        frames[i] = ((uint32_t)(uint16_t)r << 16) | (uint16_t)l;
    }
}

/* Interleaved int16 PCM -> I2S buffers (split into AUDIO_BUF_FRAMES blocks) */
static void emit_pcm(const int16_t *pcm, int frames, int channels) {
    while (frames > 0) {
        int n = frames > AUDIO_BUF_FRAMES ? AUDIO_BUF_FRAMES : frames;
        audio_buf_t *b = audio_get_free(portMAX_DELAY);
        if (channels == 2) {
            for (int i = 0; i < n; i++)
                b->data[i] = ((uint32_t)(uint16_t)pcm[2 * i + 1] << 16) | (uint16_t)pcm[2 * i];
        } else {
            for (int i = 0; i < n; i++)
                b->data[i] = ((uint32_t)(uint16_t)pcm[i] << 16) | (uint16_t)pcm[i];
        }
        b->frames = (uint32_t)n;
        measure(b);                             /* VU level before volume, after trim */
        apply_gain_stereo(b->data, b->frames);
        audio_submit(b);
        pcm += n * channels;
        frames -= n;
    }
}

static void consume(size_t n) {
    if (n > s_inlen) n = s_inlen;
    s_inlen -= n;
    memmove(s_in, s_in + n, s_inlen);
}

/* ---------------- MP3 ---------------- */

static void mp3_frame(void) {
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&s_mp3, s_in, (int)s_inlen, s_pcm, &info);
    if (info.frame_bytes == 0) {
        if (s_inlen >= INBUF_SIZE) s_inlen = 0;     /* garbage, drop */
        else vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    consume((size_t)info.frame_bytes);
    if (samples <= 0) return;

    set_rate((uint32_t)info.hz);
    g_radio.bitrate_kbps = (uint32_t)info.bitrate_kbps;
    g_radio.codec = CODEC_MP3;
    g_radio.chmode = info.channels == 2 ? CH_STEREO : CH_MONO;
    emit_pcm(s_pcm, samples, info.channels);
}

/* ---------------- AAC / HE-AAC (ADTS) ---------------- */

static void aac_frame(void) {
    aac_frame_t fi;
    int r = aac_decode(s_in, (int)s_inlen, s_pcm, &fi);
    if (r < 0) { consume((size_t)-r); return; }          /* resync */
    if (r == 0) {
        if (s_inlen >= INBUF_SIZE) consume(1);
        else vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    consume((size_t)r);
    if (fi.frames <= 0) return;

    /* bitrate from frame size: bytes * 8 * core_rate / 1024 samples */
    uint32_t kbps_x16 = (uint32_t)r * 8u * (uint32_t)fi.core_rate * 16u / 1024u / 1000u;
    s_aac_kbps_x16 = s_aac_kbps_x16 ? (s_aac_kbps_x16 * 15 + kbps_x16) / 16 : kbps_x16;

    set_rate((uint32_t)fi.sample_rate);
    g_radio.bitrate_kbps = (s_aac_kbps_x16 + 8) / 16;
    g_radio.codec = fi.sbr ? CODEC_AACP : CODEC_AAC;
    g_radio.chmode = fi.channels == 2 ? CH_STEREO : CH_MONO;
    emit_pcm(s_pcm, fi.frames, fi.channels);
}

/* ---------------- compressed internet radio ---------------- */

static void radio_step(void) {
    if (!s_playing) {
        size_t avail = xStreamBufferBytesAvailable(g_radio.mp3_stream) + s_inlen;
        g_radio.buffer_pct = avail >= PREBUFFER_BYTES ? 100 : (uint32_t)(avail * 100 / PREBUFFER_BYTES);
        if (avail < PREBUFFER_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(20));
            return;
        }
        /* the net task sets the format before the first audio byte */
        s_fmt = g_radio.stream_fmt;
        if (s_fmt == FMT_AAC && !aac_open()) {
            radio_set_info("AAC: not enough memory");
            radio_set_status(RS_ERROR);
            while (xStreamBufferReceive(g_radio.mp3_stream, s_in, sizeof s_in, 0) > 0) { }
            s_inlen = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            return;
        }
        s_playing = true;
        s_starve = 0;
        radio_set_status(RS_PLAYING);
    }

    if (s_inlen < INBUF_SIZE) {
        TickType_t wait = s_inlen < MIN_DECODE_BYTES ? pdMS_TO_TICKS(100) : 0;
        s_inlen += xStreamBufferReceive(g_radio.mp3_stream, s_in + s_inlen, INBUF_SIZE - s_inlen, wait);
    }
    g_radio.buffer_pct = (uint32_t)(xStreamBufferBytesAvailable(g_radio.mp3_stream) * 100 / MP3_STREAM_SIZE);

    if (s_inlen < MIN_DECODE_BYTES) {
        if (++s_starve > 15) {                 /* ~1.5 s without data: rebuffer */
            s_playing = false;
            radio_set_status(RS_BUFFERING);
        }
        return;
    }
    s_starve = 0;

    if (s_fmt == FMT_AAC) aac_frame();
    else mp3_frame();
}

/* ---------------- PCM (Bluetooth, AI) ---------------- */

/* Bluetooth: real-time stream, drop samples when the phone runs ahead.
 * AI: answer arrives faster than real time, never drop; play the tail
 * of a short answer when the sender marks the end (pcm_eos). */
static void pcm_step(bool realtime) {
    size_t avail = xStreamBufferBytesAvailable(g_radio.mp3_stream);
    g_radio.buffer_pct = (uint32_t)(avail * 100 / BT_HIGH_BYTES);

    if (!s_playing) {
        bool tail = g_radio.pcm_eos && avail > 0;
        if (avail < BT_PREBUFFER_BYTES && !tail) {
            vTaskDelay(pdMS_TO_TICKS(10));
            return;
        }
        s_playing = true;
    }

    if (realtime && avail > BT_HIGH_BYTES) {
        uint8_t junk[256];
        xStreamBufferReceive(g_radio.mp3_stream, junk, sizeof junk, 0);
    }

    set_rate(g_radio.pcm_sample_rate);

    audio_buf_t *b = audio_get_free(portMAX_DELAY);
    size_t n = xStreamBufferReceive(g_radio.mp3_stream, b->data, sizeof b->data, pdMS_TO_TICKS(50));
    uint32_t frames = (uint32_t)(n / 4);
    if (frames == 0) {
        audio_release(b);
        s_playing = false;                    /* paused or underrun: rebuffer */
        return;
    }
    b->frames = frames;
    measure(b);                             /* VU level before volume, after trim */
    apply_gain_stereo(b->data, frames);
    audio_submit(b);
}

/* ---------------- task ---------------- */

void decoder_task(void *arg) {
    (void)arg;
    audio_init(s_cur_hz);
    mp3dec_init(&s_mp3);
    audio_source_t source = SRC_RADIO;

    for (;;) {
        if (g_radio.flush_req) {
            while (xStreamBufferReceive(g_radio.mp3_stream, s_in, sizeof s_in, 0) > 0) { }
            s_inlen = 0;
            mp3dec_init(&s_mp3);
            aac_close();                       /* give ~80 KB back (AI needs it for TLS) */
            s_aac_kbps_x16 = 0;
            s_playing = false;
            g_radio.buffer_pct = 0;
            source = g_radio.source;
            g_radio.codec = CODEC_NONE;
            g_radio.chmode = CH_NONE;
            g_radio.bitrate_kbps = 0;
            g_radio.flush_req = false;
            continue;
        }
        if (source == SRC_BT) pcm_step(true);
        else if (source == SRC_AI) pcm_step(false);
        else radio_step();
    }
}
