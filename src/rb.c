/*
 * radio-browser.info station search, plain HTTP/1.0 (no chunked encoding),
 * JSON scanned as it arrives: the answer is an array of flat station objects
 * and we only keep a few fields of the playable ones.
 *
 *   GET /json/stations/search?name=..&tag=..&countrycode=..&hidebroken=true
 *       &order=clickcount&reverse=true&limit=25
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "rb.h"
#include "version.h"

static const char *const SERVERS[] = { "de1.api.radio-browser.info", "nl1.api.radio-browser.info",
                                       "at1.api.radio-browser.info" };

/* ---------------- URL query encoding ---------------- */

static size_t qput(char *o, size_t cap, const char *key, const char *v) {
    if (!v || !*v) return 0;
    size_t n = (size_t)snprintf(o, cap, "&%s=", key);
    for (; *v && n + 4 < cap; v++) {
        unsigned char c = (unsigned char)*v;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            o[n++] = (char)c;
        else n += (size_t)snprintf(o + n, cap - n, "%%%02X", c);
    }
    o[n] = 0;
    return n;
}

/* ---------------- JSON scanner ---------------- */

typedef struct {
    int depth;
    bool in_str, esc, is_value, key_done, expect;
    int uni_n; uint32_t uni;
    char key[24]; int kl;
    char *cap; size_t capn, capl;      /* string value being captured */
    char num[12]; int nl; bool in_num;
    rb_station_t cur;
    int hls, ok;
    char url2[ST_URL_MAX];             /* "url" when "url_resolved" is missing */
    rb_station_t *out; int max, n;
} rbscan_t;

static void cap_byte(rbscan_t *s, char c) {
    if (s->cap && s->capl + 1 < s->capn) { s->cap[s->capl++] = c; s->cap[s->capl] = 0; }
    else if (s->cap) s->capl = s->capn;            /* too long: mark (url is rejected) */
}

static void cap_cp(rbscan_t *s, uint32_t cp) {
    if (cp < 0x80) cap_byte(s, (char)cp);
    else if (cp < 0x800) { cap_byte(s, (char)(0xC0 | (cp >> 6))); cap_byte(s, (char)(0x80 | (cp & 0x3F))); }
    else { cap_byte(s, (char)(0xE0 | (cp >> 12))); cap_byte(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
           cap_byte(s, (char)(0x80 | (cp & 0x3F))); }
}

static bool playable(const rbscan_t *s) {
    const rb_station_t *c = &s->cur;
    if (s->hls || !s->ok) return false;
    if (!(strncmp(c->url, "http://", 7) == 0 || strncmp(c->url, "https://", 8) == 0)) return false;
    if (strlen(c->url) >= ST_URL_MAX - 1) return false;
    if (strstr(c->url, ".m3u8")) return false;
    return !strcasecmp(c->codec, "MP3") || !strcasecmp(c->codec, "AAC") || !strcasecmp(c->codec, "AAC+");
}

static void end_object(rbscan_t *s) {
    if (!s->cur.url[0] && s->url2[0]) memcpy(s->cur.url, s->url2, sizeof s->cur.url);
    if (s->n < s->max && s->cur.name[0] && playable(s)) {
        /* skip duplicates (same stream listed twice) */
        for (int i = 0; i < s->n; i++) if (!strcmp(s->out[i].url, s->cur.url)) return;
        s->out[s->n++] = s->cur;
    }
}

static void begin_value(rbscan_t *s) {
    s->cap = NULL;
    if (s->depth != 2) return;
    const char *k = s->key;
    if (!strcmp(k, "name"))              { s->cap = s->cur.name; s->capn = sizeof s->cur.name; }
    else if (!strcmp(k, "url_resolved")) { s->cap = s->cur.url; s->capn = sizeof s->cur.url; }
    else if (!strcmp(k, "url"))          { s->cap = s->url2; s->capn = sizeof s->url2; }
    else if (!strcmp(k, "codec"))        { s->cap = s->cur.codec; s->capn = sizeof s->cur.codec; }
    else if (!strcmp(k, "countrycode"))  { s->cap = s->cur.country; s->capn = sizeof s->cur.country; }
    if (s->cap) { s->capl = 0; s->cap[0] = 0; }
}

static void end_number(rbscan_t *s) {
    s->num[s->nl] = 0;
    int v = atoi(s->num);
    if (!strcmp(s->key, "bitrate")) s->cur.bitrate = v;
    else if (!strcmp(s->key, "hls")) s->hls = v;
    else if (!strcmp(s->key, "lastcheckok")) s->ok = v;
    s->in_num = false;
}

static void scan(rbscan_t *s, char c) {
    if (s->in_str) {
        if (s->uni_n) {
            int h = (c >= '0' && c <= '9') ? c - '0' : ((c | 32) >= 'a' && (c | 32) <= 'f') ? (c | 32) - 'a' + 10 : 0;
            s->uni = (s->uni << 4) | (uint32_t)h;
            if (--s->uni_n == 0 && s->is_value) cap_cp(s, s->uni);
            return;
        }
        if (s->esc) {
            s->esc = false;
            if (c == 'u') { s->uni_n = 4; s->uni = 0; return; }
            if (s->is_value) cap_byte(s, c == 'n' || c == 't' || c == 'r' ? ' ' : c);
            return;
        }
        if (c == '\\') { s->esc = true; return; }
        if (c == '"') {
            s->in_str = false;
            if (!s->is_value) { s->key[s->kl] = 0; s->key_done = true; }
            s->cap = NULL;
            return;
        }
        if (s->is_value) cap_byte(s, c);
        else if (s->kl < (int)sizeof s->key - 1) s->key[s->kl++] = c;
        return;
    }
    if (s->in_num) {
        if ((c >= '0' && c <= '9') || c == '-') { if (s->nl < (int)sizeof s->num - 1) s->num[s->nl++] = c; return; }
        end_number(s);
    }
    switch (c) {
    case '"':
        s->in_str = true; s->esc = false; s->uni_n = 0;
        if (s->expect) { s->is_value = true; s->expect = false; begin_value(s); }
        else { s->is_value = false; s->kl = 0; }
        return;
    case ':':
        if (s->key_done) { s->key_done = false; s->expect = true; }
        return;
    case '{':
        s->depth++; s->expect = false;
        if (s->depth == 2) { memset(&s->cur, 0, sizeof s->cur); s->url2[0] = 0; s->hls = 0; s->ok = 1; }
        return;
    case '}':
        if (s->depth == 2) end_object(s);
        s->depth--; s->expect = false;
        return;
    case '[': s->depth++; s->expect = false; return;
    case ']': s->depth--; s->expect = false; return;
    case ',': s->expect = false; return;
    default:
        if (s->expect && ((c >= '0' && c <= '9') || c == '-')) {
            s->expect = false; s->in_num = true; s->nl = 0; s->num[s->nl++] = c;
        } else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            s->expect = false;                     /* true / false / null */
        }
        return;
    }
}

/* ---------------- HTTP ---------------- */

static int query(const char *server, const char *path, rbscan_t *s) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (lwip_getaddrinfo(server, "80", &hints, &res) != 0 || !res) return -1;
    int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { lwip_freeaddrinfo(res); return -1; }
    struct timeval tv = { .tv_sec = 6, .tv_usec = 0 };
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (lwip_connect(fd, res->ai_addr, res->ai_addrlen) != 0) { lwip_freeaddrinfo(res); lwip_close(fd); return -1; }
    lwip_freeaddrinfo(res);

    static char req[640];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PicoRadio/%s\r\n"
                     "Accept: application/json\r\nConnection: close\r\n\r\n", path, server, FW_VERSION_STR);
    if (n <= 0 || n >= (int)sizeof req || lwip_send(fd, req, (size_t)n, 0) != n) { lwip_close(fd); return -1; }

    /* headers, then the body through the scanner */
    static char buf[512];
    int state = 0, code = 0, hdr = 0;           /* state: 0 = status line, 1 = headers, 2 = body */
    char line[64]; int ll = 0;
    for (;;) {
        int r = lwip_recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        for (int i = 0; i < r; i++) {
            char c = buf[i];
            if (state == 2) { scan(s, c); continue; }
            if (c == '\n') {
                line[ll] = 0;
                if (state == 0) { const char *sp = strchr(line, ' '); code = sp ? atoi(sp + 1) : 0; state = 1; }
                else if (ll == 0 || (ll == 1 && line[0] == '\r')) state = 2;
                ll = 0;
                hdr++;
            } else if (ll < (int)sizeof line - 1) line[ll++] = c;
        }
        if (s->n >= s->max && s->depth <= 1) break;
    }
    lwip_close(fd);
    return code == 200 ? 0 : -1;
}

int rb_search(const char *name, const char *tag, const char *cc, rb_station_t *out, int max) {
    static rbscan_t s;
    static char path[400];
    for (int attempt = 0; attempt < 2; attempt++) {
        /* second attempt: a "name" like "jazz" is often a genre */
        const char *qn = attempt == 0 ? name : NULL;
        const char *qt = attempt == 0 ? tag : name;
        if (attempt == 1 && (!name || !*name || (tag && *tag))) break;
        size_t n = (size_t)snprintf(path, sizeof path, "/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=25");
        n += qput(path + n, sizeof path - n, "name", qn);
        n += qput(path + n, sizeof path - n, "tag", qt);
        char ccu[4] = "";
        if (cc && cc[0] && cc[1]) { ccu[0] = (char)(cc[0] & ~32); ccu[1] = (char)(cc[1] & ~32); ccu[2] = 0; }
        qput(path + n, sizeof path - n, "countrycode", ccu);

        int rc = -1;
        for (unsigned i = 0; i < sizeof SERVERS / sizeof SERVERS[0] && rc != 0; i++) {
            memset(&s, 0, sizeof s);
            s.out = out; s.max = max;
            printf("[rb] %s%s\n", SERVERS[i], path);
            rc = query(SERVERS[i], path, &s);
        }
        if (rc != 0) return -1;
        printf("[rb] %d playable station(s)\n", s.n);
        if (s.n) return s.n;
    }
    return 0;
}
