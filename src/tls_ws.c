/*
 * Secure WebSocket client (RFC 6455) on top of tls_conn (mbedTLS).
 * Single connection, used by the AI source to talk to the Gemini Live API.
 */
#include <stdio.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "pico/rand.h"
#include "FreeRTOS.h"
#include "task.h"

#include "tls_ws.h"
#include "tls_conn.h"

#define RX_BUF 2048

static bool s_open;
static uint8_t s_rx[RX_BUF];

/* ---------------- WebSocket frame parser ---------------- */

typedef struct {
    uint8_t  hdr[14];
    int      hdr_len, hdr_need;
    uint64_t remain;
    uint8_t  opcode;        /* opcode of the current frame */
    uint8_t  data_op;       /* opcode of the current (fragmented) message */
    bool     fin;
    bool     msg_started;   /* first chunk of the current message delivered */
    uint8_t  ctrl[125];
    int      ctrl_len;
} parser_t;

static parser_t P;
static uint8_t s_pending[512];   /* bytes after the HTTP upgrade response */
static size_t s_pending_len;

static void parser_reset(void) {
    memset(&P, 0, sizeof P);
    P.hdr_need = 2;
}

static int send_frame(uint8_t opcode, uint8_t *payload, size_t len) {
    uint8_t *h;
    size_t hl;
    if (len < 126) {
        hl = 6; h = payload - hl;
        h[1] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        hl = 8; h = payload - hl;
        h[1] = 0x80 | 126;
        h[2] = (uint8_t)(len >> 8); h[3] = (uint8_t)len;
    } else {
        hl = 14; h = payload - hl;
        h[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
    }
    h[0] = (uint8_t)(0x80 | opcode);             /* FIN + opcode */
    uint32_t m = get_rand_32();
    uint8_t *mask = h + hl - 4;
    memcpy(mask, &m, 4);
    for (size_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
    return tls_write(h, hl + len);
}

/* returns <0 when the server closed the connection */
static int parse(const uint8_t *d, size_t n, ws_data_cb cb, void *ctx) {
    while (n) {
        if (P.hdr_len < P.hdr_need) {
            P.hdr[P.hdr_len++] = *d++;
            n--;
            if (P.hdr_len == 2) {
                uint8_t l7 = P.hdr[1] & 0x7F;
                P.hdr_need = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + ((P.hdr[1] & 0x80) ? 4 : 0);
            }
            if (P.hdr_len < P.hdr_need) continue;

            uint8_t l7 = P.hdr[1] & 0x7F;
            if (l7 < 126) P.remain = l7;
            else if (l7 == 126) P.remain = ((uint64_t)P.hdr[2] << 8) | P.hdr[3];
            else { P.remain = 0; for (int i = 0; i < 8; i++) P.remain = (P.remain << 8) | P.hdr[2 + i]; }
            P.fin = (P.hdr[0] & 0x80) != 0;
            uint8_t op = P.hdr[0] & 0x0F;
            if (op == 1 || op == 2) { P.msg_started = false; P.data_op = op; }   /* new message */
            P.opcode = op ? op : P.data_op;                      /* 0 = continuation */
            P.ctrl_len = 0;
            if (P.remain == 0) goto frame_done;
            continue;
        }

        size_t take = n < P.remain ? n : (size_t)P.remain;
        if (P.opcode >= 8) {
            for (size_t i = 0; i < take && P.ctrl_len < (int)sizeof P.ctrl; i++) P.ctrl[P.ctrl_len++] = d[i];
        } else {
            cb(d, take, !P.msg_started, ctx);
            P.msg_started = true;
        }
        d += take; n -= take; P.remain -= take;
        if (P.remain) continue;

frame_done:
        if (P.opcode == 0x8) return -1;                          /* close */
        if (P.opcode == 0x9) {                                   /* ping -> pong */
            uint8_t buf[WS_HDR_ROOM + 125];
            memcpy(buf + WS_HDR_ROOM, P.ctrl, (size_t)P.ctrl_len);
            send_frame(0xA, buf + WS_HDR_ROOM, (size_t)P.ctrl_len);
        }
        P.hdr_len = 0;
        P.hdr_need = 2;
    }
    return 0;
}

/* ---------------- public API ---------------- */

void ws_close(void) {
    if (s_open) tls_close();
    s_open = false;
}

int ws_connect(const char *host, const char *path) {
    ws_close();
    parser_reset();
    s_pending_len = 0;

    /* the API key travels in the URL: always verify the server */
    if (tls_open(host, 443, true, 20) != 0) {
        printf("[ws] %s\n", tls_last_error());
        return -1;
    }
    s_open = true;

    uint8_t key_raw[16];
    for (int i = 0; i < 16; i += 4) { uint32_t v = get_rand_32(); memcpy(key_raw + i, &v, 4); }
    unsigned char key[32];
    size_t klen;
    mbedtls_base64_encode(key, sizeof key, &klen, key_raw, sizeof key_raw);
    key[klen] = 0;

    static char req[512];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n", path, host, key);
    if (n <= 0 || n >= (int)sizeof req || tls_write(req, (size_t)n) != 0) {
        ws_close();
        return -1;
    }

    size_t len = 0;
    char *end = NULL;
    TickType_t start = xTaskGetTickCount();
    while (!end) {
        if (len >= sizeof req - 1 || xTaskGetTickCount() - start > pdMS_TO_TICKS(10000)) {
            printf("[ws] no upgrade response\n");
            ws_close();
            return -1;
        }
        int r = tls_read(req + len, sizeof req - 1 - len);
        if (r == -1) continue;
        if (r <= 0) { ws_close(); return -1; }
        len += (size_t)r;
        req[len] = 0;
        end = strstr(req, "\r\n\r\n");
    }
    if (strncmp(req, "HTTP/1.1 101", 12) != 0) {
        char *eol = strchr(req, '\r');
        if (eol) *eol = 0;
        printf("[ws] upgrade refused: %s\n", req);
        ws_close();
        return -2;
    }
    end += 4;
    s_pending_len = len - (size_t)(end - req);
    if (s_pending_len > sizeof s_pending) s_pending_len = sizeof s_pending;
    memcpy(s_pending, end, s_pending_len);
    printf("[ws] WebSocket connected\n");
    return 0;
}

int ws_send_text(uint8_t *payload, size_t len) {
    if (!s_open) return -1;
    return send_frame(0x1, payload, len) == 0 ? 0 : -1;
}

int ws_poll(ws_data_cb cb, void *ctx) {
    if (!s_open) return -1;
    if (s_pending_len) {
        size_t n = s_pending_len;
        s_pending_len = 0;
        if (parse(s_pending, n, cb, ctx) < 0) return -1;
    }
    for (int i = 0; i < 16; i++) {           /* at most 32 KB per call */
        int r = tls_read(s_rx, sizeof s_rx);
        if (r == -1) return 0;               /* nothing more right now */
        if (r <= 0) return -1;               /* closed or error */
        if (parse(s_rx, (size_t)r, cb, ctx) < 0) return -1;
    }
    return 0;
}
