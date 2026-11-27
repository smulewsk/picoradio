/*
 * Setup access point helpers: a tiny DHCP server (hands out 192.168.4.16..)
 * and a "captive" DNS server that answers every name with 192.168.4.1, so a
 * phone joining the setup network opens the radio's page by itself.
 * Both run on lwIP raw UDP callbacks (tcpip thread).
 */
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "ap.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#define AP_IP_A 192
#define AP_IP_B 168
#define AP_IP_C 4
#define AP_IP_D 1

/* ---------- DHCP ---------- */

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u
#define LEASES           8
#define LEASE_BASE       16          /* 192.168.4.16 .. .23 */
#define LEASE_SECS       (24 * 3600)

enum { DHCPDISCOVER = 1, DHCPOFFER = 2, DHCPREQUEST = 3, DHCPDECLINE = 4,
       DHCPACK = 5, DHCPNAK = 6, DHCPRELEASE = 7, DHCPINFORM = 8 };

typedef struct {
    uint8_t op, htype, hlen, hops;
    uint8_t xid[4], secs[2], flags[2];
    uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint8_t magic[4];
    uint8_t options[312];
} dhcp_msg_t;

static uint8_t s_lease_mac[LEASES][6];
static struct udp_pcb *s_dhcp, *s_dns;

/* option `code` in the options area, or NULL */
static const uint8_t *opt_find(const uint8_t *o, size_t n, uint8_t code) {
    size_t i = 0;
    while (i < n && o[i] != 255) {
        if (o[i] == 0) { i++; continue; }
        if (i + 1 >= n || i + 2 + o[i + 1] > n) break;
        if (o[i] == code) return &o[i];
        i += 2 + o[i + 1];
    }
    return NULL;
}

static int lease_for(const uint8_t *mac) {
    int free_slot = -1;
    for (int i = 0; i < LEASES; i++) {
        if (!memcmp(s_lease_mac[i], mac, 6)) return i;
        static const uint8_t zero[6];
        if (free_slot < 0 && !memcmp(s_lease_mac[i], zero, 6)) free_slot = i;
    }
    if (free_slot < 0) return -1;
    memcpy(s_lease_mac[free_slot], mac, 6);
    return free_slot;
}

static uint8_t *opt_put(uint8_t *o, uint8_t code, uint8_t len, const void *data) {
    *o++ = code;
    *o++ = len;
    memcpy(o, data, len);
    return o + len;
}

static void dhcp_reply(struct netif *nif, const dhcp_msg_t *req, uint8_t type, int lease) {
    static dhcp_msg_t m;
    memset(&m, 0, sizeof m);
    m.op = 2;                                   /* BOOTREPLY */
    m.htype = 1;
    m.hlen = 6;
    memcpy(m.xid, req->xid, 4);
    memcpy(m.flags, req->flags, 2);
    memcpy(m.chaddr, req->chaddr, 16);
    const uint8_t me[4] = { AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D };
    if (lease >= 0) {
        m.yiaddr[0] = AP_IP_A; m.yiaddr[1] = AP_IP_B; m.yiaddr[2] = AP_IP_C;
        m.yiaddr[3] = (uint8_t)(LEASE_BASE + lease);
    }
    memcpy(m.siaddr, me, 4);
    m.magic[0] = 0x63; m.magic[1] = 0x82; m.magic[2] = 0x53; m.magic[3] = 0x63;

    uint8_t *o = m.options;
    o = opt_put(o, 53, 1, &type);
    o = opt_put(o, 54, 4, me);                  /* server id */
    if (type != DHCPNAK) {
        const uint8_t mask[4] = { 255, 255, 255, 0 };
        const uint8_t t[4] = { (uint8_t)(LEASE_SECS >> 24), (uint8_t)(LEASE_SECS >> 16),
                               (uint8_t)(LEASE_SECS >> 8), (uint8_t)LEASE_SECS };
        o = opt_put(o, 51, 4, t);               /* lease time */
        o = opt_put(o, 1, 4, mask);
        o = opt_put(o, 3, 4, me);               /* router */
        o = opt_put(o, 6, 4, me);               /* DNS: us (captive portal) */
    }
    *o++ = 255;
    size_t len = (size_t)(o - (uint8_t *)&m);
    if (len < 300) len = 300;                   /* some clients want a BOOTP-sized reply */

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, &m, len);
    ip_addr_t bc;
    IP4_ADDR(ip_2_ip4(&bc), 255, 255, 255, 255);
    udp_sendto_if(s_dhcp, p, &bc, DHCP_CLIENT_PORT, nif);
    pbuf_free(p);
}

static void dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    static dhcp_msg_t req;
    memset(&req, 0, sizeof req);
    u16_t n = pbuf_copy_partial(p, &req, sizeof req, 0);
    pbuf_free(p);
    if (n < 240 || req.op != 1 || req.hlen != 6) return;
    uint32_t magic = ((uint32_t)req.magic[0] << 24) | ((uint32_t)req.magic[1] << 16) |
                     ((uint32_t)req.magic[2] << 8) | req.magic[3];
    if (magic != DHCP_MAGIC) return;
    size_t on = n - 240;
    const uint8_t *t = opt_find(req.options, on, 53);
    if (!t || t[1] < 1) return;
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_AP];

    switch (t[2]) {
    case DHCPDISCOVER: {
        int l = lease_for(req.chaddr);
        if (l >= 0) dhcp_reply(nif, &req, DHCPOFFER, l);
        break;
    }
    case DHCPREQUEST: {
        int l = lease_for(req.chaddr);
        /* the address asked for: option 50, or ciaddr when renewing */
        const uint8_t *ra = opt_find(req.options, on, 50);
        const uint8_t *want = ra && ra[1] == 4 ? &ra[2] : req.ciaddr;
        bool ok = l >= 0 && want[0] == AP_IP_A && want[1] == AP_IP_B && want[2] == AP_IP_C &&
                  want[3] == LEASE_BASE + l;
        if (ok) {
            dhcp_reply(nif, &req, DHCPACK, l);
            printf("[ap] client %02x:%02x:%02x:%02x:%02x:%02x got 192.168.4.%d\n", req.chaddr[0],
                   req.chaddr[1], req.chaddr[2], req.chaddr[3], req.chaddr[4], req.chaddr[5], LEASE_BASE + l);
        } else {
            dhcp_reply(nif, &req, DHCPNAK, -1);
        }
        break;
    }
    case DHCPRELEASE:
    case DHCPDECLINE: {
        for (int i = 0; i < LEASES; i++)
            if (!memcmp(s_lease_mac[i], req.chaddr, 6)) memset(s_lease_mac[i], 0, 6);
        break;
    }
    default: break;
    }
}

/* ---------- captive DNS: every A query -> 192.168.4.1 ---------- */

static void dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    static uint8_t q[512];
    u16_t n = pbuf_copy_partial(p, q, sizeof q - 16, 0);
    pbuf_free(p);
    if (n < 12) return;
    if (q[2] & 0x80) return;                         /* a response, not a query */
    if (((q[4] << 8) | q[5]) < 1) return;            /* no question */

    /* skip the first question's name */
    size_t i = 12;
    while (i < n && q[i]) {
        if (q[i] & 0xC0) { i += 1; break; }          /* compressed (shouldn't happen) */
        i += q[i] + 1u;
    }
    i++;
    if (i + 4 > n) return;
    uint16_t qtype = (uint16_t)((q[i] << 8) | q[i + 1]);
    size_t end = i + 4;                              /* end of question */

    q[2] = 0x84 | (q[2] & 0x01);                     /* response, authoritative, keep RD */
    q[3] = 0x80;                                     /* RA, no error */
    q[4] = 0; q[5] = 1;                              /* one question */
    q[8] = q[9] = q[10] = q[11] = 0;                 /* no authority / additional */
    size_t len = end;
    if (qtype == 1 || qtype == 255) {                /* A or ANY */
        static const uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
                                         AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D };
        memcpy(q + len, ans, sizeof ans);
        len += sizeof ans;
        q[6] = 0; q[7] = 1;
    } else {
        q[6] = q[7] = 0;                             /* e.g. AAAA: no answer */
    }
    struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!r) return;
    memcpy(r->payload, q, len);
    udp_sendto(pcb, r, addr, port);
    pbuf_free(r);
}

/* ---------- start / stop ---------- */

void ap_servers_start(void) {
    cyw43_arch_lwip_begin();
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_AP];
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    gw = ip;
    netif_set_addr(nif, &ip, &mask, &gw);
    memset(s_lease_mac, 0, sizeof s_lease_mac);

    if (!s_dhcp) {
        s_dhcp = udp_new();
        if (s_dhcp) {
            ip_set_option(s_dhcp, SOF_BROADCAST);
            udp_bind(s_dhcp, IP_ANY_TYPE, DHCP_SERVER_PORT);
            udp_recv(s_dhcp, dhcp_recv, NULL);
        }
    }
    if (!s_dns) {
        s_dns = udp_new();
        if (s_dns) {
            udp_bind(s_dns, IP_ANY_TYPE, 53);
            udp_recv(s_dns, dns_recv, NULL);
        }
    }
    cyw43_arch_lwip_end();
    if (!s_dhcp || !s_dns) printf("[ap] cannot start DHCP / DNS server\n");
}

int ap_client_count(void) {
    static uint8_t macs[6 * 8];
    int num = 8;                                     /* in: room in macs, out: connected */
    memset(macs, 0, sizeof macs);
    cyw43_wifi_ap_get_stas(&cyw43_state, &num, macs);   /* returns void in this SDK */
    /* count real MACs too, in case the call failed and left num as it was */
    int real = 0;
    for (int i = 0; i < num && i < 8; i++) {
        const uint8_t *m = &macs[6 * i];
        if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) real++;
    }
    return real;
}
