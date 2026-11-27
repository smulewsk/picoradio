/*
 * Station store: two 8 KB flash slots written alternately (A/B), each with a
 * sequence number and CRC, so a power cut while saving keeps the old list.
 * Records are read straight from flash (XIP): no RAM copy of the list.
 *
 * Flash map (4 MB Pico 2 W):  ... firmware slots A/B (ota.c) ... | stations A |
 *                             stations B | BTstack pairings (8 KB) | reserved sector (4 KB)
 */
#include <string.h>
#include <stdint.h>
#include "stations.h"
#include "config.h"

#ifndef STATIONS_HOST_TEST
#include "FreeRTOS.h"
#include "semphr.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "radio.h"
#include "mem.h"
#endif

#define SLOT_SIZE    (8 * 1024)
#define MAGIC        0x53544E31u       /* "STN1" */

typedef struct {
    char name[ST_NAME_MAX];
    char url[ST_URL_MAX];
} rec_t;                               /* 256 bytes */

typedef struct {
    uint32_t magic, seq, count, crc;
    rec_t    rec[ST_MAX];
} store_t;

_Static_assert(sizeof(rec_t) == 256, "record size");
_Static_assert(sizeof(store_t) <= SLOT_SIZE, "slot size");

/* ---------------- flash access (replaced in the host test) ---------------- */

#ifdef STATIONS_HOST_TEST
extern uint8_t g_test_flash[2 * SLOT_SIZE];
static const store_t *slot_ptr(int s) { return (const store_t *)(g_test_flash + s * SLOT_SIZE); }
static bool slot_write(int s, const void *data) { memcpy(g_test_flash + s * SLOT_SIZE, data, SLOT_SIZE); return true; }
static void slot_erase(int s) { memset(g_test_flash + s * SLOT_SIZE, 0xFF, SLOT_SIZE); }
static void *buf_alloc(void) { static uint8_t b[SLOT_SIZE]; return b; }
static void buf_free(void *p) { (void)p; }
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
extern int g_req, g_last; extern int g_retunes;
static int  playing(void) { return g_req; }
static void set_playing(int i) { g_req = i; }
static void retune(int i) { g_req = i; g_retunes++; }
static int  last(void) { return g_last; }
static void set_last(int i) { g_last = i; }
#else
#define STATIONS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE - 2 * FLASH_SECTOR_SIZE - 2 * SLOT_SIZE)
static SemaphoreHandle_t s_lock;
#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* Read through the untranslated XIP window: when the firmware runs from an
 * OTA slot (see ota.c), XIP_BASE maps only that slot, not the end of flash. */
#if PICO_RP2040
#define STATIONS_READ_BASE XIP_BASE
#else
#define STATIONS_READ_BASE XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#endif
static const store_t *slot_ptr(int s) {
    return (const store_t *)(STATIONS_READ_BASE + STATIONS_OFFSET + (uint32_t)s * SLOT_SIZE);
}
typedef struct { uint32_t off; const void *data; } flash_op_t;
static void do_write(void *p) {
    flash_op_t *op = p;
    flash_range_erase(op->off, SLOT_SIZE);
    if (op->data) flash_range_program(op->off, op->data, SLOT_SIZE);
}
static bool slot_write(int s, const void *data) {
    flash_op_t op = { STATIONS_OFFSET + (uint32_t)s * SLOT_SIZE, data };
    return flash_safe_execute(do_write, &op, 1000) == PICO_OK;
}
static void slot_erase(int s) {
    flash_op_t op = { STATIONS_OFFSET + (uint32_t)s * SLOT_SIZE, NULL };
    flash_safe_execute(do_write, &op, 1000);
}
static void *buf_alloc(void) { return xmem_alloc(SLOT_SIZE); }
static void buf_free(void *p) { xmem_free(p); }
static int  playing(void) { return g_radio.req_station; }
static void set_playing(int i) { g_radio.req_station = i; }
static void retune(int i) { radio_tune(i); }
static int  last(void) { return g_radio.last_station; }
static void set_last(int i) { g_radio.last_station = i; }
#endif

/* ---------------- slots ---------------- */

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t store_crc(const store_t *st) {
    return crc32((const uint8_t *)st->rec, sizeof(rec_t) * st->count);
}

static bool slot_valid(const store_t *st) {
    return st->magic == MAGIC && st->count >= 1 && st->count <= ST_MAX && st->crc == store_crc(st);
}

static int s_active = -1;             /* -1 = config.h defaults */

static void pick_active(void) {
    const store_t *a = slot_ptr(0), *b = slot_ptr(1);
    bool va = slot_valid(a), vb = slot_valid(b);
    if (va && vb) s_active = (int32_t)(b->seq - a->seq) > 0 ? 1 : 0;
    else s_active = va ? 0 : vb ? 1 : -1;
}

static int count_nolock(void) {
    return s_active < 0 ? NUM_STATIONS : (int)slot_ptr(s_active)->count;
}

/* ---------------- text helpers ---------------- */

/* copy, trimmed, cut at a UTF-8 character boundary */
static void copy_field(char *dst, size_t dn, const char *src) {
    while (*src == ' ' || *src == '\t') src++;
    size_t n = strlen(src);
    while (n && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\r' || src[n - 1] == '\n')) n--;
    if (n >= dn) {
        n = dn - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   /* don't split a character */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool valid_url(const char *u) {
    if (!u || strlen(u) >= ST_URL_MAX) return false;          /* never store a cut URL */
    return (!strncmp(u, "http://", 7) && u[7]) || (!strncmp(u, "https://", 8) && u[8]);
}

/* ---------------- read API ---------------- */

void stations_init(void) {
#ifndef STATIONS_HOST_TEST
    s_lock = xSemaphoreCreateMutex();
#endif
    pick_active();
}

int stations_count(void) {
    LOCK();
    int n = count_nolock();
    UNLOCK();
    return n;
}

bool stations_is_default(void) { return s_active < 0; }

static char s_tmp_name[ST_NAME_MAX], s_tmp_url[ST_URL_MAX];

void stations_set_temp(const char *name, const char *url) {
    LOCK();
    copy_field(s_tmp_name, sizeof s_tmp_name, name);
    copy_field(s_tmp_url, sizeof s_tmp_url, url);
    UNLOCK();
}

/* lower-case ASCII view of a name, Polish letters folded (for matching) */
static void fold(char *dst, size_t dn, const char *src) {
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)src; *s && n + 1 < dn; s++) {
        unsigned char c = *s;
        if (c == 0xC4 || c == 0xC5 || c == 0xC3) {           /* 2-byte Latin letters: drop the accent */
            unsigned char d = s[1];
            if (!d) break;
            s++;
            char base = 0;
            if (c == 0xC4) base = d == 0x85 || d == 0x84 ? 'a' : d == 0x87 || d == 0x86 ? 'c' : d == 0x99 || d == 0x98 ? 'e' : 0;
            else if (c == 0xC5) base = d == 0x82 || d == 0x81 ? 'l' : d == 0x84 || d == 0x83 ? 'n' : d == 0x9B || d == 0x9A ? 's' :
                                      d == 0xBA || d == 0xB9 || d == 0xBC || d == 0xBB ? 'z' : 0;
            else base = d == 0xB3 || d == 0x93 ? 'o' : 0;
            if (base) dst[n++] = base;
            continue;
        }
        dst[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    dst[n] = 0;
}

int stations_find(const char *name) {
    char want[ST_NAME_MAX], have[ST_NAME_MAX], raw[ST_NAME_MAX];
    fold(want, sizeof want, name);
    if (!want[0]) return -1;
    int count = stations_count();
    for (int i = 0; i < count; i++) {                     /* exact name first */
        if (!stations_get(i, raw, sizeof raw, NULL, 0)) continue;
        fold(have, sizeof have, raw);
        if (!strcmp(have, want)) return i;
    }
    for (int i = 0; i < count; i++) {                     /* then "contains" */
        if (!stations_get(i, raw, sizeof raw, NULL, 0)) continue;
        fold(have, sizeof have, raw);
        if (strstr(have, want)) return i;
    }
    return -1;
}

bool stations_get(int i, char *name, size_t nn, char *url, size_t un) {
    if (i == ST_TEMP) {                                   /* the AI's directory find */
        LOCK();
        bool ok = s_tmp_url[0] != 0;
        if (ok) {
            if (name) copy_field(name, nn, s_tmp_name);
            if (url) copy_field(url, un, s_tmp_url);
        }
        UNLOCK();
        return ok;
    }
    LOCK();
    bool ok = i >= 0 && i < count_nolock();
    if (ok) {
        const char *n, *u;
        if (s_active < 0) { n = STATIONS[i].name; u = STATIONS[i].url; }
        else { n = slot_ptr(s_active)->rec[i].name; u = slot_ptr(s_active)->rec[i].url; }
        if (name) copy_field(name, nn, n);
        if (url) copy_field(url, un, u);
    }
    UNLOCK();
    return ok;
}

/* ---------------- editing ---------------- */

/* current list -> RAM image */
static store_t *load_image(void) {
    store_t *st = buf_alloc();
    if (!st) return NULL;
    memset(st, 0xFF, SLOT_SIZE);
    memset(st, 0, sizeof *st);
    if (s_active < 0) {
        st->count = NUM_STATIONS > ST_MAX ? ST_MAX : NUM_STATIONS;
        for (uint32_t i = 0; i < st->count; i++) {
            copy_field(st->rec[i].name, ST_NAME_MAX, STATIONS[i].name);
            copy_field(st->rec[i].url, ST_URL_MAX, STATIONS[i].url);
        }
        st->seq = 0;
    } else {
        memcpy(st, slot_ptr(s_active), sizeof *st);
    }
    return st;
}

/* RAM image -> the other slot */
static bool save_image(store_t *st) {
    st->magic = MAGIC;
    st->seq = (s_active < 0 ? 0 : slot_ptr(s_active)->seq) + 1;
    st->crc = store_crc(st);
    int target = s_active == 0 ? 1 : 0;
    bool ok = slot_write(target, st) && slot_valid(slot_ptr(target));
    if (ok) s_active = target;
    return ok;
}

bool stations_add(const char *name, const char *url) {
    if (!name || !*name || !valid_url(url)) return false;
    LOCK();
    store_t *st = load_image();
    bool ok = st && st->count < ST_MAX;
    if (ok) {
        copy_field(st->rec[st->count].name, ST_NAME_MAX, name);
        copy_field(st->rec[st->count].url, ST_URL_MAX, url);
        st->count++;
        ok = save_image(st);
    }
    if (st) buf_free(st);
    UNLOCK();
    return ok;
}

bool stations_update(int i, const char *name, const char *url) {
    if (!name || !*name || !valid_url(url)) return false;
    LOCK();
    store_t *st = load_image();
    bool ok = st && i >= 0 && i < (int)st->count;
    bool url_changed = false;
    if (ok) {
        char u[ST_URL_MAX];
        copy_field(u, sizeof u, url);
        url_changed = strcmp(u, st->rec[i].url) != 0;
        copy_field(st->rec[i].name, ST_NAME_MAX, name);
        strcpy(st->rec[i].url, u);
        ok = save_image(st);
    }
    if (st) buf_free(st);
    UNLOCK();
    if (ok && url_changed && playing() == i) retune(i);   /* reconnect to the new URL */
    return ok;
}

bool stations_delete(int i) {
    LOCK();
    store_t *st = load_image();
    bool ok = st && i >= 0 && i < (int)st->count && st->count > 1;   /* keep at least one */
    int remaining = 0;
    if (ok) {
        memmove(&st->rec[i], &st->rec[i + 1], sizeof(rec_t) * (st->count - (uint32_t)i - 1));
        st->count--;
        remaining = (int)st->count;
        ok = save_image(st);
    }
    if (st) buf_free(st);
    UNLOCK();
    if (!ok) return false;
    if (last() > i) set_last(last() - 1);
    else if (last() == i) set_last(i < remaining ? i : remaining - 1);
    int p = playing();
    if (p >= 0 && p < ST_MAX) {                      /* playing a radio station */
        if (p > i) set_playing(p - 1);               /* same stream, new index */
        else if (p == i) retune(i < remaining ? i : remaining - 1);
    }
    return true;
}

bool stations_move(int i, int delta) {
    int j = i + delta;
    LOCK();
    store_t *st = load_image();
    bool ok = st && i >= 0 && i < (int)st->count && j >= 0 && j < (int)st->count;
    if (ok) {
        rec_t t = st->rec[i];
        st->rec[i] = st->rec[j];
        st->rec[j] = t;
        ok = save_image(st);
    }
    if (st) buf_free(st);
    UNLOCK();
    if (!ok) return false;
    if (playing() == i) set_playing(j); else if (playing() == j) set_playing(i);
    if (last() == i) set_last(j); else if (last() == j) set_last(i);
    return true;
}

bool stations_reset(void) {
    LOCK();
    slot_erase(0);
    slot_erase(1);
    s_active = -1;
    UNLOCK();
    set_last(0);
    if (playing() >= 0 && playing() < ST_MAX) retune(0);
    return true;
}

int stations_import(const char *lines, bool replace) {
    LOCK();
    store_t *st = load_image();
    if (!st) { UNLOCK(); return -1; }
    if (replace) st->count = 0;
    int added = 0;
    const char *p = lines;
    while (*p && st->count < ST_MAX) {
        const char *eol = p + strcspn(p, "\n");
        const char *tab = memchr(p, '\t', (size_t)(eol - p));
        if (tab) {
            char name[ST_NAME_MAX * 2], url[ST_URL_MAX * 2];
            size_t nl = (size_t)(tab - p), ul = (size_t)(eol - tab - 1);
            if (nl < sizeof name && ul < sizeof url) {
                memcpy(name, p, nl); name[nl] = 0;
                memcpy(url, tab + 1, ul); url[ul] = 0;
                rec_t *r = &st->rec[st->count];
                char u[ST_URL_MAX * 2];
                copy_field(u, sizeof u, url);
                if (valid_url(u)) {
                    strcpy(r->url, u);
                    copy_field(r->name, ST_NAME_MAX, name[0] ? name : url);
                    if (r->name[0]) { st->count++; added++; }
                }
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    bool ok = st->count >= 1 && save_image(st);
    buf_free(st);
    UNLOCK();
    if (!ok) return -1;
    if (replace) {
        set_last(0);
        if (playing() >= 0 && playing() < ST_MAX) retune(0);
    }
    return added;
}
