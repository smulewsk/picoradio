/*
 * UI task: encoders + 284x76 ST7789 screen.
 *
 *  Encoder 1 (source):
 *     turn = open source picker (Internet Radio / Bluetooth / AI) and move in it
 *     push = switch to the picked source (picker closed: mute on/off)
 *     long push = info screen
 *
 *  Encoder 2 (volume + stations):
 *     turn = volume (or move in the station list while it is open)
 *     Radio:     push = open station list / tune, long push = reconnect
 *     Bluetooth: push = play/pause, long push = next track
 *     AI:        push = microphone on/off, long push = new conversation
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/cyw43_arch.h"
#include "radio.h"
#include "config.h"
#include "gfx.h"
#include "st7789.h"
#include "encoder.h"
#include "audio_i2s.h"
#include "tasks.h"
#include "bt_sink.h"
#include "mem.h"
#include "stations.h"
#include "splash_logo.h"
#include "ota.h"
#include "version.h"
#include "settings.h"
#include "cfg.h"

#define FRAME_MS        40
#define BROWSE_TIMEOUT  6000
#define INFO_TIMEOUT    5000
#define VOLUME_SHOW_MS  2000
#define SPLASH_HOLD_MS  1500

#define C_BG      rgb(0, 0, 0)
#define C_HEAD    rgb(10, 30, 80)
#define C_WHITE   rgb(255, 255, 255)
#define C_TITLE   rgb(255, 210, 60)
#define C_DIM     rgb(130, 140, 160)
#define C_GREEN   rgb(40, 220, 90)
#define C_RED     rgb(240, 60, 50)
#define C_SEL     rgb(30, 90, 200)
#define C_BAR     rgb(60, 170, 255)
#define C_BT      rgb(0, 90, 200)
#define C_AI      rgb(110, 40, 160)
#define C_BADGE   rgb(255, 176, 40)

/* VU meter LED colours: lit / unlit (retro dim glow) */
#define C_VU_G    rgb(60, 230, 90)
#define C_VU_G0   rgb(10, 45, 18)
#define C_VU_Y    rgb(255, 190, 40)
#define C_VU_Y0   rgb(50, 38, 8)
#define C_VU_R    rgb(255, 55, 40)
#define C_VU_R0   rgb(55, 12, 10)

typedef enum { MENU_NONE, MENU_STATIONS, MENU_SOURCE } menu_t;

static menu_t s_menu;
static int s_sel;                    /* highlighted row of the open menu */
static uint32_t s_menu_until, s_info_until;
static int s_scroll;
static uint32_t s_title_seq = UINT32_MAX;
static char s_title[160];

/* name for the 5x7 font (station names can be UTF-8 from the web page) */
static const char *source_name(int i) {
    static char out[ST_NAME_MAX];
    char raw[ST_NAME_MAX];
    if (i == SOURCE_BT) return "Bluetooth";
    if (i == SOURCE_AI) return "AI Gemini";
    if (!stations_get(i, raw, sizeof raw, NULL, 0)) return "?";
    radio_to_ascii(raw, out, sizeof out);
    return out;
}

#define NUM_SRC 3
static const char *const SOURCES[NUM_SRC] = { "Internet Radio", "Bluetooth", "AI Gemini" };

static bool bt_mode(void) { return g_radio.req_station == SOURCE_BT; }
static bool ai_mode(void) { return g_radio.req_station == SOURCE_AI; }
static bool radio_mode(void) { return g_radio.req_station < SOURCE_BT; }
static int  cur_source(void) { return bt_mode() ? 1 : ai_mode() ? 2 : 0; }

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

static void fit(char *dst, size_t dn, const char *src, int max_chars) {
    snprintf(dst, dn, "%s", src);
    if ((int)strlen(dst) > max_chars && max_chars > 2) {
        dst[max_chars - 2] = '.';
        dst[max_chars - 1] = '.';
        dst[max_chars] = 0;
    }
}

/* ---------------- VU meters ----------------
 * Two LED bars on one line, L grows to the left and R to the right from a
 * centre badge. 20 segments of 2 dB (-40..0 dB), green / amber / red,
 * with falling peak-hold markers and a dB scale underneath. */

#define VU_Y        56
#define VU_SEG_H    9
#define VU_SEG_W    5
#define VU_STEP     6
#define VU_NSEG     20
#define VU_CX       142
#define VU_GAP      9          /* half width of the centre badge */
#define VU_OFFSET   9.0f       /* dB added so typical music reaches amber (+6 dB to make up for TRIM_RADIO_DB) */

static float s_vu[2], s_pk[2];
static uint32_t s_pk_t[2];

static void vu_update(uint32_t now) {
    uint16_t lv[2];
    audio_levels(&lv[0], &lv[1]);
    for (int c = 0; c < 2; c++) {
        float db = lv[c] ? 20.0f * log10f((float)lv[c] / 32768.0f) + VU_OFFSET : -60.0f;
        float t = (db + 40.0f) / 2.0f;                    /* lit segments */
        if (t < 0) t = 0;
        if (t > VU_NSEG) t = VU_NSEG;
        if (t > s_vu[c]) s_vu[c] += (t - s_vu[c]) * 0.7f; /* fast attack */
        else s_vu[c] -= 0.8f;                              /* ~40 dB/s fall */
        if (s_vu[c] < 0) s_vu[c] = 0;
        if (s_vu[c] >= s_pk[c]) { s_pk[c] = s_vu[c]; s_pk_t[c] = now; }
        else if (now - s_pk_t[c] > 800) s_pk[c] -= 0.4f;  /* hold, then drop */
        if (s_pk[c] < 0) s_pk[c] = 0;
    }
}

static void seg_colors(int i, uint16_t *on, uint16_t *off) {
    if (i >= 17)      { *on = C_VU_R; *off = C_VU_R0; }    /* above -6 dB  */
    else if (i >= 14) { *on = C_VU_Y; *off = C_VU_Y0; }    /* -12 .. -6 dB */
    else              { *on = C_VU_G; *off = C_VU_G0; }
}

static void draw_vu(void) {
    for (int c = 0; c < 2; c++) {
        int lit = (int)(s_vu[c] + 0.5f);
        int pk = (int)(s_pk[c] + 0.5f) - 1;
        for (int i = 0; i < VU_NSEG; i++) {
            uint16_t on, off;
            seg_colors(i, &on, &off);
            int x = c == 0 ? VU_CX - VU_GAP - (i + 1) * VU_STEP + 1
                           : VU_CX + VU_GAP + i * VU_STEP;
            bool bright = i < lit || (i == pk && pk >= 1);
            gfx_fill(x, VU_Y, VU_SEG_W, VU_SEG_H, bright ? on : off);
        }
    }

    /* centre badge */
    gfx_rect(VU_CX - VU_GAP + 1, VU_Y - 1, 2 * VU_GAP - 2, VU_SEG_H + 2, C_DIM);
    gfx_text(VU_CX - 7, VU_Y + 1, "L", C_WHITE, 1, NULL);
    gfx_text(VU_CX + 3, VU_Y + 1, "R", C_WHITE, 1, NULL);
    gfx_text(VU_CX - 6, VU_Y + 13, "dB", C_DIM, 1, NULL);

    /* scale: tick + label for each mark, mirrored on both sides */
    static const int marks[] = { -30, -20, -10, -5, 0 };
    static const char *const labels[] = { "30", "20", "10", "5", "0" };
    for (int m = 0; m < 5; m++) {
        int off = (marks[m] + 40) * 3 - 1;
        int w = gfx_text_width(labels[m], 1) - 1;
        for (int side = 0; side < 2; side++) {
            int x = side == 0 ? VU_CX - VU_GAP - off : VU_CX + VU_GAP + off - 1;
            uint16_t tick = marks[m] >= -5 ? C_VU_R : C_DIM;
            gfx_fill(x, VU_Y + VU_SEG_H + 1, 1, 2, tick);
            gfx_text(x - w / 2, VU_Y + VU_SEG_H + 4, labels[m], tick, 1, NULL);
        }
    }
}

static void draw_volume(void) {
    const int y = 58;
    gfx_text(4, y + 4, "VOL", C_DIM, 1, NULL);
    const int bx = 26, bw = 200, bh = 14;
    gfx_rect(bx, y, bw, bh, C_DIM);
    int fill = (bw - 4) * g_radio.volume / VOLUME_MAX;
    gfx_fill(bx + 2, y + 2, fill, bh - 4, g_radio.muted ? C_RED : C_BAR);
    char v[8];
    if (g_radio.muted) snprintf(v, sizeof v, "MUTE");
    else snprintf(v, sizeof v, "%d", g_radio.volume);
    gfx_text(bx + bw + 8, y, v, g_radio.muted ? C_RED : C_WHITE, 2, NULL);
}

static void draw_main(void) {
    char buf[64];

    /* header: station name */
    int st = g_radio.req_station;
    gfx_fill(0, 0, TFT_WIDTH, 18, st == SOURCE_BT ? C_BT : st == SOURCE_AI ? C_AI : C_HEAD);
    char num[8];
    if (st == SOURCE_BT) snprintf(num, sizeof num, "BT");
    else if (st == SOURCE_AI) snprintf(num, sizeof num, "AI");
    else if (st == ST_TEMP) snprintf(num, sizeof num, "NEW");      /* found by the AI, not saved */
    else snprintf(num, sizeof num, "%d/%d", st + 1, stations_count());
    int numw = gfx_text_width(num, 1);
    gfx_text(TFT_WIDTH - numw - 4, 6, num, C_DIM, 1, NULL);
    fit(buf, sizeof buf, source_name(st), (TFT_WIDTH - numw - 12) / 12);
    gfx_text(4, 2, buf, C_WHITE, 2, NULL);

    /* song title, scrolling if too long */
    uint32_t seq = radio_get_title(s_title, sizeof s_title);
    if (seq != s_title_seq) { s_title_seq = seq; s_scroll = -40; }
    const char *t = s_title;
    uint16_t tcol = C_TITLE;
    char info[64];
    if (!t[0]) {
        radio_get_info(info, sizeof info);
        t = info[0] ? info : "";
        tcol = g_radio.status == RS_ERROR ? C_RED : C_DIM;
        if (g_radio.status == RS_BT_WAIT || g_radio.status == RS_BT_CONNECTED ||
            g_radio.status == RS_AI_LISTENING || g_radio.status == RS_WIFI_SETUP) tcol = C_WHITE;
    }
    clip_t clip = { 0, 21, TFT_WIDTH, 36 };
    int tw = gfx_text_width(t, 2);
    if (tw <= TFT_WIDTH - 8) {
        gfx_text(4, 22, t, tcol, 2, &clip);
    } else {
        const int gap = 48;
        int off = s_scroll < 0 ? 0 : s_scroll;
        gfx_text(4 - off, 22, t, tcol, 2, &clip);
        gfx_text(4 - off + tw + gap, 22, t, tcol, 2, &clip);
        s_scroll += 2;
        if (s_scroll >= tw + gap) s_scroll = 0;
    }

    /* status line */
    const char *sname;
    uint16_t scol = C_DIM;
    switch (g_radio.status) {
    case RS_WIFI_CONNECTING: sname = "WIFI..."; break;
    case RS_CONNECTING:      sname = "CONNECT"; break;
    case RS_BUFFERING:       sname = "BUFFER";  break;
    case RS_PLAYING:         sname = "PLAY";    scol = C_GREEN; break;
    case RS_BT_WAIT:         sname = "BT WAITING FOR PHONE"; scol = C_BAR; break;
    case RS_BT_CONNECTED:    sname = "BT PAUSED"; scol = C_BAR; break;
    case RS_BT_STREAMING:    sname = "BT PLAY"; scol = C_GREEN; break;
    case RS_AI_CONNECTING:   sname = "AI CONNECTING"; break;
    case RS_AI_LISTENING:    sname = "AI LISTENING"; scol = C_GREEN; break;
    case RS_AI_SPEAKING:     sname = "AI SPEAKING"; scol = C_TITLE; break;
    case RS_AI_PAUSED:       sname = "AI MIC OFF"; scol = C_RED; break;
    case RS_WIFI_SETUP:      sname = "WIFI SETUP"; scol = C_BAR; break;
    default:                 sname = "ERROR";   scol = C_RED; break;
    }
    int x = gfx_text(4, 44, sname, scol, 1, NULL);

    /* stream parameters: [MP3] 128kbps 44.1kHz ST */
    bool active = g_radio.status == RS_PLAYING || g_radio.status == RS_BT_STREAMING ||
                  g_radio.status == RS_AI_SPEAKING;
    if (active && g_radio.codec != CODEC_NONE) {
        static const char *const CODEC[] = { "", "MP3", "SBC", "PCM", "AAC", "AAC+" };
        static const char *const CH[] = { "", "MONO", "ST", "JS", "DUAL" };
        const char *codec = CODEC[g_radio.codec];
        int bw = gfx_text_width(codec, 1) + 3;
        x += 6;
        gfx_fill(x, 43, bw, 9, C_BADGE);                 /* retro badge */
        gfx_text(x + 2, 44, codec, C_BG, 1, NULL);
        x += bw + 6;

        char rate[12];
        uint32_t sr = g_radio.sample_rate;
        if (sr % 1000) snprintf(rate, sizeof rate, "%lu.%lukHz", (unsigned long)(sr / 1000), (unsigned long)(sr % 1000 / 100));
        else snprintf(rate, sizeof rate, "%lukHz", (unsigned long)(sr / 1000));
        if (g_radio.codec == CODEC_PCM)
            snprintf(buf, sizeof buf, "16bit %s %s", rate, CH[g_radio.chmode]);
        else
            snprintf(buf, sizeof buf, "%lukbps %s %s", (unsigned long)g_radio.bitrate_kbps, rate, CH[g_radio.chmode]);
        x = gfx_text(x, 44, buf, C_WHITE, 1, NULL);
    }
    if (g_radio.status == RS_AI_LISTENING) {        /* microphone level meter */
        int w = 90 * g_radio.mic_level / 100;
        gfx_rect(x + 8, 43, 94, 9, C_DIM);
        gfx_fill(x + 10, 45, w, 5, g_radio.mic_level > 85 ? C_RED : C_GREEN);
    }

    /* right side: buffer fill and WiFi signal */
    char right[24] = "";
    if (g_radio.status == RS_PLAYING || g_radio.status == RS_BUFFERING ||
        g_radio.status == RS_BT_STREAMING)
        snprintf(right, sizeof right, "buf%lu%%", (unsigned long)g_radio.buffer_pct);
    if (g_radio.wifi_up && !bt_mode()) {
        size_t n = strlen(right);
        snprintf(right + n, sizeof right - n, "%s%ddBm", n ? " " : "", g_radio.rssi);
    }
    if (right[0]) gfx_text(TFT_WIDTH - gfx_text_width(right, 1) - 4, 44, right, C_DIM, 1, NULL);
}

/* 3-row scrolling list with the selection in the middle */
static void draw_list(int count, int sel, int active, bool numbered, const char *(*name_of)(int)) {
    char buf[32];
    for (int k = -1; k <= 1; k++) {
        int i = (sel + k + count) % count;
        int y = 19 + k * 18;
        if (count < 3 && k != 0 && i == sel) continue;
        if (count == 2 && k == -1) continue;            /* 2 items: no wrap-around row */
        if (k == 0) gfx_fill(0, y - 2, TFT_WIDTH, 18, C_SEL);
        int tx = 4;
        if (numbered) {
            snprintf(buf, sizeof buf, "%d", i + 1);
            gfx_text(4, y, buf, k == 0 ? C_WHITE : C_DIM, 2, NULL);
            tx = 34;
        }
        char name[32];
        fit(name, sizeof name, name_of(i), (TFT_WIDTH - tx - 20) / 12);
        uint16_t col = k == 0 ? C_WHITE : C_DIM;
        if (i == active && k != 0) col = C_GREEN;
        gfx_text(tx, y, name, col, 2, NULL);
        if (i == active) gfx_text(TFT_WIDTH - 16, y, "*", k == 0 ? C_WHITE : C_GREEN, 2, NULL);
    }
}

static const char *station_name(int i) { return source_name(i); }
static const char *src_name(int i)     { return SOURCES[i]; }

static void draw_info(void) {
    char buf[64];
    gfx_fill(0, 0, TFT_WIDTH, 12, C_HEAD);
    gfx_text(4, 2, "INFO", C_WHITE, 1, NULL);
    snprintf(buf, sizeof buf, "v%s  %s  slot %c", FW_VERSION_STR, FW_BUILD_STR, ota_slot());
    gfx_text(TFT_WIDTH - 4 - gfx_text_width(buf, 1), 2, buf, C_DIM, 1, NULL);
    snprintf(buf, sizeof buf, "SSID: %s", cfg()->wifi_ssid);
    gfx_text(4, 16, buf, C_WHITE, 1, NULL);
    snprintf(buf, sizeof buf, "IP:   %s  RSSI %d dBm", g_radio.wifi_up || g_radio.ap_mode ? g_radio.ip : "-", g_radio.rssi);
    gfx_text(4, 26, buf, C_WHITE, 1, NULL);
    snprintf(buf, sizeof buf, "Free RTOS %uK  C %uK  underruns %lu",
             (unsigned)(xPortGetFreeHeapSize() / 1024), (unsigned)(xmem_c_heap_free() / 1024),
             (unsigned long)audio_underruns());
    gfx_text(4, 36, buf, C_WHITE, 1, NULL);
    radio_get_info(buf, sizeof buf);
    gfx_text(4, 46, buf, C_DIM, 1, NULL);
}

/* Firmware update in progress (written from the web page) */
static void draw_ota(void) {
    char buf[48];
    int pct = g_ota_pct;
    gfx_fill(0, 0, TFT_WIDTH, 12, C_HEAD);
    gfx_text(4, 2, "FIRMWARE UPDATE", C_WHITE, 1, NULL);
    snprintf(buf, sizeof buf, pct >= 100 ? "RESTARTING" : "SLOT %c  %d%%", ota_target_slot(), pct);
    gfx_text(4, 20, buf, C_TITLE, 2, NULL);
    gfx_fill(4, 44, TFT_WIDTH - 8, 10, C_DIM);
    gfx_fill(5, 45, TFT_WIDTH - 10, 8, C_BG);
    gfx_fill(5, 45, (TFT_WIDTH - 10) * pct / 100, 8, C_BAR);
    gfx_text(4, 62, "Don't switch off", C_DIM, 1, NULL);
}

/* Boot splash: the logo, faded in with the backlight, held while WiFi starts */
static void show_splash(void) {
    gfx_clear(C_BG);
    const int x0 = (TFT_WIDTH - SPLASH_W) / 2, y0 = (TFT_HEIGHT - SPLASH_H) / 2;
    /* run-length coded, see splash_logo.h */
    for (int i = 0, p = 0; p < SPLASH_W * SPLASH_H; ) {
        uint16_t h = SPLASH_RLE[i++];
        int n = h & 0x7fff;
        bool run = h & 0x8000;
        uint16_t c = run ? SPLASH_RLE[i++] : 0;
        for (; n > 0 && p < SPLASH_W * SPLASH_H; n--, p++)
            g_fb[(y0 + p / SPLASH_W) * TFT_WIDTH + x0 + p % SPLASH_W] = run ? c : SPLASH_RLE[i++];
    }
    /* firmware version under the lettering, right-aligned with it */
    char ver[24];
    snprintf(ver, sizeof ver, "v%s", FW_VERSION_STR);
    gfx_text(x0 + 259 - gfx_text_width(ver, 1), y0 + 56, ver, C_DIM, 1, NULL);
    st7789_push(g_fb);
    for (int l = 0; l <= 255; l += 5) {                 /* ~0.5 s fade-in */
        st7789_backlight_level((uint8_t)l);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    st7789_backlight_level(255);
    vTaskDelay(pdMS_TO_TICKS(SPLASH_HOLD_MS));
}

/* Signal strength every 3 s, in its own low-priority task so a slow answer
 * from the WiFi chip never stalls the display. */
void rssi_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!g_radio.wifi_up) continue;
        int32_t rssi;
        TickType_t t0 = xTaskGetTickCount();
        if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0) g_radio.rssi = (int)rssi;
        uint32_t ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        if (ms > 200) printf("[ui] RSSI read took %lu ms\n", (unsigned long)ms);
    }
}

void ui_task(void *arg) {
    (void)arg;
    st7789_init();
    encoders_init();
    show_splash();

    TickType_t wake = xTaskGetTickCount();
    uint32_t last_frame = 0;
    int last_vol = g_radio.volume;
    bool last_muted = g_radio.muted;
    uint32_t vol_until = 0;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
        uint32_t now = now_ms();

        /* --- input --- */

        /* Encoder 1: source picker */
        int d1 = encoder_read(0);
        if (d1) {
            if (s_menu != MENU_SOURCE) { s_menu = MENU_SOURCE; s_sel = cur_source(); }
            s_sel = (s_sel + (d1 > 0 ? 1 : -1) + NUM_SRC) % NUM_SRC;
            s_menu_until = now + BROWSE_TIMEOUT;
            s_info_until = 0;
        }
        switch (encoder_button(0, now)) {
        case BTN_SHORT:
            if (s_menu == MENU_SOURCE) {
                s_menu = MENU_NONE;
                if (s_sel != cur_source()) {
                    if (s_sel == 1) radio_tune(SOURCE_BT);
                    else if (s_sel == 2) { g_radio.mic_paused = false; radio_tune(SOURCE_AI); }
                    else radio_tune(g_radio.last_station);
                }
            } else {
                radio_toggle_mute();
            }
            break;
        case BTN_LONG:
            s_info_until = now + INFO_TIMEOUT;
            s_menu = MENU_NONE;
            break;
        default: break;
        }

        /* Encoder 2: volume, or station list while it is open */
        int d2 = encoder_read(1);
        if (d2 && s_menu == MENU_STATIONS) {
            int n = stations_count();
            s_sel = ((s_sel + d2) % n + n) % n;
            s_menu_until = now + BROWSE_TIMEOUT;
        } else if (d2) {
            radio_set_volume(g_radio.volume + d2);
            if (bt_mode()) bt_sync_volume(g_radio.volume);
        }
        switch (encoder_button(1, now)) {
        case BTN_SHORT:
            if (s_menu == MENU_STATIONS) {
                s_menu = MENU_NONE;
                if (s_sel != g_radio.req_station) radio_tune(s_sel);
            } else if (bt_mode()) {
                bt_play_pause();
            } else if (ai_mode()) {
                g_radio.mic_paused = !g_radio.mic_paused;   /* microphone on/off */
            } else {
                s_menu = MENU_STATIONS;
                s_sel = g_radio.req_station < stations_count() ? g_radio.req_station : 0;
                s_menu_until = now + BROWSE_TIMEOUT;
                s_info_until = 0;
            }
            break;
        case BTN_LONG:
            s_menu = MENU_NONE;
            if (bt_mode()) bt_next();
            else radio_tune(g_radio.req_station);   /* radio: reconnect, AI: new conversation */
            break;
        default: break;
        }
        if (s_menu != MENU_NONE && (int32_t)(now - s_menu_until) > 0) s_menu = MENU_NONE;

        settings_poll(now);        /* saves station / volume once they settle */

        /* RSSI is read by rssi_task: asking the WiFi chip can take up to ~1 s
         * (shared bus, power save in Bluetooth mode) and froze the screen */

        /* --- render --- */
        if (now - last_frame < FRAME_MS) continue;
        last_frame = now;
        gfx_lock();                /* drawing: the web page must not copy this half-done */
        gfx_clear(C_BG);
        if (g_ota_pct >= 0) {
            draw_ota();
            gfx_unlock();
            st7789_push(g_fb);
            continue;
        }
        if (s_info_until && (int32_t)(now - s_info_until) < 0) draw_info();
        else if (s_menu == MENU_STATIONS) draw_list(stations_count(), s_sel, g_radio.req_station, true, station_name);
        else if (s_menu == MENU_SOURCE) draw_list(NUM_SRC, s_sel, cur_source(), false, src_name);
        else draw_main();
        /* bottom line: volume for 2 s after any change (or while muted), else VU */
        if (g_radio.volume != last_vol || g_radio.muted != last_muted) {
            last_vol = g_radio.volume;
            last_muted = g_radio.muted;
            vol_until = now + VOLUME_SHOW_MS;
        }
        vu_update(now);
        if (g_radio.muted || (int32_t)(now - vol_until) < 0) draw_volume();
        else draw_vu();
        gfx_unlock();              /* the push only reads the frame */
        st7789_push(g_fb);
    }
}
