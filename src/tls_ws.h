#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal secure WebSocket (wss://) client: one connection at a time,
 * used by the AI source. Built on lwIP sockets + mbedTLS. */

#define WS_HDR_ROOM 14   /* bytes the caller must leave free before a payload */

/* Called for every chunk of an incoming text/binary message. */
typedef void (*ws_data_cb)(const uint8_t *data, size_t len, bool msg_start, void *ctx);

int  ws_connect(const char *host, const char *path);  /* 0 = connected */
/* payload must have WS_HDR_ROOM writable bytes in front of it (it is masked in place) */
int  ws_send_text(uint8_t *payload, size_t len);      /* 0 = sent */
/* Reads whatever has arrived (waits at most ~20 ms). <0 = connection closed. */
int  ws_poll(ws_data_cb cb, void *ctx);
void ws_close(void);
