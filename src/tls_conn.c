/*
 * TLS 1.2 client over lwIP sockets with mbedTLS.
 *
 * Trust: instead of parsing every bundled root into RAM, a CA callback
 * looks up only the roots whose subject matches the issuer of the
 * certificate being checked, and parses just those (ca_roots.h, ~23 roots
 * kept in flash). Memory comes from xmem_alloc (whichever heap has room).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"
#include "FreeRTOS.h"
#include "task.h"

#include "tls_conn.h"
#include "ca_roots.h"
#include "mem.h"

#define ERR_NET_RECV  (-0x004C)
#define ERR_NET_SEND  (-0x004E)
#define ERR_NET_RESET (-0x0050)

static int s_fd = -1;
static bool s_ready;
static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_conf;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static char s_err[48];
static int s_sock_errno;               /* errno of the last failed socket call */

const char *tls_last_error(void) { return s_err; }

/* ---------------- socket glue ---------------- */

/* The build sets LWIP_PROVIDE_ERRNO, so lwIP keeps its own errno and the C
 * library's errno (what <errno.h> gives us) stays 0. Ask the socket instead:
 * SO_ERROR is 0 after a receive timeout, and the real error (reset,
 * aborted...) once the connection is dead. */
static int sock_error(void) {
    int e = 0;
    socklen_t len = sizeof e;
    if (lwip_getsockopt(s_fd, SOL_SOCKET, SO_ERROR, &e, &len) != 0) return -1;
    return e;
}

static bool would_block(void) {
    int e = sock_error();
    if (e == 0 || e == EWOULDBLOCK || e == EAGAIN) return true;
    s_sock_errno = e;
    return false;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    int n = lwip_send(s_fd, buf, len, 0);
    if (n >= 0) return n;
    return would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : ERR_NET_SEND;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    int n = lwip_recv(s_fd, buf, len, 0);
    if (n > 0) return n;
    if (n == 0) return ERR_NET_RESET;
    return would_block() ? MBEDTLS_ERR_SSL_WANT_READ : ERR_NET_RECV;
}

/* ---------------- trust: roots on demand ---------------- */

static int ca_lookup(void *ctx, const mbedtls_x509_crt *child, mbedtls_x509_crt **candidates) {
    (void)ctx;
    mbedtls_x509_crt *list = NULL;
    for (int i = 0; i < CA_ROOT_COUNT; i++) {
        mbedtls_x509_crt *c = mbedtls_calloc(1, sizeof *c);
        if (!c) break;
        mbedtls_x509_crt_init(c);
        if (mbedtls_x509_crt_parse_der_nocopy(c, CA_ROOTS[i].der, CA_ROOTS[i].len) == 0 &&
            c->subject_raw.len == child->issuer_raw.len &&
            memcmp(c->subject_raw.p, child->issuer_raw.p, c->subject_raw.len) == 0) {
            c->next = list;                       /* keep this one */
            list = c;
            printf("[tls] trusted root: %s\n", CA_ROOTS[i].name);
        } else {
            mbedtls_x509_crt_free(c);
            mbedtls_free(c);
        }
    }
    *candidates = list;                           /* mbedTLS frees the list */
    return 0;
}

static void set_err(const char *what, int r) {
    char m[64];
    if (r == ERR_NET_RECV || r == ERR_NET_SEND)
        snprintf(m, sizeof m, "socket error %d%s", s_sock_errno,
                 s_sock_errno == ECONNRESET ? " (server reset the connection)" : "");
    else if (r == ERR_NET_RESET)
        snprintf(m, sizeof m, "server closed the connection");
    else
        mbedtls_strerror(r, m, sizeof m);
    printf("[tls] %s: -0x%04x %s\n", what, (unsigned)-r, m);
    snprintf(s_err, sizeof s_err, "%s", what);
}

/* ---------------- API ---------------- */

void tls_close(void) {
    if (s_ready) {
        mbedtls_ssl_close_notify(&s_ssl);
        mbedtls_ssl_free(&s_ssl);
        mbedtls_ssl_config_free(&s_conf);
        mbedtls_ctr_drbg_free(&s_drbg);
        mbedtls_entropy_free(&s_entropy);
        s_ready = false;
    }
    if (s_fd >= 0) {
        lwip_close(s_fd);
        s_fd = -1;
    }
}

int tls_open(const char *host, uint16_t port, bool verify, int recv_timeout_ms) {
    static bool platform_set;
    if (!platform_set) {
        mbedtls_platform_set_calloc_free(xmem_calloc, xmem_free);
        platform_set = true;
    }
    tls_close();
    s_err[0] = 0;

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", port);
    if (lwip_getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        snprintf(s_err, sizeof s_err, "DNS lookup failed");
        return TLS_ERR_CONNECT;
    }
    s_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s_fd < 0 || lwip_connect(s_fd, res->ai_addr, res->ai_addrlen) != 0) {
        lwip_freeaddrinfo(res);
        tls_close();
        snprintf(s_err, sizeof s_err, "Connect failed");
        return TLS_ERR_CONNECT;
    }
    lwip_freeaddrinfo(res);
    struct timeval tv = { .tv_sec = recv_timeout_ms / 1000, .tv_usec = (recv_timeout_ms % 1000) * 1000 };
    lwip_setsockopt(s_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    lwip_setsockopt(s_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_ctr_drbg_init(&s_drbg);
    mbedtls_entropy_init(&s_entropy);
    s_ready = true;

    int r;
    if ((r = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy, NULL, 0)) != 0 ||
        (r = mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        set_err("TLS setup failed", r);
        tls_close();
        return TLS_ERR_HANDSHAKE;
    }
    mbedtls_ssl_conf_authmode(&s_conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_cb(&s_conf, ca_lookup, NULL);
    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);
    if ((r = mbedtls_ssl_setup(&s_ssl, &s_conf)) != 0 ||
        (r = mbedtls_ssl_set_hostname(&s_ssl, host)) != 0) {
        set_err("TLS out of memory", r);
        tls_close();
        return TLS_ERR_HANDSHAKE;
    }
    mbedtls_ssl_set_bio(&s_ssl, NULL, bio_send, bio_recv, NULL);

    TickType_t start = xTaskGetTickCount();
    while ((r = mbedtls_ssl_handshake(&s_ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (xTaskGetTickCount() - start < pdMS_TO_TICKS(15000)) continue;
        }
        uint32_t flags = mbedtls_ssl_get_verify_result(&s_ssl);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED))
            set_err("TLS: unknown cert authority", r);
        else if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
            set_err("TLS: bad certificate", r);
        else if (r == MBEDTLS_ERR_SSL_ALLOC_FAILED)
            set_err("TLS: out of memory", r);
        else
            set_err("TLS handshake failed", r);
        tls_close();
        return TLS_ERR_HANDSHAKE;
    }
    printf("[tls] %s:%u %s\n", host, port, mbedtls_ssl_get_ciphersuite(&s_ssl));
    return 0;
}

int tls_write(const void *p, size_t n) {
    if (!s_ready) return -1;
    const uint8_t *b = p;
    TickType_t start = xTaskGetTickCount();
    while (n) {
        int w = mbedtls_ssl_write(&s_ssl, b, n);
        if (w > 0) { b += w; n -= (size_t)w; continue; }
        if (w != MBEDTLS_ERR_SSL_WANT_WRITE && w != MBEDTLS_ERR_SSL_WANT_READ) return -1;
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(10000)) return -1;
    }
    return 0;
}

int tls_read(void *p, size_t n) {
    if (!s_ready) return -2;
    int r = mbedtls_ssl_read(&s_ssl, p, n);
    if (r > 0) return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == ERR_NET_RESET) return 0;
    set_err("TLS read error", r);
    return -2;
}
