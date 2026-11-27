#include <string.h>
#include <math.h>
#include "radio.h"
#include "stations.h"
#include "config.h"

radio_t g_radio;

static SemaphoreHandle_t s_lock;
static char s_title[160];
static char s_title_utf8[240];   /* original text (valid UTF-8) for the web page */
static char s_info[64];
static uint32_t s_title_seq;

static void update_gain(void) {
    int v = g_radio.volume;
    if (g_radio.muted || v <= 0) {
        g_radio.gain_q15 = 0;
        return;
    }
    /* 1.5 dB per step, 0 dB at VOLUME_MAX */
    float db = (float)(v - VOLUME_MAX) * 1.5f;
    g_radio.gain_q15 = (int32_t)(32767.0f * powf(10.0f, db / 20.0f));
}

void radio_init(void) {
    memset(&g_radio, 0, sizeof g_radio);
    s_lock = xSemaphoreCreateMutex();
    g_radio.mp3_stream = xStreamBufferCreate(MP3_STREAM_SIZE, 1);
    configASSERT(s_lock && g_radio.mp3_stream);
    g_radio.req_station = DEFAULT_STATION;
    g_radio.last_station = DEFAULT_STATION;
    g_radio.volume = VOLUME_DEFAULT;
    g_radio.status = RS_WIFI_CONNECTING;
    update_gain();
}

void radio_tune(int station) {
    if (station >= 0 && station < ST_MAX) g_radio.last_station = station;   /* not ST_TEMP */
    g_radio.req_station = station;
    g_radio.tune_seq++;
    if (g_radio.net_task) xTaskNotifyGive(g_radio.net_task);
}

void radio_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > VOLUME_MAX) vol = VOLUME_MAX;
    g_radio.volume = vol;
    if (vol > 0) g_radio.muted = false;
    update_gain();
}

void radio_toggle_mute(void) {
    g_radio.muted = !g_radio.muted;
    update_gain();
}

void radio_set_status(radio_status_t st) { g_radio.status = st; }

const char *radio_status_name(radio_status_t st) {
    switch (st) {
    case RS_WIFI_CONNECTING: return "WIFI...";
    case RS_CONNECTING:      return "CONNECT";
    case RS_BUFFERING:       return "BUFFER";
    case RS_PLAYING:         return "PLAY";
    case RS_BT_WAIT:         return "BT WAITING FOR PHONE";
    case RS_BT_CONNECTED:    return "BT PAUSED";
    case RS_BT_STREAMING:    return "BT PLAY";
    case RS_AI_CONNECTING:   return "AI CONNECTING";
    case RS_AI_LISTENING:    return "AI LISTENING";
    case RS_AI_SPEAKING:     return "AI SPEAKING";
    case RS_AI_PAUSED:       return "AI MIC OFF";
    case RS_WIFI_SETUP:      return "WIFI SETUP";
    default:                 return "ERROR";
    }
}

void radio_flush_decoder(void) {
    g_radio.flush_req = true;
    while (g_radio.flush_req) vTaskDelay(pdMS_TO_TICKS(5));
}

/* ---------- text helpers ---------- */

static char translit(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return (char)cp;
    switch (cp) {
    /* Polish */
    case 0x104: return 'A'; case 0x105: return 'a';
    case 0x106: return 'C'; case 0x107: return 'c';
    case 0x118: return 'E'; case 0x119: return 'e';
    case 0x141: return 'L'; case 0x142: return 'l';
    case 0x143: return 'N'; case 0x144: return 'n';
    case 0xD3:  return 'O'; case 0xF3:  return 'o';
    case 0x15A: return 'S'; case 0x15B: return 's';
    case 0x179: case 0x17B: return 'Z';
    case 0x17A: case 0x17C: return 'z';
    /* common Western European */
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return 'a';
    case 0xC7: return 'C'; case 0xE7: return 'c';
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
    case 0xD1: return 'N'; case 0xF1: return 'n';
    case 0xD2: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
    case 0xF2: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
    case 0xDF: return 's';
    case 0x2018: case 0x2019: case 0xB4: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    default: return '?';
    }
}

/* UTF-8 -> ASCII; invalid sequences are treated as Latin-1 bytes */
static void to_ascii(const char *in, char *out, size_t n) {
    const uint8_t *s = (const uint8_t *)in;
    size_t o = 0;
    while (*s && o + 1 < n) {
        uint32_t cp;
        int len = 0;
        if (s[0] < 0x80)                 { cp = s[0]; len = 1; }
        else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); len = 2;
        } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); len = 3;
        } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            cp = 0x10000; len = 4;      /* emoji etc. */
        } else                           { cp = s[0]; len = 1; } /* Latin-1 */
        s += len;
        if (cp == 0x10000) continue;
        out[o++] = translit(cp);
    }
    out[o] = 0;
}

void radio_to_ascii(const char *utf8, char *out, size_t n) { to_ascii(utf8, out, n); }

/* Copies text as valid UTF-8; bytes that aren't UTF-8 are taken as Latin-1
 * (common in Shoutcast metadata) and re-encoded. */
static void to_utf8(const char *in, char *out, size_t n) {
    const uint8_t *s = (const uint8_t *)in;
    size_t o = 0;
    while (*s && o + 4 < n) {
        int len = 0;
        if (s[0] < 0x80) len = 1;
        else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) len = 2;
        else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) len = 3;
        else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) len = 4;
        if (len) {
            memcpy(out + o, s, (size_t)len);
            o += (size_t)len;
            s += len;
        } else {                                   /* Latin-1 byte -> 2-byte UTF-8 */
            out[o++] = (char)(0xC0 | (s[0] >> 6));
            out[o++] = (char)(0x80 | (s[0] & 0x3F));
            s++;
        }
    }
    out[o] = 0;
}

void radio_set_title(const char *utf8) {
    char tmp[sizeof s_title];
    char raw[sizeof s_title_utf8];
    to_utf8(utf8, raw, sizeof raw);
    to_ascii(utf8, tmp, sizeof tmp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(s_title_utf8, raw);
    if (strcmp(tmp, s_title) != 0) {
        strcpy(s_title, tmp);
        s_title_seq++;
    }
    xSemaphoreGive(s_lock);
}

void radio_get_title_utf8(char *out, size_t n) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(out, s_title_utf8, n - 1);
    out[n - 1] = 0;
    xSemaphoreGive(s_lock);
}

uint32_t radio_get_title(char *out, size_t n) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(out, s_title, n - 1);
    out[n - 1] = 0;
    uint32_t seq = s_title_seq;
    xSemaphoreGive(s_lock);
    return seq;
}

void radio_set_info(const char *msg) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    to_ascii(msg, s_info, sizeof s_info);
    xSemaphoreGive(s_lock);
}

void radio_get_info(char *out, size_t n) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(out, s_info, n - 1);
    out[n - 1] = 0;
    xSemaphoreGive(s_lock);
}
