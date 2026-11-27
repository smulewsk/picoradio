/*
 * Network task: WiFi connection + HTTP/HTTPS ICY (Shoutcast/Icecast)
 * streaming, following redirects and .pls/.m3u playlists.
 * MP3/AAC payload goes into g_radio.mp3_stream, ICY metadata -> title.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>

#include "pico/cyw43_arch.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"

#include "radio.h"
#include "ota.h"
#include "cfg.h"
#include "config.h"
#include "tasks.h"
#include "bt_sink.h"
#include "ai_gemini.h"
#include "tls_conn.h"
#include "stations.h"
#include "ap.h"
#include "hardware/watchdog.h"
#include "hardware/gpio.h"

#define URL_MAX        256
#define HDR_MAX        2048
#define RX_CHUNK       2048
#define NO_DATA_MS     8000
#define MAX_REDIRECTS  6          /* HTTP redirects + playlist hops */

static char    s_hdr[HDR_MAX + 1];
static uint8_t s_rx[RX_CHUNK];
static char    s_meta[255 * 16 + 1];   /* ICY metadata, also reused for playlists */

/* ------------------------------------------------------------------ */

/* false once the user tunes elsewhere, or while a firmware update runs
 * (the stream would slow the upload down a lot) */
static bool still_wanted(uint32_t seq) { return g_radio.tune_seq == seq && g_ota_pct < 0; }

/* http://host[:port]/path or https://host[:port]/path */
static bool parse_url(const char *url, bool *tls, char *host, size_t hn, uint16_t *port, char *path, size_t pn) {
    *tls = false;
    if (strncasecmp(url, "https://", 8) == 0) { *tls = true; url += 8; }
    else if (strncasecmp(url, "http://", 7) == 0) url += 7;
    else if (strstr(url, "://")) return false;          /* other schemes */
    const char *slash = strchr(url, '/');
    const char *hend = slash ? slash : url + strlen(url);
    const char *colon = memchr(url, ':', (size_t)(hend - url));
    size_t hl = (size_t)((colon ? colon : hend) - url);
    if (hl == 0 || hl >= hn) return false;
    memcpy(host, url, hl);
    host[hl] = 0;
    *port = colon ? (uint16_t)atoi(colon + 1) : (*tls ? 443 : 80);
    snprintf(path, pn, "%s", slash ? slash : "/");
    return true;
}

/* ---------------- plain TCP or TLS connection ---------------- */

typedef struct { int fd; bool tls; } conn_t;

static int conn_open(conn_t *c, const char *host, uint16_t port, bool tls) {
    c->tls = tls;
    c->fd = -1;
    if (tls) {
        if (tls_open(host, port, cfg()->tls_verify, 250) != 0) {
            radio_set_info(tls_last_error());
            return -1;
        }
        return 0;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", port);
    if (lwip_getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        radio_set_info("DNS lookup failed");
        return -1;
    }
    int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || lwip_connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        if (fd >= 0) lwip_close(fd);
        lwip_freeaddrinfo(res);
        radio_set_info("Connect failed");
        return -1;
    }
    lwip_freeaddrinfo(res);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    c->fd = fd;
    return 0;
}

static bool conn_send(conn_t *c, const char *s, size_t n) {
    if (c->tls) return tls_write(s, n) == 0;
    while (n) {
        int w = lwip_send(c->fd, s, n, 0);
        if (w <= 0) return false;
        s += w;
        n -= (size_t)w;
    }
    return true;
}

/* > 0 bytes, 0 = closed, -1 = nothing yet (timeout), -2 = error */
static int conn_recv(conn_t *c, void *buf, size_t n) {
    if (c->tls) return tls_read(buf, n);
    int r = lwip_recv(c->fd, buf, n, 0);
    if (r >= 0) return r;
    /* lwIP keeps its own errno (LWIP_PROVIDE_ERRNO), so ask the socket:
     * SO_ERROR stays 0 after a plain receive timeout */
    int e = 0;
    socklen_t len = sizeof e;
    lwip_getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &len);
    return (e == 0 || e == EWOULDBLOCK || e == EAGAIN) ? -1 : -2;
}

static void conn_close(conn_t *c) {
    if (c->tls) tls_close();
    else if (c->fd >= 0) lwip_close(c->fd);
    c->fd = -1;
    c->tls = false;
}

/* Finds header value (case-insensitive name), copies trimmed value. */
static bool hdr_get(const char *hdr, const char *name, char *out, size_t on) {
    size_t nl = strlen(name);
    const char *p = hdr;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            size_t i = 0;
            while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < on) { out[i] = p[i]; i++; }
            out[i] = 0;
            return true;
        }
    }
    return false;
}

/* Reads response headers. Returns HTTP status, body bytes already read in *body/*body_len. */
static int read_headers(conn_t *c, uint32_t seq, const char **body, size_t *body_len) {
    size_t len = 0;
    TickType_t start = xTaskGetTickCount();
    while (len < HDR_MAX && still_wanted(seq)) {
        int n = conn_recv(c, s_hdr + len, HDR_MAX - len);
        if (n == 0 || n == -2) return -1;
        if (n < 0) {
            if (xTaskGetTickCount() - start > pdMS_TO_TICKS(NO_DATA_MS)) return -1;
            continue;
        }
        len += (size_t)n;
        s_hdr[len] = 0;
        char *end = strstr(s_hdr, "\r\n\r\n");
        size_t skip = 4;
        if (!end) { end = strstr(s_hdr, "\n\n"); skip = 2; }
        if (end) {
            *body = end + skip;
            *body_len = len - (size_t)(end + skip - s_hdr);
            *end = 0;   /* terminate header block */
            /* "HTTP/1.1 200 OK": the number after the first word. Parsed by
             * hand: sscanf() alone pulls ~46 KB of newlib into the firmware. */
            const char *p = s_hdr;
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p < '0' || *p > '9') return -1;
            int code = 0;
            while (*p >= '0' && *p <= '9') code = code * 10 + (*p++ - '0');
            return code;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Playlists (.pls / .m3u): station links often point to one of these   */

static bool ends_with(const char *path, const char *ext) {
    size_t n = strcspn(path, "?#");                    /* ignore the query string */
    size_t e = strlen(ext);
    return n >= e && strncasecmp(path + n - e, ext, e) == 0;
}

static bool looks_like_playlist(const char *ctype, const char *path, const char *body, size_t blen) {
    if (strstr(ctype, "scpls") || strstr(ctype, "mpegurl") || strstr(ctype, "x-pls")) return true;
    if (ends_with(path, ".pls") || ends_with(path, ".m3u") || ends_with(path, ".m3u8")) return true;
    while (blen && (*body == ' ' || *body == '\r' || *body == '\n')) { body++; blen--; }
    return (blen >= 10 && strncasecmp(body, "[playlist]", 10) == 0) ||
           (blen >= 7 && strncmp(body, "#EXTM3U", 7) == 0);
}

/* First stream URL in a PLS or M3U playlist. Returns false (and sets
 * info) for HLS playlists or when no URL is found. */
static bool playlist_first_url(const char *text, char *out, size_t on) {
    if (strstr(text, "#EXT-X-")) {
        radio_set_info("HLS (m3u8) streams not supported");
        return false;
    }
    const char *p = text;
    while (*p) {
        const char *eol = p + strcspn(p, "\r\n");
        while (p < eol && (*p == ' ' || *p == '\t')) p++;
        const char *v = NULL;
        if (strncasecmp(p, "File", 4) == 0) {                 /* PLS: File1=http://... */
            const char *eq = memchr(p, '=', (size_t)(eol - p));
            if (eq) v = eq + 1;
        } else if (strncasecmp(p, "http://", 7) == 0 || strncasecmp(p, "https://", 8) == 0) {
            v = p;                                             /* M3U: bare URL line */
        }
        if (v) {
            while (v < eol && *v == ' ') v++;
            size_t n = (size_t)(eol - v);
            while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
            if (n > 0 && n < on) {
                memcpy(out, v, n);
                out[n] = 0;
                return true;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    radio_set_info("Playlist has no stream URL");
    return false;
}

/* Reads the rest of a (small) playlist body into s_meta. */
static void read_playlist(conn_t *c, uint32_t seq, const char *body, size_t blen) {
    size_t cap = sizeof s_meta - 1, len = blen < cap ? blen : cap;
    memcpy(s_meta, body, len);
    TickType_t start = xTaskGetTickCount();
    while (len < cap && still_wanted(seq) && xTaskGetTickCount() - start < pdMS_TO_TICKS(4000)) {
        int n = conn_recv(c, s_meta + len, cap - len);
        if (n == 0 || n == -2) break;                 /* end of file */
        if (n > 0) len += (size_t)n;
    }
    s_meta[len] = 0;
}

/* ------------------------------------------------------------------ */
/* ICY metadata demux                                                  */

typedef struct {
    uint32_t metaint;
    uint32_t until_meta;
    int      meta_len;   /* -1 = waiting for length byte */
    int      meta_pos;
} icy_t;

static void parse_metadata(const char *m) {
    const char *p = strstr(m, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");
    if (!e) e = p + strlen(p);
    char title[160];
    size_t n = (size_t)(e - p);
    if (n >= sizeof title) n = sizeof title - 1;
    memcpy(title, p, n);
    title[n] = 0;
    radio_set_title(title);
}

static void push_audio(const uint8_t *d, size_t n, uint32_t seq) {
    while (n && still_wanted(seq)) {
        size_t w = xStreamBufferSend(g_radio.mp3_stream, d, n, pdMS_TO_TICKS(100));
        d += w;
        n -= w;
    }
}

static void icy_feed(icy_t *s, const uint8_t *d, size_t n, uint32_t seq) {
    while (n && still_wanted(seq)) {
        if (s->metaint == 0 || s->until_meta > 0) {
            size_t c = n;
            if (s->metaint && c > s->until_meta) c = s->until_meta;
            push_audio(d, c, seq);
            d += c; n -= c;
            if (s->metaint) s->until_meta -= (uint32_t)c;
        } else if (s->meta_len < 0) {
            s->meta_len = d[0] * 16;
            s->meta_pos = 0;
            d++; n--;
            if (s->meta_len == 0) {
                s->meta_len = -1;
                s->until_meta = s->metaint;
            }
        } else {
            size_t c = (size_t)(s->meta_len - s->meta_pos);
            if (c > n) c = n;
            memcpy(s_meta + s->meta_pos, d, c);
            s->meta_pos += (int)c;
            d += c; n -= c;
            if (s->meta_pos == s->meta_len) {
                s_meta[s->meta_len] = 0;
                parse_metadata(s_meta);
                s->meta_len = -1;
                s->until_meta = s->metaint;
            }
        }
    }
}

/* ------------------------------------------------------------------ */


/* Streams one station until it is changed or an error occurs. */
static void stream_station(int idx, uint32_t seq) {
    static char url[URL_MAX], host[128], path[URL_MAX], req[URL_MAX + 256], val[96];

    g_radio.source = SRC_RADIO;
    radio_flush_decoder();
    radio_set_title("");
    radio_set_info("");
    radio_set_status(RS_CONNECTING);
    if (!stations_get(idx, NULL, 0, url, sizeof url)) {
        radio_set_info("No such station");
        radio_set_status(RS_ERROR);
        return;
    }
    printf("[net] tuning %d: %s\n", idx, url);

    conn_t conn = { .fd = -1 };
    bool open = false;
    const char *body = NULL;
    size_t body_len = 0;
    int code = 0;

    /* follow HTTP redirects and playlist files until we reach the stream */
    for (int hop = 0; hop <= MAX_REDIRECTS && still_wanted(seq); hop++) {
        uint16_t port;
        bool tls;
        if (!parse_url(url, &tls, host, sizeof host, &port, path, sizeof path)) {
            radio_set_info("Bad URL");
            radio_set_status(RS_ERROR);
            return;
        }
        if (conn_open(&conn, host, port, tls) != 0) { radio_set_status(RS_ERROR); return; }
        open = true;

        int n = snprintf(req, sizeof req,
                         "GET %s HTTP/1.0\r\n"
                         "Host: %s\r\n"
                         "User-Agent: PicoRadio/1.0\r\n"
                         "Accept: */*\r\n"
                         "Icy-MetaData: 1\r\n"
                         "Connection: close\r\n\r\n", path, host);
        if (!conn_send(&conn, req, (size_t)n)) {
            conn_close(&conn);
            radio_set_info("Send failed");
            radio_set_status(RS_ERROR);
            return;
        }
        code = read_headers(&conn, seq, &body, &body_len);
        if (code >= 300 && code < 400 && hdr_get(s_hdr, "Location", url, sizeof url)) {
            printf("[net] redirect -> %s\n", url);
            conn_close(&conn);
            open = false;
            continue;
        }
        if (code != 200) break;

        val[0] = 0;
        if (hdr_get(s_hdr, "Content-Type", val, sizeof val))
            for (char *c = val; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        if (looks_like_playlist(val, path, body, body_len)) {
            read_playlist(&conn, seq, body, body_len);
            conn_close(&conn);
            open = false;
            if (!playlist_first_url(s_meta, url, sizeof url)) { radio_set_status(RS_ERROR); return; }
            printf("[net] playlist -> %s\n", url);
            continue;
        }
        break;                                         /* it's the stream */
    }
    if (!still_wanted(seq)) { if (open) conn_close(&conn); return; }
    if (!open) { radio_set_info("Too many redirects"); radio_set_status(RS_ERROR); return; }
    if (code != 200) {
        char msg[40];
        snprintf(msg, sizeof msg, code < 0 ? "No response" : "HTTP error %d", code);
        radio_set_info(msg);
        radio_set_status(RS_ERROR);
        conn_close(&conn);
        return;
    }

    /* MP3 or ADTS AAC / HE-AAC; other containers are not supported */
    g_radio.stream_fmt = strstr(val, "aac") ? FMT_AAC : FMT_MP3;   /* audio/aac, aacp, x-aac */
    if (val[0] && (strstr(val, "ogg") || strstr(val, "opus") || strstr(val, "flac") ||
                   strstr(val, "mp4") || strstr(val, "latm"))) {
        snprintf(req, sizeof req, "Unsupported: %s", val);
        radio_set_info(req);
        radio_set_status(RS_ERROR);
        conn_close(&conn);
        return;
    }

    icy_t icy = { .metaint = 0, .meta_len = -1 };
    if (hdr_get(s_hdr, "icy-metaint", val, sizeof val)) icy.metaint = (uint32_t)atoi(val);
    icy.until_meta = icy.metaint;
    if (hdr_get(s_hdr, "icy-name", val, sizeof val)) printf("[net] icy-name: %s\n", val);
    printf("[net] connected%s, %s, metaint=%lu\n", conn.tls ? " (https)" : "",
           g_radio.stream_fmt == FMT_AAC ? "AAC" : "MP3", (unsigned long)icy.metaint);

    radio_set_status(RS_BUFFERING);
    if (body_len) {
        /* body points into s_hdr; copy out before reuse */
        memcpy(s_rx, body, body_len);
        icy_feed(&icy, s_rx, body_len, seq);
    }

    TickType_t last_data = xTaskGetTickCount();
    while (still_wanted(seq)) {
        int n = conn_recv(&conn, s_rx, sizeof s_rx);
        if (n > 0) {
            last_data = xTaskGetTickCount();
            icy_feed(&icy, s_rx, (size_t)n, seq);
        } else if (n == 0 || n == -2) {
            radio_set_info("Stream closed");
            break;
        } else if (xTaskGetTickCount() - last_data > pdMS_TO_TICKS(NO_DATA_MS)) {
            radio_set_info("Stream timeout");
            break;
        }
    }
    conn_close(&conn);
    if (still_wanted(seq)) radio_set_status(RS_ERROR);
}

static bool s_ever_connected;   /* WiFi worked once since power-on */
static bool s_want_ap;          /* joining failed AP_AFTER_FAILS times: open the setup AP */
static bool s_bt_started;

/* Connects WiFi. Gives up (returns false) if the user switches to Bluetooth,
 * or (before the first success) after AP_AFTER_FAILS failed joins. */
static bool wifi_connect(void) {
    g_radio.wifi_up = false;
    radio_set_status(RS_WIFI_CONNECTING);
    radio_set_info("Connecting WiFi...");
    static cfg_t c;                     /* static: keeps it off the task stack */
    cfg_copy(&c);
    printf("[net] connecting to %s\n", c.wifi_ssid);
    /* power save off before joining too: the join is more reliable */
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);
    int rc, fails = 0;
    while ((rc = cyw43_arch_wifi_connect_timeout_ms(c.wifi_ssid, c.wifi_pass[0] ? c.wifi_pass : NULL,
                                                    c.wifi_pass[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN,
                                                    20000)) != 0) {
        if (g_radio.req_station == SOURCE_BT) return false;
        const char *why = rc == PICO_ERROR_BADAUTH ? "wrong password" :
                          rc == PICO_ERROR_TIMEOUT ? "timeout" :
                          rc == PICO_ERROR_CONNECT_FAILED ? "network not found or refused" : "error";
        radio_set_info("WiFi failed, retrying");
        printf("[net] wifi failed (%d, %s), retrying\n", rc, why);
        /* never joined since power-on: probably wrong settings, offer the setup AP.
         * After a working connection keep retrying (router restart etc.). */
        if (!s_ever_connected && ++fails >= AP_AFTER_FAILS) {
            s_want_ap = true;
            return false;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        if (g_radio.req_station == SOURCE_BT) return false;
    }
    /* full-speed WiFi: power save causes stutter */
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);
    snprintf(g_radio.ip, sizeof g_radio.ip, "%s",
             ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
    printf("[net] connected, IP %s\n", g_radio.ip);
    radio_set_info("");
    g_radio.wifi_up = true;
    s_ever_connected = true;
    return true;
}

/* Bluetooth source: the phone streams A2DP, we just wait for a change. */
static void bluetooth_mode(uint32_t seq) {
    printf("[net] switching to Bluetooth\n");
    g_radio.source = SRC_BT;
    radio_flush_decoder();
    radio_set_title("");
    /* WiFi and Bluetooth share one radio in the CYW43. With WiFi power save
     * off (needed for streaming) Bluetooth gets too little air time and
     * pairing times out, so let WiFi doze while the phone is the source. */
    cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM);
    bt_activate(true);
    while (g_radio.tune_seq == seq) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));   /* BT keeps playing during OTA */
    bt_activate(false);
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);
    radio_set_title("");
    printf("[net] leaving Bluetooth\n");
}

/* Setup access point: the radio becomes its own WiFi network so the settings
 * page can be reached without a router. Left by restarting (saving new WiFi
 * settings restarts), or by itself after AP_IDLE_MIN minutes without a
 * client when a network is configured. Bluetooth still works meanwhile. */
static void setup_ap_mode(bool forced) {
    const char *pw = AP_PASSWORD;
    printf("[net] WiFi setup: access point \"%s\"%s%s, http://192.168.4.1/\n", AP_SSID,
           pw[0] ? " password " : " (open)", pw);
    g_radio.wifi_up = false;
    cyw43_arch_disable_sta_mode();
    cyw43_arch_enable_ap_mode(AP_SSID, pw[0] ? pw : NULL, pw[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    ap_servers_start();
    snprintf(g_radio.ip, sizeof g_radio.ip, "192.168.4.1");
    g_radio.ap_mode = true;

    static char info[64];
    if (pw[0]) snprintf(info, sizeof info, "Join WiFi %s (%s), open 192.168.4.1", AP_SSID, pw);
    else snprintf(info, sizeof info, "Join WiFi %s, open 192.168.4.1", AP_SSID);

    TickType_t idle_since = xTaskGetTickCount();
    for (;;) {
        uint32_t seq = g_radio.tune_seq;
        if (g_radio.req_station == SOURCE_BT && g_ota_pct < 0) {
            if (!s_bt_started) { bt_init(); s_bt_started = true; }
            radio_set_info("");
            bluetooth_mode(seq);
            continue;
        }
        if (g_radio.source != SRC_RADIO) { g_radio.source = SRC_RADIO; radio_flush_decoder(); }
        radio_set_status(RS_WIFI_SETUP);
        radio_set_info(info);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        TickType_t now = xTaskGetTickCount();
        if (ap_client_count() > 0 || g_ota_pct >= 0) idle_since = now;
        if (cfg_wifi_set() && !forced && now - idle_since > pdMS_TO_TICKS(AP_IDLE_MIN * 60000)) {
            printf("[net] nobody joined the setup network, retrying the saved WiFi\n");
            watchdog_reboot(0, 0, 100);
            for (;;) vTaskDelay(1000);
        }
    }
}

void net_task(void *arg) {
    (void)arg;
    if (cyw43_arch_init()) {
        radio_set_info("CYW43 init failed");
        radio_set_status(RS_ERROR);
        vTaskDelete(NULL);
    }
    /* encoder 2 pressed at power-on, or during the first 2 s (while the logo
     * shows): open the setup network instead of joining WiFi */
    bool force_ap = cfg_force_ap();
    for (int i = 0; i < 20 && !force_ap; i++) {
        if (!gpio_get(PIN_ENC2_SW)) {
            vTaskDelay(pdMS_TO_TICKS(50));               /* debounce */
            force_ap = !gpio_get(PIN_ENC2_SW);
        }
        if (!force_ap) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (force_ap) printf("[net] encoder 2 held at start: WiFi setup access point\n");
    if (force_ap || !cfg_wifi_set()) setup_ap_mode(force_ap);   /* does not return */
    cyw43_arch_enable_sta_mode();

    for (;;) {
        uint32_t seq = g_radio.tune_seq;
        int idx = g_radio.req_station;

        /* Bluetooth starts once WiFi is up (or when it's picked as the
         * source): starting both at once makes the first WiFi join fail. */
        if (!s_bt_started && (g_radio.wifi_up || idx == SOURCE_BT)) {
            bt_init();
            s_bt_started = true;
        }

        if (idx == SOURCE_BT) {
            bluetooth_mode(seq);
            continue;
        }
        if (g_ota_pct >= 0) {          /* firmware update: stream paused, resumes if it fails */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
            g_radio.wifi_up = false;
            if (!wifi_connect()) {
                if (s_want_ap) setup_ap_mode(false);                    /* does not return */
                continue;
            }
        }
        if (!s_bt_started) {           /* WiFi is up now: start Bluetooth before streaming */
            bt_init();
            s_bt_started = true;
        }
        if (idx == SOURCE_AI) {
            ai_mode(seq);
            continue;
        }
        stream_station(idx, seq);
        if (still_wanted(seq)) {
            /* error: retry after 3 s unless the user tunes elsewhere */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
        }
    }
}
