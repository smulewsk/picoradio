#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One TLS client connection over lwIP sockets (mbedTLS), shared by the
 * HTTPS radio client and the Gemini WebSocket. Only one is open at a time. */

#define TLS_ERR_CONNECT  -1   /* DNS / TCP failed */
#define TLS_ERR_HANDSHAKE -2  /* TLS failed (e.g. certificate not trusted) */

/* verify: check the server certificate against the bundled roots (ca_roots.h) */
int  tls_open(const char *host, uint16_t port, bool verify, int recv_timeout_ms);
int  tls_write(const void *p, size_t n);         /* 0 = everything sent */
/* > 0 bytes read, 0 = closed by server, -1 = nothing yet (timeout), -2 = error */
int  tls_read(void *p, size_t n);
void tls_close(void);
const char *tls_last_error(void);                 /* short text for the screen */
