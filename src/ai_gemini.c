/*
 * AI voice source: Google Gemini Live API over a secure WebSocket.
 *
 *   microphone (INMP441, 16 kHz) --base64 JSON--> Gemini
 *   Gemini --base64 24 kHz PCM--> decoder --> speaker
 *
 * Half-duplex: the microphone is muted while the answer plays.
 * Gemini can also control the radio through function calls: play a station
 * (saved list first, then the radio-browser.info directory), add or remove
 * saved stations, list them, and set the volume.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "mbedtls/base64.h"
#include "radio.h"
#include "ota.h"
#include "config.h"
#include "cfg.h"
#include "mic.h"
#include "tls_ws.h"
#include "ai_gemini.h"
#include "stations.h"
#include "rb.h"

#define HOST       "generativelanguage.googleapis.com"
#define PATH       "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="
#define OUT_RATE   24000
#define CHUNK      1600            /* 100 ms of 16 kHz mic audio per message */
#define TEXT_MAX   150

/* ---------------- session state ---------------- */

static uint32_t s_seq;
static bool s_ev_setup, s_ev_turn_done, s_ev_goaway;
static bool s_speaking;            /* answer audio received this turn */
static bool s_new_user_turn = true, s_new_ai_turn = true;
static char s_user_text[TEXT_MAX + 1], s_ai_text[TEXT_MAX + 1];

static uint8_t s_tx[WS_HDR_ROOM + 6400];      /* setup message: instructions + tool list */
static int16_t s_mic[CHUNK];
static size_t  s_mic_n;

static bool still_wanted(void) { return g_radio.tune_seq == s_seq && g_ota_pct < 0; }   /* paused during OTA */

/* ---------------- text on the screen ---------------- */

static void append_text(char *buf, const char *add) {
    size_t a = strlen(buf), b = strlen(add);
    if (b >= TEXT_MAX) { add += b - TEXT_MAX; b = TEXT_MAX; }
    if (a + b > TEXT_MAX) {                       /* keep the newest text */
        size_t drop = a + b - TEXT_MAX;
        memmove(buf, buf + drop, a - drop + 1);
        a -= drop;
    }
    memcpy(buf + a, add, b + 1);
}

static void show_text(void) {
    char t[TEXT_MAX + 8];
    if (s_speaking && s_ai_text[0]) snprintf(t, sizeof t, "AI: %s", s_ai_text);
    else if (s_user_text[0] && !s_new_user_turn) snprintf(t, sizeof t, "You: %s", s_user_text);
    else if (s_ai_text[0]) snprintf(t, sizeof t, "AI: %s", s_ai_text);
    else t[0] = 0;
    radio_set_title(t);
}

static void on_transcription(int which, const char *text) {
    if (which == 1) {                              /* what you said */
        if (s_new_user_turn) { s_user_text[0] = 0; s_new_user_turn = false; }
        append_text(s_user_text, text);
    } else {                                       /* what Gemini says */
        if (s_new_ai_turn) { s_ai_text[0] = 0; s_new_ai_turn = false; }
        append_text(s_ai_text, text);
    }
    show_text();
}

/* ---------------- answer audio ---------------- */

static uint32_t s_frames[256];
static int s_nframes;
static uint8_t s_carry;
static bool s_have_carry;

static void pcm_flush(void) {
    const uint8_t *p = (const uint8_t *)s_frames;
    size_t n = (size_t)s_nframes * 4;
    while (n && still_wanted()) {
        size_t w = xStreamBufferSend(g_radio.mp3_stream, p, n, pdMS_TO_TICKS(50));
        p += w;
        n -= w;
    }
    s_nframes = 0;
}

static void pcm_byte(uint8_t b) {
    if (!s_have_carry) { s_carry = b; s_have_carry = true; return; }
    s_have_carry = false;
    uint16_t s = (uint16_t)(s_carry | (b << 8));   /* 16-bit little endian mono */
    s_frames[s_nframes++] = ((uint32_t)s << 16) | s;
    if (s_nframes == 256) pcm_flush();
}

static int s_tune = -1;            /* station to switch to once Gemini has spoken */
static bool s_tune_heard;          /* Gemini has started its sentence about it */
static TickType_t s_tune_deadline;

static void audio_started(void) {
    if (s_speaking) return;
    s_speaking = true;
    if (s_tune >= 0) s_tune_heard = true;
    g_radio.pcm_eos = false;
    g_radio.mic_level = 0;
    radio_set_status(RS_AI_SPEAKING);
    show_text();
}

/* ---------------- function calls (Gemini controls the radio) ---------------- */

typedef struct {
    bool ready;                    /* a complete toolCall message arrived */
    char id[80], fn[32];
    char a_name[ST_NAME_MAX], a_genre[48], a_cc[8];
    int  a_percent;
} call_t;

static call_t s_call;
static rb_station_t s_found[5];
static rb_station_t s_last_found;  /* for "save it" (kept across sessions) */

/* ---------------- streaming JSON scanner ---------------- */
/* Server messages can be large (base64 audio), so they are scanned on the
 * fly instead of being buffered: we only care about a few keys. */

static int8_t b64val(char c) {
    if (c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int8_t)(c - '0' + 52);
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

typedef struct {
    bool in_str, esc, value_str, expect_value, key_pending;
    char key[24]; int key_len;
    char cur_key[24];
    int  ctx;                  /* 1 = inputTranscription, 2 = outputTranscription, 3 = toolCall */
    bool in_b64; uint32_t quad; int qn;
    bool in_text; char text[160]; int text_len;
    int  uni_n; uint32_t uni;
    int  depth;                /* {} nesting outside strings: 0 = message complete */
    bool tool, in_args;        /* inside a toolCall / its args */
    int  args_depth;           /* depth of the args object */
    char *cap; size_t capn, capl;      /* toolCall string value being captured */
    bool in_num; int num;              /* toolCall number value (percent) */
} scan_t;

static scan_t J;

static void text_put(uint32_t cp) {
    char *t = J.text;
    int n = J.text_len;
    if (n > (int)sizeof J.text - 5) return;
    if (cp < 0x80) t[n++] = (char)cp;
    else if (cp < 0x800) { t[n++] = (char)(0xC0 | (cp >> 6)); t[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { t[n++] = (char)(0xE0 | (cp >> 12)); t[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[n++] = (char)(0x80 | (cp & 0x3F)); }
    J.text_len = n;
}

static void text_raw(char c) {                     /* byte already UTF-8 */
    if (J.text_len < (int)sizeof J.text - 1) J.text[J.text_len++] = c;
}

static void cap_byte(char c) {
    if (J.cap && J.capl + 1 < J.capn) { J.cap[J.capl++] = c; J.cap[J.capl] = 0; }
}

static void cap_cp(uint32_t cp) {
    if (cp < 0x80) cap_byte((char)cp);
    else if (cp < 0x800) { cap_byte((char)(0xC0 | (cp >> 6))); cap_byte((char)(0x80 | (cp & 0x3F))); }
    else { cap_byte((char)(0xE0 | (cp >> 12))); cap_byte((char)(0x80 | ((cp >> 6) & 0x3F))); cap_byte((char)(0x80 | (cp & 0x3F))); }
}

/* which call_t field a toolCall string value goes into */
static void tool_value_start(void) {
    J.cap = NULL;
    const char *k = J.cur_key;
    if (!J.in_args) {
        if (!strcmp(k, "id")) { J.cap = s_call.id; J.capn = sizeof s_call.id; }
        else if (!strcmp(k, "name")) { J.cap = s_call.fn; J.capn = sizeof s_call.fn; }
    } else {
        if (!strcmp(k, "name")) { J.cap = s_call.a_name; J.capn = sizeof s_call.a_name; }
        else if (!strcmp(k, "genre")) { J.cap = s_call.a_genre; J.capn = sizeof s_call.a_genre; }
        else if (!strcmp(k, "country_code")) { J.cap = s_call.a_cc; J.capn = sizeof s_call.a_cc; }
    }
    if (J.cap) { J.capl = 0; J.cap[0] = 0; }
}

static void on_key(const char *k) {
    if (!strcmp(k, "toolCall")) { J.tool = true; J.ctx = 3; memset(&s_call, 0, sizeof s_call); s_call.a_percent = -1; }
    else if (!strcmp(k, "args") && J.tool) { J.in_args = true; J.args_depth = J.depth + 1; }
    else if (!strcmp(k, "setupComplete")) s_ev_setup = true;
    else if (!strcmp(k, "turnComplete") || !strcmp(k, "interrupted")) s_ev_turn_done = true;
    else if (!strcmp(k, "goAway")) s_ev_goaway = true;
    else if (!strcmp(k, "inputTranscription")) J.ctx = 1;
    else if (!strcmp(k, "outputTranscription")) J.ctx = 2;
    else if (!strcmp(k, "modelTurn")) J.ctx = 0;
}

static void scan_char(char c) {
    if (J.in_str) {
        if (J.in_b64) {
            if (c == '"') { J.in_str = J.in_b64 = false; return; }
            int8_t v = b64val(c);
            if (v < 0) return;                      /* '=', '\\', whitespace */
            J.quad = (J.quad << 6) | (uint32_t)v;
            if (++J.qn == 4) {
                pcm_byte((uint8_t)(J.quad >> 16));
                pcm_byte((uint8_t)(J.quad >> 8));
                pcm_byte((uint8_t)J.quad);
                J.qn = 0; J.quad = 0;
            }
            return;
        }
        if (J.uni_n) {                              /* \uXXXX */
            int8_t h = (c >= '0' && c <= '9') ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 0;
            J.uni = (J.uni << 4) | (uint32_t)h;
            if (--J.uni_n == 0) { if (J.in_text) text_put(J.uni); else if (J.cap) cap_cp(J.uni); }
            return;
        }
        if (J.esc) {
            J.esc = false;
            if (c == 'u') { J.uni_n = 4; J.uni = 0; return; }
            if (J.in_text) text_raw(c == 'n' || c == 't' || c == 'r' ? ' ' : c);
            else if (J.cap) cap_byte(c == 'n' || c == 't' || c == 'r' ? ' ' : c);
            return;
        }
        if (c == '\\') { J.esc = true; return; }
        if (c == '"') {
            J.in_str = false;
            if (J.value_str) {
                if (J.in_text && J.text_len) {
                    J.text[J.text_len] = 0;
                    on_transcription(J.ctx, J.text);
                }
                J.in_text = false;
                J.cap = NULL;
            } else {
                J.key[J.key_len] = 0;
                J.key_pending = true;
            }
            return;
        }
        if (J.value_str) { if (J.in_text) text_raw(c); else if (J.cap) cap_byte(c); }
        else if (J.key_len < (int)sizeof J.key - 1) J.key[J.key_len++] = c;
        return;
    }

    if (c == '"') {
        J.in_str = true;
        J.esc = false;
        if (J.expect_value) {
            J.expect_value = false;
            J.value_str = true;
            if (!strcmp(J.cur_key, "data")) {
                J.in_b64 = true; J.quad = 0; J.qn = 0;
                audio_started();
            } else if (!strcmp(J.cur_key, "text") && (J.ctx == 1 || J.ctx == 2)) {
                J.in_text = true; J.text_len = 0;
            } else if (J.tool) {
                tool_value_start();
            }
        } else {
            J.value_str = false;
            J.key_len = 0;
        }
        return;
    }
    if (c == ':' && J.key_pending) {
        J.key_pending = false;
        strcpy(J.cur_key, J.key);
        J.expect_value = true;
        on_key(J.cur_key);
        return;
    }
    if (J.in_num) {                                /* toolCall number (percent) */
        if (c >= '0' && c <= '9') { J.num = J.num * 10 + (c - '0'); return; }
        J.in_num = false;
        s_call.a_percent = J.num;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return;
    if (J.expect_value && J.tool && J.in_args && c >= '0' && c <= '9' && !strcmp(J.cur_key, "percent")) {
        J.in_num = true; J.num = c - '0'; J.expect_value = false;
        return;
    }
    if (c == '{') J.depth++;
    else if (c == '}') {
        J.depth--;
        if (J.in_args && J.depth < J.args_depth) J.in_args = false;      /* args object closed */
        if (J.depth == 0 && J.tool) s_call.ready = true;                 /* whole toolCall read */
    }
    J.expect_value = false;                        /* object, array, number, bool */
    J.key_pending = false;
}

static void on_ws_data(const uint8_t *d, size_t n, bool msg_start, void *ctx) {
    (void)ctx;
    if (msg_start) {
        memset(&J, 0, sizeof J);
    }
    for (size_t i = 0; i < n; i++) scan_char((char)d[i]);
}

/* ---------------- outgoing messages ---------------- */

/* the settings for this session (copied at connect, see ai_mode) */
static cfg_t s_cfg;

/* appends s as JSON string content (no quotes) */
static size_t json_esc(char *o, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c == '\n') { o[n++] = '\\'; o[n++] = 'n'; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    return n;
}

/* the functions Gemini may call (Live API function declarations) */
static const char TOOLS[] =
    "\"tools\":[{\"functionDeclarations\":["
      "{\"name\":\"play_station\","
       "\"description\":\"Play an internet radio station. Looks in the user's saved stations first, then searches "
       "the radio-browser.info directory. Give name for a specific station, genre for a style of music or topic, "
       "and country_code to stay in one country. Playing a station ends this conversation.\","
       "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
         "\"name\":{\"type\":\"STRING\",\"description\":\"station name, e.g. RMF FM\"},"
         "\"genre\":{\"type\":\"STRING\",\"description\":\"music style or topic in English, e.g. jazz, rock, news\"},"
         "\"country_code\":{\"type\":\"STRING\",\"description\":\"ISO 3166-1 alpha-2 country code, e.g. PL\"}}}},"
      "{\"name\":\"add_station\","
       "\"description\":\"Add a station to the user's saved stations without playing it. Without arguments it "
       "saves the station that was last found or played.\","
       "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
         "\"name\":{\"type\":\"STRING\",\"description\":\"station name\"},"
         "\"genre\":{\"type\":\"STRING\",\"description\":\"music style or topic in English\"},"
         "\"country_code\":{\"type\":\"STRING\",\"description\":\"ISO 3166-1 alpha-2 country code\"}}}},"
      "{\"name\":\"remove_station\","
       "\"description\":\"Remove a station from the user's saved stations.\","
       "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
         "\"name\":{\"type\":\"STRING\",\"description\":\"name of the saved station\"}},\"required\":[\"name\"]}},"
      "{\"name\":\"list_saved_stations\","
       "\"description\":\"List the user's saved stations.\","
       "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{}}},"
      "{\"name\":\"set_volume\","
       "\"description\":\"Set the radio volume.\","
       "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
         "\"percent\":{\"type\":\"INTEGER\",\"description\":\"0 to 100\"}},\"required\":[\"percent\"]}}"
    "]}],";

static const char TOOL_HINT[] =
    " You control the radio with functions: play_station, add_station, remove_station, list_saved_stations and "
    "set_volume. When the user asks for a station or some music, call play_station; starting a station ends the "
    "conversation, so afterwards say in one short sentence what will play. Never claim to have done something "
    "without calling the function.";

static int send_setup(void) {
    char *p = (char *)s_tx + WS_HDR_ROOM;
    size_t cap = sizeof s_tx - WS_HDR_ROOM, n = 0;
    n += (size_t)snprintf(p + n, cap - n, "{\"setup\":{\"model\":\"models/");
    n += json_esc(p + n, cap - n, s_cfg.gemini_model);
    n += (size_t)snprintf(p + n, cap - n, "\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
                          "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"");
    n += json_esc(p + n, cap - n, s_cfg.gemini_voice);
    n += (size_t)snprintf(p + n, cap - n, "\"}}}},\"systemInstruction\":{\"parts\":[{\"text\":\"");
    n += json_esc(p + n, cap - n, s_cfg.ai_prompt);
    if (!strcmp(s_cfg.ai_language, "auto")) {
        n += json_esc(p + n, cap - n, " Answer in the language the user speaks.");
    } else {
        n += json_esc(p + n, cap - n, " Always answer in ");
        n += json_esc(p + n, cap - n, s_cfg.ai_language);
        n += json_esc(p + n, cap - n, ", even if the user speaks another language.");
    }
    n += json_esc(p + n, cap - n, TOOL_HINT);
    n += (size_t)snprintf(p + n, cap - n, "\"}]},%s", TOOLS);
    n += (size_t)snprintf(p + n, cap - n, "\"inputAudioTranscription\":{},\"outputAudioTranscription\":{}}}");
    if (n >= cap) { printf("[ai] setup message too long\n"); return -1; }
    return ws_send_text((uint8_t *)p, n);
}

/* text from "the user" in the middle of the session (the greeting request) */
static int send_text(const char *text) {
    char *p = (char *)s_tx + WS_HDR_ROOM;
    size_t cap = sizeof s_tx - WS_HDR_ROOM, n = 0;
    n += (size_t)snprintf(p, cap, "{\"realtimeInput\":{\"text\":\"");
    n += json_esc(p + n, cap - n, text);
    n += (size_t)snprintf(p + n, cap - n, "\"}}");
    return n < cap ? ws_send_text((uint8_t *)p, n) : -1;
}

static bool s_greet_pending;         /* say hello once per switch to AI */

static int send_audio(const int16_t *pcm, size_t samples) {
    static const char head[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
    static const char tail[] = "\"}}}";
    uint8_t *p = s_tx + WS_HDR_ROOM;
    size_t cap = sizeof s_tx - WS_HDR_ROOM;
    memcpy(p, head, sizeof head - 1);
    size_t len = sizeof head - 1, olen = 0;
    mbedtls_base64_encode(p + len, cap - len, &olen, (const uint8_t *)pcm, samples * 2);
    len += olen;
    memcpy(p + len, tail, sizeof tail - 1);
    len += sizeof tail - 1;
    return ws_send_text(p, len);
}

/* ---------------- carrying out function calls ---------------- */

static int send_tool_response(const char *result) {
    char *p = (char *)s_tx + WS_HDR_ROOM;
    size_t cap = sizeof s_tx - WS_HDR_ROOM, n = 0;
    n += (size_t)snprintf(p, cap, "{\"toolResponse\":{\"functionResponses\":[{\"id\":\"");
    n += json_esc(p + n, cap - n, s_call.id);
    n += (size_t)snprintf(p + n, cap - n, "\",\"name\":\"");
    n += json_esc(p + n, cap - n, s_call.fn);
    n += (size_t)snprintf(p + n, cap - n, "\",\"response\":{\"result\":\"");
    n += json_esc(p + n, cap - n, result);
    n += (size_t)snprintf(p + n, cap - n, "\"}}]}}");
    return n < cap ? ws_send_text((uint8_t *)p, n) : -1;
}

static void describe(char *o, size_t on, const rb_station_t *st) {
    snprintf(o, on, "%s (%s%s%s %d kbps)", st->name, st->country, st->country[0] ? ", " : "", st->codec, st->bitrate);
}

/* finds stations in the directory; reports the outcome in res */
static int directory(const char *name, const char *genre, const char *cc, char *res, size_t rn) {
    radio_set_info("Searching radio-browser.info...");
    int n = rb_search(name, genre, cc, s_found, (int)(sizeof s_found / sizeof s_found[0]));
    radio_set_info("");
    if (n < 0) snprintf(res, rn, "The radio-browser.info directory can't be reached right now.");
    else if (n == 0) snprintf(res, rn, "No playable station found for that. Try another name or genre.");
    return n;
}

static void tool_play(char *res, size_t rn) {
    char nm[ST_NAME_MAX];
    int idx = s_call.a_name[0] ? stations_find(s_call.a_name) : -1;
    if (idx < 0 && !s_call.a_name[0] && s_call.a_genre[0]) idx = stations_find(s_call.a_genre);
    if (idx >= 0 && stations_get(idx, nm, sizeof nm, NULL, 0)) {
        snprintf(res, rn, "Found in the saved stations: %s. It starts as soon as you finish your sentence.", nm);
        s_tune = idx;
        return;
    }
    int n = directory(s_call.a_name, s_call.a_genre, s_call.a_cc, res, rn);
    if (n <= 0) return;
    s_last_found = s_found[0];
    stations_set_temp(s_found[0].name, s_found[0].url);
    s_tune = ST_TEMP;
    char d[ST_NAME_MAX + 32];
    describe(d, sizeof d, &s_found[0]);
    size_t k = (size_t)snprintf(res, rn, "Playing %s from the radio-browser.info directory. It starts as soon as you "
                                "finish your sentence. It is not saved; the user can ask to save it.", d);
    if (n > 1) k += (size_t)snprintf(res + k, rn - k, " Other matches:");
    for (int i = 1; i < n && k < rn; i++) k += (size_t)snprintf(res + k, rn - k, "%s %s", i > 1 ? "," : "", s_found[i].name);
}

static void tool_add(char *res, size_t rn) {
    rb_station_t st;
    if (s_call.a_name[0] || s_call.a_genre[0]) {
        int n = directory(s_call.a_name, s_call.a_genre, s_call.a_cc, res, rn);
        if (n <= 0) return;
        st = s_found[0];
        s_last_found = st;
    } else if (s_last_found.url[0]) {
        st = s_last_found;
    } else {
        snprintf(res, rn, "Nothing to save yet: tell me which station.");
        return;
    }
    int have = stations_find(st.name);
    char nm[ST_NAME_MAX];
    if (have >= 0 && stations_get(have, nm, sizeof nm, NULL, 0) && !strcmp(nm, st.name)) {
        snprintf(res, rn, "%s is already saved, as station %d.", st.name, have + 1);
        return;
    }
    if (stations_count() >= ST_MAX && !stations_is_default()) {
        snprintf(res, rn, "The station list is full (%d stations). Remove one first.", ST_MAX);
        return;
    }
    if (!stations_add(st.name, st.url)) {
        snprintf(res, rn, "Couldn't save %s.", st.name);
        return;
    }
    snprintf(res, rn, "Saved %s as station %d.", st.name, stations_count());
}

static void tool_remove(char *res, size_t rn) {
    char nm[ST_NAME_MAX];
    int idx = stations_find(s_call.a_name);
    if (idx < 0 || !stations_get(idx, nm, sizeof nm, NULL, 0)) {
        snprintf(res, rn, "No saved station matches \"%s\".", s_call.a_name);
        return;
    }
    if (stations_count() <= 1) {
        snprintf(res, rn, "%s is the only saved station; the list can't be empty.", nm);
        return;
    }
    if (!stations_delete(idx)) snprintf(res, rn, "Couldn't remove %s.", nm);
    else snprintf(res, rn, "Removed %s. %d stations left.", nm, stations_count());
}

static void tool_list(char *res, size_t rn) {
    char nm[ST_NAME_MAX];
    int count = stations_count();
    size_t k = (size_t)snprintf(res, rn, "%d saved stations:", count);
    for (int i = 0; i < count && k + 8 < rn; i++)
        if (stations_get(i, nm, sizeof nm, NULL, 0)) k += (size_t)snprintf(res + k, rn - k, " %d. %s;", i + 1, nm);
}

static void tool_volume(char *res, size_t rn) {
    int p = s_call.a_percent;
    if (p < 0) { snprintf(res, rn, "Missing percent."); return; }
    if (p > 100) p = 100;
    radio_set_volume((p * VOLUME_MAX + 50) / 100);
    snprintf(res, rn, "Volume set to %d%%.", p);
}

static void handle_call(void) {
    static char res[600];
    res[0] = 0;
    printf("[ai] function %s(name=\"%s\" genre=\"%s\" country=\"%s\" percent=%d)\n", s_call.fn,
           s_call.a_name, s_call.a_genre, s_call.a_cc, s_call.a_percent);
    if (!strcmp(s_call.fn, "play_station")) tool_play(res, sizeof res);
    else if (!strcmp(s_call.fn, "add_station")) tool_add(res, sizeof res);
    else if (!strcmp(s_call.fn, "remove_station")) tool_remove(res, sizeof res);
    else if (!strcmp(s_call.fn, "list_saved_stations")) tool_list(res, sizeof res);
    else if (!strcmp(s_call.fn, "set_volume")) tool_volume(res, sizeof res);
    else snprintf(res, sizeof res, "Unknown function.");
    printf("[ai] -> %s\n", res);
    if (s_tune >= 0) { s_tune_heard = false; s_tune_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(12000); }
    send_tool_response(res);
}

/* switches to the station picked by play_station (ends AI mode) */
static void do_tune(void) {
    int st = s_tune;
    s_tune = -1;
    printf("[ai] switching to station %d\n", st);
    radio_tune(st);
}

static int mic_level(const int16_t *pcm, size_t n) {
    float sum = 0;
    for (size_t i = 0; i < n; i++) sum += (float)pcm[i] * (float)pcm[i];
    float rms = sqrtf(sum / (float)n) + 1.0f;
    float db = 20.0f * log10f(rms / 32768.0f);     /* -90 .. 0 dBFS */
    int lvl = (int)((db + 60.0f) * 100.0f / 60.0f);
    return lvl < 0 ? 0 : lvl > 100 ? 100 : lvl;
}

/* ---------------- one WebSocket session ---------------- */

static void session(void) {
    s_ev_setup = s_ev_turn_done = s_ev_goaway = false;
    s_speaking = false;
    s_mic_n = 0;
    s_nframes = 0;
    s_have_carry = false;
    s_call.ready = false;
    s_tune = -1;
    bool ready = false, turn_done = false;
    TickType_t quiet_since = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);

    mic_start();
    while (still_wanted()) {
        if (ws_poll(on_ws_data, NULL) < 0) {
            radio_set_info("Gemini disconnected");
            break;
        }
        if (s_nframes) pcm_flush();

        if (s_ev_setup) {
            s_ev_setup = false;
            ready = true;
            radio_set_info("Speak, I'm listening...");
            radio_set_status(g_radio.mic_paused ? RS_AI_PAUSED : RS_AI_LISTENING);
            printf("[ai] session ready\n");
            if (s_greet_pending) {
                s_greet_pending = false;
                send_text("(The user has just switched on the voice assistant on the radio. "
                          "Greet them warmly in one short sentence and ask how you can help.)");
            }
        }
        if (!ready) {
            if (xTaskGetTickCount() > deadline) { radio_set_info("Gemini setup timeout"); break; }
            continue;
        }
        if (s_call.ready) {                         /* Gemini wants to do something */
            s_call.ready = false;
            handle_call();
        }
        if (s_ev_turn_done) {
            s_ev_turn_done = false;
            turn_done = true;
            g_radio.pcm_eos = true;                /* let the decoder play the tail */
            s_new_user_turn = s_new_ai_turn = true;
        }

        /* answer finished playing -> listen again (or switch to the station) */
        if (turn_done) {
            if (!s_speaking) {
                turn_done = false;                  /* turn without audio */
            } else if (xStreamBufferBytesAvailable(g_radio.mp3_stream) == 0) {
                if (!quiet_since) quiet_since = xTaskGetTickCount();
                if (xTaskGetTickCount() - quiet_since > pdMS_TO_TICKS(400)) {
                    s_speaking = turn_done = false;
                    quiet_since = 0;
                    mic_flush();
                    s_mic_n = 0;
                    radio_set_status(g_radio.mic_paused ? RS_AI_PAUSED : RS_AI_LISTENING);
                    show_text();
                }
            }
        }
        /* a station was picked: switch once Gemini has said its sentence
         * (or after 12 s if it says nothing) */
        if (s_tune >= 0 && s_tune_heard && !s_speaking && !turn_done) {
            do_tune();
            break;
        }
        if (s_tune >= 0 && (int32_t)(xTaskGetTickCount() - s_tune_deadline) > 0) {
            do_tune();
            break;
        }
        if (s_ev_goaway && !s_speaking) {           /* server wants us to reconnect */
            printf("[ai] goAway, reconnecting\n");
            break;
        }

        /* microphone */
        if (s_speaking) { mic_flush(); continue; }
        if (g_radio.mic_paused) {
            mic_flush();
            s_mic_n = 0;
            g_radio.mic_level = 0;
            if (g_radio.status != RS_AI_PAUSED) radio_set_status(RS_AI_PAUSED);
            continue;
        }
        if (g_radio.status == RS_AI_PAUSED) radio_set_status(RS_AI_LISTENING);

        s_mic_n += mic_read(s_mic + s_mic_n, CHUNK - s_mic_n);
        if (s_mic_n == CHUNK) {
            g_radio.mic_level = mic_level(s_mic, CHUNK);
            if (send_audio(s_mic, CHUNK) != 0) {
                radio_set_info("Gemini send failed");
                break;
            }
            s_mic_n = 0;
        }
    }
    mic_stop();
    g_radio.mic_level = 0;
}

/* ---------------- entry point (runs in the net task) ---------------- */

void ai_mode(uint32_t seq) {
    static bool mic_ready;
    s_seq = seq;
    printf("[ai] switching to AI\n");

    g_radio.source = SRC_AI;
    g_radio.pcm_sample_rate = OUT_RATE;
    g_radio.pcm_eos = false;
    radio_flush_decoder();
    g_radio.codec = CODEC_PCM;              /* Gemini voice: raw 16-bit PCM */
    g_radio.chmode = CH_MONO;
    g_radio.bitrate_kbps = OUT_RATE * 16 / 1000;
    s_user_text[0] = s_ai_text[0] = 0;
    s_new_user_turn = s_new_ai_turn = true;
    radio_set_title("");

    if (!mic_ready) { mic_init(); mic_ready = true; }

    cfg_copy(&s_cfg);                       /* settings changed on the web page apply here */
    s_greet_pending = s_cfg.ai_greet;       /* not again after a reconnect */
    if (!s_cfg.gemini_key[0] || strncmp(s_cfg.gemini_key, "your-", 5) == 0) {
        radio_set_info("Set the Gemini API key on the web page");
        radio_set_status(RS_ERROR);
        while (still_wanted()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        return;
    }

    while (still_wanted()) {
        radio_set_status(RS_AI_CONNECTING);
        radio_set_info("Connecting to Gemini...");
        static char path[sizeof PATH + sizeof s_cfg.gemini_key];
        snprintf(path, sizeof path, "%s%s", PATH, s_cfg.gemini_key);
        int r = ws_connect(HOST, path);
        if (r == 0 && send_setup() == 0) {
            session();
        } else {
            radio_set_info(r == -2 ? "Gemini refused - check API key" : "Gemini connection failed");
            radio_set_status(RS_ERROR);
        }
        ws_close();
        if (still_wanted()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(r == 0 ? 500 : 5000));
    }
    mic_stop();
    radio_set_title("");
    printf("[ai] leaving AI\n");
}
