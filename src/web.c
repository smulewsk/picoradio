/*
 * Web remote: a tiny HTTP/1.0 server on port 80 (one client at a time).
 *
 *   GET /                    control page (web/index.html, embedded at build)
 *   GET /api/status          JSON: source, station, title, stream info, volume...
 *   GET /api/stations        JSON array of station names
 *   GET /api/screen.bmp      live copy of the TFT (284x76, 16-bit BMP)
 *   GET /api/screen.rle?h=.. the same, run-length coded (used by the page); 204 if unchanged
 *   GET /api/cmd?a=..&v=..   tune / source / vol / mute / play / next / prev / mic / reload
 *   POST /api/stations       edit the station list (add / edit / del / up / down / reset / import)
 *   POST /api/ota            firmware update: body = build/pico_radio.uf2 (or .bin), then reboot
 *   GET  /api/config         settings (WiFi, Gemini, names...); passwords/keys only as "set"
 *   POST /api/config         save settings (form); a=reset for the config.h defaults
 *   GET /stations.m3u|.pls   export the station list as a playlist
 *
 * Also announces itself as http://picoradio.local (mDNS).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>

#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/sockets.h"
#include "lwip/apps/mdns.h"

#include "radio.h"
#include "config.h"
#include "gfx.h"
#include "audio_i2s.h"
#include "bt_sink.h"
#include "tasks.h"
#include "web_page.h"
#include "stations.h"
#include "mem.h"
#include "ota.h"
#include "version.h"
#include "cfg.h"

#define REQ_MAX 1024

static char s_req[REQ_MAX + 1];
static char s_out[2048];
static bool s_reboot;              /* firmware written: reboot once the reply is sent */
static bool s_restart;             /* plain restart requested from the page */

/* ---------------- helpers ---------------- */

static void send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        int w = lwip_send(fd, b, n, 0);
        if (w <= 0) return;
        b += w;
        n -= (size_t)w;
    }
}

/* len < 0: no Content-Length, the body ends when the connection closes */
static void send_head_ex(int fd, int code, const char *ctype, long len, const char *extra) {
    char h[320];
    int n = snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n",
                     code, code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : "Not Found", ctype);
    if (len >= 0) n += snprintf(h + n, sizeof h - (size_t)n, "Content-Length: %ld\r\n", len);
    n += snprintf(h + n, sizeof h - (size_t)n, "%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
                  extra ? extra : "");
    send_all(fd, h, (size_t)n);
}

static void send_head(int fd, int code, const char *ctype, size_t len) {
    send_head_ex(fd, code, ctype, (long)len, NULL);
}

static void send_text(int fd, int code, const char *ctype, const char *body) {
    size_t n = strlen(body);
    send_head(fd, code, ctype, n);
    send_all(fd, body, n);
}

/* appends a JSON string literal */
static size_t json_str(char *o, size_t cap, const char *s) {
    size_t n = 0;
    if (n < cap) o[n++] = '"';
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    if (n < cap) o[n++] = '"';
    return n;
}

static const char *source_id(void) {
    int st = g_radio.req_station;
    return st == SOURCE_BT ? "bt" : st == SOURCE_AI ? "ai" : "radio";
}

static int level_db(uint16_t v) {
    return v ? (int)(20.0f * log10f((float)v / 32768.0f)) : -90;
}

/* ---------------- API ---------------- */

static void api_status(int fd) {
    static const char *const CODEC[] = { "", "MP3", "SBC", "PCM", "AAC", "AAC+" };
    static const char *const CH[] = { "", "MONO", "ST", "JS", "DUAL" };
    char title[240], info[64];
    radio_get_title_utf8(title, sizeof title);
    radio_get_info(info, sizeof info);
    uint16_t l, r;
    audio_levels(&l, &r);
    int st = g_radio.req_station;
    char sname[ST_NAME_MAX];
    const char *name = st == SOURCE_BT ? "Bluetooth" : st == SOURCE_AI ? "AI Gemini" :
                       stations_get(st, sname, sizeof sname, NULL, 0) ? sname : "?";

    size_t n = 0, cap = sizeof s_out;
    n += (size_t)snprintf(s_out + n, cap - n, "{\"src\":\"%s\",\"station\":%d,\"last\":%d,\"name\":",
                          source_id(), st, g_radio.last_station);
    n += json_str(s_out + n, cap - n, name);
    n += (size_t)snprintf(s_out + n, cap - n, ",\"title\":");
    n += json_str(s_out + n, cap - n, title);
    n += (size_t)snprintf(s_out + n, cap - n, ",\"info\":");
    n += json_str(s_out + n, cap - n, info);
    n += (size_t)snprintf(s_out + n, cap - n,
        ",\"state\":\"%s\",\"codec\":\"%s\",\"kbps\":%lu,\"rate\":%lu,\"ch\":\"%s\","
        "\"vol\":%d,\"volmax\":%d,\"muted\":%s,\"buf\":%lu,\"wifi\":%s,\"rssi\":%d,\"ip\":\"%s\","
        "\"vu\":[%d,%d],\"mic\":%d,\"micoff\":%s,\"fw\":\"%c\",\"otaslot\":\"%c\",\"otakb\":%lu,"
        "\"ver\":\"%s\",\"built\":\"%s\",\"ap\":%s}",
        radio_status_name(g_radio.status), CODEC[g_radio.codec],
        (unsigned long)g_radio.bitrate_kbps, (unsigned long)g_radio.sample_rate, CH[g_radio.chmode],
        g_radio.volume, VOLUME_MAX, g_radio.muted ? "true" : "false",
        (unsigned long)g_radio.buffer_pct, g_radio.wifi_up ? "true" : "false", g_radio.rssi, g_radio.ip,
        level_db(l), level_db(r), g_radio.mic_level, g_radio.mic_paused ? "true" : "false",
        ota_slot(), ota_target_slot(), (unsigned long)(ota_slot_size() / 1024),
        FW_VERSION_STR, FW_BUILD_STR, g_radio.ap_mode ? "true" : "false");
    send_text(fd, 200, "application/json", s_out);
}

/* {"max":31,"default":true,"list":[{"name":"..","url":".."},...]}, streamed */
static void api_stations(int fd) {
    send_head_ex(fd, 200, "application/json", -1, NULL);
    char name[ST_NAME_MAX], url[ST_URL_MAX];
    int n = snprintf(s_out, sizeof s_out, "{\"max\":%d,\"default\":%s,\"list\":[",
                     ST_MAX, stations_is_default() ? "true" : "false");
    send_all(fd, s_out, (size_t)n);
    for (int i = 0; stations_get(i, name, sizeof name, url, sizeof url); i++) {
        size_t k = 0, cap = sizeof s_out;
        k += (size_t)snprintf(s_out, cap, "%s{\"name\":", i ? "," : "");
        k += json_str(s_out + k, cap - k, name);
        k += (size_t)snprintf(s_out + k, cap - k, ",\"url\":");
        k += json_str(s_out + k, cap - k, url);
        s_out[k++] = '}';
        send_all(fd, s_out, k);
    }
    send_all(fd, "]}", 2);
}

/* station list as an M3U or PLS playlist (download, or open in VLC) */
static void export_playlist(int fd, bool pls) {
    send_head_ex(fd, 200, pls ? "audio/x-scpls" : "audio/x-mpegurl", -1,
                 pls ? "Content-Disposition: attachment; filename=\"picoradio.pls\"\r\n"
                     : "Content-Disposition: attachment; filename=\"picoradio.m3u\"\r\n");
    char name[ST_NAME_MAX], url[ST_URL_MAX];
    int count = stations_count();
    int n = pls ? snprintf(s_out, sizeof s_out, "[playlist]\r\nNumberOfEntries=%d\r\n", count)
                : snprintf(s_out, sizeof s_out, "#EXTM3U\r\n");
    send_all(fd, s_out, (size_t)n);
    for (int i = 0; stations_get(i, name, sizeof name, url, sizeof url); i++) {
        n = pls ? snprintf(s_out, sizeof s_out, "File%d=%s\r\nTitle%d=%s\r\nLength%d=-1\r\n", i + 1, url, i + 1, name, i + 1)
                : snprintf(s_out, sizeof s_out, "#EXTINF:-1,%s\r\n%s\r\n", name, url);
        send_all(fd, s_out, (size_t)n);
    }
    if (pls) send_all(fd, "Version=2\r\n", 11);
}

/* ---- POST /api/stations: application/x-www-form-urlencoded ---- */

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* in-place URL decoding (%XX and '+') */
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

typedef struct { const char *a, *i, *name, *url, *mode, *data; } form_t;

static void form_parse(char *body, form_t *f) {
    memset(f, 0, sizeof *f);
    for (char *p = body; p && *p; ) {
        char *amp = strchr(p, '&');
        if (amp) *amp = 0;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            url_decode(p);
            url_decode(eq + 1);
            const char *v = eq + 1;
            if (!strcmp(p, "a")) f->a = v;
            else if (!strcmp(p, "i")) f->i = v;
            else if (!strcmp(p, "name")) f->name = v;
            else if (!strcmp(p, "url")) f->url = v;
            else if (!strcmp(p, "mode")) f->mode = v;
            else if (!strcmp(p, "data")) f->data = v;
        }
        p = amp ? amp + 1 : NULL;
    }
}

static void api_stations_post(int fd, char *body) {
    form_t f;
    form_parse(body, &f);
    int i = f.i ? atoi(f.i) : -1;
    bool ok = false;
    int added = 0;
    const char *a = f.a ? f.a : "";
    if (!strcmp(a, "add")) ok = stations_add(f.name, f.url);
    else if (!strcmp(a, "edit")) ok = stations_update(i, f.name, f.url);
    else if (!strcmp(a, "del")) ok = stations_delete(i);
    else if (!strcmp(a, "up")) ok = stations_move(i, -1);
    else if (!strcmp(a, "down")) ok = stations_move(i, +1);
    else if (!strcmp(a, "reset")) ok = stations_reset();
    else if (!strcmp(a, "import") && f.data) {
        added = stations_import(f.data, f.mode && !strcmp(f.mode, "replace"));
        ok = added >= 0;
    }
    char r[64];
    snprintf(r, sizeof r, "{\"ok\":%s,\"added\":%d,\"count\":%d}", ok ? "true" : "false",
             added, stations_count());
    send_text(fd, ok ? 200 : 400, "application/json", r);
}

/* 16-bit BMP (BI_BITFIELDS RGB565), top-down, straight from the framebuffer */
static void api_screen(int fd) {
    const uint32_t row = TFT_WIDTH * 2, img = row * TFT_HEIGHT, off = 14 + 40 + 12;
    uint8_t h[66] = { 'B', 'M' };
    uint32_t v;
#define PUT32(o, x) do { v = (uint32_t)(x); memcpy(h + (o), &v, 4); } while (0)
    PUT32(2, off + img);
    PUT32(10, off);
    PUT32(14, 40);
    PUT32(18, TFT_WIDTH);
    PUT32(22, -(int32_t)TFT_HEIGHT);             /* negative = top-down rows */
    h[26] = 1; h[28] = 16;                       /* planes, bits per pixel */
    PUT32(30, 3);                                /* BI_BITFIELDS */
    PUT32(34, img);
    PUT32(54, 0xF800); PUT32(58, 0x07E0); PUT32(62, 0x001F);
#undef PUT32
    send_head(fd, 200, "image/bmp", off + img);
    send_all(fd, h, sizeof h);

    static uint16_t line[TFT_WIDTH * 4];
    for (int y = 0; y < TFT_HEIGHT; y += 4) {
        for (int i = 0; i < TFT_WIDTH * 4; i++) {
            uint16_t c = g_fb[y * TFT_WIDTH + i];   /* panel order is big-endian */
            line[i] = (uint16_t)((c >> 8) | (c << 8));
        }
        send_all(fd, line, sizeof line);
    }
}

/* GET /api/screen.rle?h=<hash of the frame the page has>
 * 204 if the screen hasn't changed, else: u32 hash, u16 width, u16 height,
 * then 16-bit words (little-endian): bit 15 set = run of (w & 0x7fff) copies of
 * the next word; clear = that many literal pixels follow. Pixels are RGB565 in
 * the panel's byte order (big-endian), as in g_fb. A typical frame is 2-8 KB
 * instead of the 43 KB BMP. */
static const char *query(const char *q, const char *key, char *out, size_t on);

/* RLE encoder into a memory buffer; returns the size, or 0 if it didn't fit */
typedef struct { uint8_t *p; size_t n, cap; bool full; } rle_out_t;

static void put_word(rle_out_t *o, uint16_t w) {
    if (o->n + 2 > o->cap) { o->full = true; return; }
    memcpy(o->p + o->n, &w, 2);
    o->n += 2;
}

static size_t rle_encode(const uint16_t *fb, uint32_t hash, uint8_t *buf, size_t cap) {
    const int total = TFT_WIDTH * TFT_HEIGHT;
    rle_out_t o = { buf, 0, cap, false };
    put_word(&o, (uint16_t)hash);
    put_word(&o, (uint16_t)(hash >> 16));
    put_word(&o, TFT_WIDTH);
    put_word(&o, TFT_HEIGHT);
    int lit_start = 0, lit = 0;                     /* pending literals */
    for (int i = 0; i < total && !o.full; ) {
        int j = i + 1;
        while (j < total && fb[j] == fb[i] && j - i < 0x7fff) j++;
        if (j - i >= 3 || lit == 0x7fff) {
            if (lit) {
                put_word(&o, (uint16_t)lit);
                for (int k = 0; k < lit; k++) put_word(&o, fb[lit_start + k]);
                lit = 0;
            }
            if (j - i >= 3) {
                put_word(&o, (uint16_t)(0x8000 | (j - i)));
                put_word(&o, fb[i]);
                i = j;
                continue;
            }
        }
        if (!lit) lit_start = i;
        lit++;
        i++;
    }
    if (lit) {
        put_word(&o, (uint16_t)lit);
        for (int k = 0; k < lit; k++) put_word(&o, fb[lit_start + k]);
    }
    return o.full ? 0 : o.n;
}

static uint32_t fb_hash(const uint16_t *fb) {
    uint32_t h = 2166136261u;                       /* FNV-1a over the frame */
    for (int i = 0; i < TFT_WIDTH * TFT_HEIGHT; i++) h = (h ^ fb[i]) * 16777619u;
    return h;
}

#define RLE_BUF 16384      /* typical frame 2-8 KB; a busier one is sent raw */

static void api_screen_rle(int fd, const char *q) {
    char hv[16];
    bool have = query(q, "h", hv, sizeof hv) != NULL;
    uint32_t known = have ? (uint32_t)strtoul(hv, NULL, 10) : 0;

    uint8_t *buf = xmem_alloc(RLE_BUF);
    if (!buf) { send_text(fd, 400, "text/plain", "out of memory\n"); return; }

    /* encode a consistent frame: the UI task holds this lock while drawing */
    gfx_lock();
    uint32_t h = fb_hash(g_fb);
    size_t n = 0;
    if (!have || known != h) n = rle_encode(g_fb, h, buf, RLE_BUF);
    if (have && known == h) {
        gfx_unlock();
        xmem_free(buf);
        send_head(fd, 204, "application/octet-stream", 0);
        return;
    }
    if (!n) {
        /* too busy to compress into the buffer (rare): send the raw frame as
         * one literal block, straight from the framebuffer */
        gfx_unlock();
        xmem_free(buf);
        const uint16_t total = TFT_WIDTH * TFT_HEIGHT;          /* 21584 < 0x7fff */
        uint16_t hdr[5] = { (uint16_t)h, (uint16_t)(h >> 16), TFT_WIDTH, TFT_HEIGHT, total };
        send_head(fd, 200, "application/octet-stream", sizeof hdr + (size_t)total * 2);
        send_all(fd, hdr, sizeof hdr);
        send_all(fd, g_fb, (size_t)total * 2);
        return;
    }
    gfx_unlock();
    send_head(fd, 200, "application/octet-stream", n);
    send_all(fd, buf, n);
    xmem_free(buf);
}

static const char *query(const char *q, const char *key, char *out, size_t on) {
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t i = 0;
            p += kl + 1;
            while (*p && *p != '&' && i + 1 < on) out[i++] = *p++;
            out[i] = 0;
            return out;
        }
    }
    out[0] = 0;
    return NULL;
}

static void api_cmd(int fd, const char *q) {
    char a[16], v[16];
    query(q, "a", a, sizeof a);
    query(q, "v", v, sizeof v);
    bool bt = g_radio.req_station == SOURCE_BT, ai = g_radio.req_station == SOURCE_AI;

    if (!strcmp(a, "tune")) {
        int n = atoi(v);
        if (n >= 0 && n < stations_count()) radio_tune(n);
    } else if (!strcmp(a, "source")) {
        if (!strcmp(v, "bt") && !bt) radio_tune(SOURCE_BT);
        else if (!strcmp(v, "ai") && !ai) { g_radio.mic_paused = false; radio_tune(SOURCE_AI); }
        else if (!strcmp(v, "radio") && (bt || ai)) radio_tune(g_radio.last_station);
    } else if (!strcmp(a, "vol")) {
        radio_set_volume(atoi(v));
        if (bt) bt_sync_volume(g_radio.volume);
    } else if (!strcmp(a, "mute")) {
        radio_toggle_mute();
    } else if (!strcmp(a, "play") && bt) {
        bt_play_pause();
    } else if (!strcmp(a, "next") && bt) {
        bt_next();
    } else if (!strcmp(a, "prev") && bt) {
        bt_prev();
    } else if (!strcmp(a, "mic") && ai) {
        g_radio.mic_paused = !g_radio.mic_paused;
    } else if (!strcmp(a, "restart")) {
        s_restart = true;
    } else if (!strcmp(a, "btforget")) {
        int left = bt_forget_all();
        send_text(fd, left ? 400 : 200, "application/json", left ? "{\"ok\":false}" : "{\"ok\":true}");
        return;
    } else if (!strcmp(a, "reload")) {
        radio_tune(g_radio.req_station);
    } else {
        send_text(fd, 404, "application/json", "{\"ok\":false}");
        return;
    }
    send_text(fd, 200, "application/json", "{\"ok\":true}");
}

/* ---- POST /api/ota: the firmware file as the raw body (application/octet-stream) ----
 * Streamed straight into the other flash slot; nothing is buffered beyond
 * one sector. Playback is muted while flash is written. */
static void ota_reply(int fd, const char *err) {
    if (!err) {
        snprintf(s_out, sizeof s_out, "{\"ok\":true,\"slot\":\"%c\"}", ota_target_slot());
        send_text(fd, 200, "application/json", s_out);
        return;
    }
    size_t n = (size_t)snprintf(s_out, sizeof s_out, "{\"ok\":false,\"error\":");
    n += json_str(s_out + n, sizeof s_out - n - 2, err);
    s_out[n++] = '}';
    s_out[n] = 0;
    send_text(fd, 400, "application/json", s_out);
}

/* reads and drops the rest of a refused upload, so the browser gets the
 * error reply instead of a connection reset */
static void drain(int fd, size_t left) {
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (left) {
        int n = lwip_recv(fd, s_out, left < sizeof s_out ? left : sizeof s_out, 0);
        if (n <= 0) return;
        left -= (size_t)n;
    }
}

static void api_ota(int fd, const char *first, size_t have, long blen) {
    if (blen <= 0) { ota_reply(fd, "Content-Length missing"); return; }
    if (have > (size_t)blen) have = (size_t)blen;
    const char *err = ota_begin((size_t)blen);
    if (err) { drain(fd, (size_t)blen - have); ota_reply(fd, err); return; }

    bool was_muted = g_radio.muted;
    if (!was_muted) radio_toggle_mute();
    if (g_radio.net_task) xTaskNotifyGive(g_radio.net_task);   /* stop the stream now (ota.c: g_ota_pct >= 0) */
    struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };      /* flash writes stall the cores */
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    size_t got = have;
    if (have) err = ota_write(first, have);
    while (!err && got < (size_t)blen) {
        size_t want = (size_t)blen - got;
        int n = lwip_recv(fd, s_out, want < sizeof s_out ? want : sizeof s_out, 0);
        if (n <= 0) { err = "upload interrupted"; break; }
        got += (size_t)n;
        err = ota_write(s_out, (size_t)n);
        g_ota_pct = (int)((uint64_t)got * 99 / (uint64_t)blen);
    }
    if (!err) err = ota_end();
    if (err) {
        if (got < (size_t)blen) drain(fd, (size_t)blen - got);
        ota_abort();
        if (!was_muted && g_radio.muted) radio_toggle_mute();
        printf("[web] update failed: %s\n", err);
    } else {
        s_reboot = true;
    }
    ota_reply(fd, err);
}

/* ---- settings: GET / POST /api/config ---- */

static void api_config_get(int fd) {
    static cfg_t c;
    cfg_copy(&c);
    size_t n = 0, cap = sizeof s_out;
#define JS(key, val) do { n += (size_t)snprintf(s_out + n, cap - n, "%s\"" key "\":", n > 1 ? "," : ""); \
                          n += json_str(s_out + n, cap - n, (val)); } while (0)
    s_out[n++] = '{';
    JS("ssid", c.wifi_ssid);
    /* the config.h placeholder doesn't count as a password */
    n += (size_t)snprintf(s_out + n, cap - n, ",\"pass_set\":%s,\"open\":%s",
                          c.wifi_pass[0] && strncmp(c.wifi_pass, "your-", 5) ? "true" : "false",
                          c.wifi_pass[0] ? "false" : "true");
    JS("hostname", c.hostname);
    JS("btname", c.bt_name);
    n += (size_t)snprintf(s_out + n, cap - n, ",\"key_set\":%s",
                          c.gemini_key[0] && strncmp(c.gemini_key, "your-", 5) ? "true" : "false");
    JS("model", c.gemini_model);
    JS("voice", c.gemini_voice);
    JS("prompt", c.ai_prompt);
    JS("lang", c.ai_language);
    n += (size_t)snprintf(s_out + n, cap - n, ",\"greet\":%s", c.ai_greet ? "true" : "false");
#undef JS
    n += (size_t)snprintf(s_out + n, cap - n, ",\"tls\":%s,\"default\":%s,\"promptmax\":%d}",
                          c.tls_verify ? "true" : "false", cfg_is_default() ? "true" : "false", CFG_PROMPT_MAX - 1);
    send_text(fd, 200, "application/json", s_out);
}

static bool hostname_ok(const char *h) {
    if (!*h || strlen(h) > 31 || *h == '-') return false;
    for (; *h; h++)
        if (!((*h >= 'a' && *h <= 'z') || (*h >= '0' && *h <= '9') || *h == '-')) return false;
    return true;
}

static void cfg_reply(int fd, bool ok, const char *err, bool restart) {
    size_t n = (size_t)snprintf(s_out, sizeof s_out, "{\"ok\":%s,\"restart\":%s", ok ? "true" : "false",
                                restart ? "true" : "false");
    if (err) {
        n += (size_t)snprintf(s_out + n, sizeof s_out - n, ",\"error\":");
        n += json_str(s_out + n, sizeof s_out - n - 2, err);
    }
    s_out[n++] = '}';
    s_out[n] = 0;
    send_text(fd, ok ? 200 : 400, "application/json", s_out);
}

/* form fields: ssid pass hostname btname key model voice lang greet prompt tls;
 * empty pass / key = keep the current one, clearpass=1 / clearkey=1 = remove it */
static void api_config_post(int fd, char *body) {
    static cfg_t c, old;
    cfg_copy(&c);
    old = c;
    bool reset = false, clearpass = false, clearkey = false;
    const char *err = NULL;
    for (char *p = body; p && *p && !err; ) {
        char *amp = strchr(p, '&');
        if (amp) *amp = 0;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            url_decode(p);
            url_decode(eq + 1);
            const char *v = eq + 1;
            size_t len = strlen(v);
#define FIELD(name, dst) else if (!strcmp(p, name)) { \
                if (len >= sizeof (dst)) err = name " is too long"; else memcpy(dst, v, len + 1); }
            if (!strcmp(p, "a")) reset = !strcmp(v, "reset");
            FIELD("ssid", c.wifi_ssid)
            FIELD("hostname", c.hostname)
            FIELD("btname", c.bt_name)
            FIELD("model", c.gemini_model)
            FIELD("voice", c.gemini_voice)
            FIELD("prompt", c.ai_prompt)
            FIELD("lang", c.ai_language)
#undef FIELD
            else if (!strcmp(p, "pass") && len) {
                if (len < 8 || len > 63) err = "WiFi password must be 8-63 characters";
                else memcpy(c.wifi_pass, v, len + 1);
            } else if (!strcmp(p, "key") && len) {
                if (len >= sizeof c.gemini_key) err = "API key is too long";
                else memcpy(c.gemini_key, v, len + 1);
            } else if (!strcmp(p, "clearpass")) clearpass = *v == '1';
            else if (!strcmp(p, "clearkey")) clearkey = *v == '1';
            else if (!strcmp(p, "tls")) c.tls_verify = *v == '1';
            else if (!strcmp(p, "greet")) c.ai_greet = *v == '1';
        }
        p = amp ? amp + 1 : NULL;
    }
    if (reset) {
        bool ok = cfg_reset();
        cfg_reply(fd, ok, ok ? NULL : "flash write failed", true);
        return;
    }
    if (clearpass) c.wifi_pass[0] = 0;
    if (clearkey) c.gemini_key[0] = 0;
    if (!err && !c.wifi_ssid[0]) err = "WiFi name is empty";
    if (!err && !hostname_ok(c.hostname)) err = "network name: lowercase letters, digits and -, up to 31";
    if (!err && !c.bt_name[0]) err = "Bluetooth name is empty";
    if (!err && (!c.gemini_model[0] || !c.gemini_voice[0])) err = "Gemini model and voice can't be empty";
    if (!err && !c.ai_language[0]) strcpy(c.ai_language, "auto");
    if (err) { cfg_reply(fd, false, err, false); return; }

    bool restart = strcmp(c.wifi_ssid, old.wifi_ssid) || strcmp(c.wifi_pass, old.wifi_pass) ||
                   strcmp(c.hostname, old.hostname) ||
                   strcmp(c.bt_name, old.bt_name);
    bool ai_changed = strcmp(c.gemini_key, old.gemini_key) || strcmp(c.gemini_model, old.gemini_model) ||
                      strcmp(c.gemini_voice, old.gemini_voice) || strcmp(c.ai_prompt, old.ai_prompt) ||
                      strcmp(c.ai_language, old.ai_language);
    if (!cfg_save(&c)) { cfg_reply(fd, false, "flash write failed", false); return; }
    printf("[web] settings saved%s\n", restart ? " (restart to apply WiFi / names)" : "");
    if (ai_changed && g_radio.req_station == SOURCE_AI) radio_tune(SOURCE_AI);   /* new AI session */
    cfg_reply(fd, true, NULL, restart);
}

/* ---------------- server ---------------- */

/* Value of request header `name` (case-insensitive), or NULL. Only the
 * header block is searched, never the body. */
static const char *req_header(const char *req, const char *hdr_end, const char *name) {
    size_t nl = strlen(name);
    const char *p = strstr(req, "\r\n");
    while (p && (!hdr_end || p < hdr_end)) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        p = strstr(p, "\r\n");
    }
    return NULL;
}

/* Requests that change something must carry "X-Pico-Radio: 1". Other web
 * sites can't add that header to requests to the radio without a CORS
 * preflight, which this server never answers, so a page open in the browser
 * can't flash firmware, change settings or send commands behind your back. */
static bool trusted(const char *req, const char *hdr_end) {
    const char *v = req_header(req, hdr_end, "X-Pico-Radio");
    return v && *v == '1';
}

static void handle(int fd) {
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    /* send replies right away: with Nagle on, the body waits for the browser to
     * ACK the header, and browsers delay ACKs by 40-200 ms, on every request */
    int one = 1;
    lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    size_t len = 0;
    while (len < REQ_MAX) {
        int n = lwip_recv(fd, s_req + len, REQ_MAX - len, 0);
        if (n <= 0) return;
        len += (size_t)n;
        s_req[len] = 0;
        if (strstr(s_req, "\r\n\r\n")) break;
    }
    bool post = !strncmp(s_req, "POST ", 5);
    if (!post && strncmp(s_req, "GET ", 4) != 0) {
        send_text(fd, 404, "text/plain", "only GET / POST\n");
        return;
    }
    char *hdr_end = strstr(s_req, "\r\n\r\n");
    char *path = s_req + (post ? 5 : 4);
    /* read the headers now: cutting the path below ends the string early */
    const bool is_trusted = trusted(s_req, hdr_end);
    const char *cl = req_header(s_req, hdr_end, "Content-Length");

    if (post) {
        /* body: what came with the headers + the rest (Content-Length) */
        long blen = cl ? atol(cl) : 0;
        if (!is_trusted) {
            send_text(fd, 403, "application/json",
                      "{\"ok\":false,\"error\":\"missing X-Pico-Radio header\"}");
            return;
        }
        if (hdr_end && strstr(s_req, "100-continue"))          /* curl, for big uploads */
            send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
        if (hdr_end && !strncmp(path, "/api/ota ", 9)) {
            api_ota(fd, hdr_end + 4, len - (size_t)(hdr_end + 4 - s_req), blen);
            return;
        }
        if (!hdr_end || blen <= 0 || blen > 16 * 1024) {
            send_text(fd, 400, "application/json", "{\"ok\":false}");
            return;
        }
        char *body = xmem_alloc((size_t)blen + 1);
        if (!body) { send_text(fd, 400, "application/json", "{\"ok\":false,\"error\":\"memory\"}"); return; }
        size_t have = len - (size_t)(hdr_end + 4 - s_req);
        if (have > (size_t)blen) have = (size_t)blen;
        memcpy(body, hdr_end + 4, have);
        while (have < (size_t)blen) {
            int n = lwip_recv(fd, body + have, (size_t)blen - have, 0);
            if (n <= 0) break;
            have += (size_t)n;
        }
        body[have] = 0;
        char *sp2 = strchr(path, ' ');
        if (sp2) *sp2 = 0;
        if (!strcmp(path, "/api/stations")) api_stations_post(fd, body);
        else if (!strcmp(path, "/api/config")) api_config_post(fd, body);
        else send_text(fd, 404, "text/plain", "not found\n");
        xmem_free(body);
        return;
    }
    char *sp = strchr(path, ' ');
    if (sp) *sp = 0;
    char *q = strchr(path, '?');
    if (q) *q++ = 0;

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        send_head_ex(fd, 200, "text/html; charset=utf-8", (long)sizeof WEB_PAGE_GZ,
                     "Content-Encoding: gzip\r\n");
        send_all(fd, WEB_PAGE_GZ, sizeof WEB_PAGE_GZ);
    } else if (!strcmp(path, "/api/status")) {
        api_status(fd);
    } else if (!strcmp(path, "/api/stations")) {
        api_stations(fd);
    } else if (!strcmp(path, "/api/config")) {
        api_config_get(fd);
    } else if (!strcmp(path, "/api/screen.rle")) {
        api_screen_rle(fd, q ? q : "");
    } else if (!strcmp(path, "/api/screen.bmp")) {
        api_screen(fd);
    } else if (!strcmp(path, "/stations.m3u")) {
        export_playlist(fd, false);
    } else if (!strcmp(path, "/stations.pls")) {
        export_playlist(fd, true);
    } else if (!strcmp(path, "/api/cmd")) {
        if (is_trusted) api_cmd(fd, q ? q : "");
        else send_text(fd, 403, "application/json", "{\"ok\":false,\"error\":\"missing X-Pico-Radio header\"}");
    } else if (g_radio.ap_mode) {
        /* setup network: phones probe e.g. /generate_204 or /hotspot-detect.html
         * to detect a login page; send them to the radio's page */
        static const char redir[] = "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/\r\n"
                                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, redir, sizeof redir - 1);
    } else {
        send_text(fd, 404, "text/plain", "not found\n");
    }
}

static void start_mdns(void) {
    cyw43_arch_lwip_begin();
    struct netif *nif = &cyw43_state.netif[g_radio.ap_mode ? CYW43_ITF_AP : CYW43_ITF_STA];
    mdns_resp_init();
    mdns_resp_add_netif(nif, cfg()->hostname);
    mdns_resp_add_service(nif, "Pico Radio", "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
    cyw43_arch_lwip_end();
    printf("[web] http://%s.local/  (http://%s/)\n", cfg()->hostname, g_radio.ip);
}

void web_task(void *arg) {
    (void)arg;
    ota_init();
    cfg_start();
    /* After an update the new firmware confirms itself once the web server
     * is up (so it can be updated again), or after 12 s without WiFi: the
     * boot ROM's trial-boot watchdog must not expire first. */
    bool confirmed = false;
    TickType_t t0 = xTaskGetTickCount();
    while (!g_radio.wifi_up && !g_radio.ap_mode) {
        if (!confirmed && xTaskGetTickCount() - t0 > pdMS_TO_TICKS(12000)) {
            ota_confirm_boot();
            confirmed = true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    start_mdns();

    int srv = lwip_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = PP_HTONS(80) };
    a.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
    if (srv < 0 || lwip_bind(srv, (struct sockaddr *)&a, sizeof a) != 0 || lwip_listen(srv, 2) != 0) {
        printf("[web] cannot listen on port 80\n");
        if (!confirmed) ota_confirm_boot();
        vTaskDelete(NULL);
    }
    if (!confirmed) ota_confirm_boot();

    /* Browsers open spare connections ahead of time and may send nothing on
     * them for seconds. Serving one connection at a time, an idle one would
     * block every other request (the live display froze ~1 s at a time), so
     * accepted connections wait here until a request has actually arrived. */
    enum { MAX_PENDING = 3, IDLE_MS = 10000 };
    int pend[MAX_PENDING];
    TickType_t since[MAX_PENDING];
    for (int i = 0; i < MAX_PENDING; i++) pend[i] = -1;

    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(srv, &rs);
        int maxfd = srv;
        for (int i = 0; i < MAX_PENDING; i++)
            if (pend[i] >= 0) { FD_SET(pend[i], &rs); if (pend[i] > maxfd) maxfd = pend[i]; }
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = lwip_select(maxfd + 1, &rs, NULL, NULL, &tv);
        TickType_t now = xTaskGetTickCount();

        if (r > 0) {
            /* requests first, then new connections */
            for (int i = 0; i < MAX_PENDING; i++) {
                if (pend[i] < 0 || !FD_ISSET(pend[i], &rs)) continue;
                handle(pend[i]);                /* data (or a close) is waiting */
                lwip_close(pend[i]);
                pend[i] = -1;
                if (s_reboot) {
                    vTaskDelay(pdMS_TO_TICKS(300));     /* let the reply reach the browser */
                    ota_reboot();
                }
                if (s_restart) {
                    printf("[web] restarting\n");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    watchdog_reboot(0, 0, 100);
                    for (;;) vTaskDelay(1000);
                }
            }
            if (FD_ISSET(srv, &rs)) {
                int fd = lwip_accept(srv, NULL, NULL);
                if (fd >= 0) {
                    int slot = -1, oldest = 0;
                    for (int i = 0; i < MAX_PENDING; i++) {
                        if (pend[i] < 0) { slot = i; break; }
                        if ((int32_t)(since[i] - since[oldest]) < 0) oldest = i;
                    }
                    if (slot < 0) {                     /* all busy: drop the longest-idle one */
                        lwip_close(pend[oldest]);
                        slot = oldest;
                    }
                    pend[slot] = fd;
                    since[slot] = now;
                }
            }
        } else if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        /* close connections that never sent a request */
        for (int i = 0; i < MAX_PENDING; i++)
            if (pend[i] >= 0 && now - since[i] > pdMS_TO_TICKS(IDLE_MS)) {
                lwip_close(pend[i]);
                pend[i] = -1;
            }
    }
}
