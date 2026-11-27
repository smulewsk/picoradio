/*
 * Last station + volume in one flash sector, written as a log of 16-byte
 * records: each save programs the next free record, and the sector is erased
 * only when all 256 are used. The newest valid record wins, so a power cut
 * while saving just keeps the previous setting.
 *
 * The sector sits just below the stations (see stations.c), outside the OTA
 * firmware slots, so updates keep it.
 */
#include <stdio.h>
#include <string.h>
#include "pico/flash.h"
#include "hardware/flash.h"
#include "settings.h"
#include "radio.h"
#include "stations.h"
#include "config.h"

#define SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - 8 * FLASH_SECTOR_SIZE)   /* 0x3F8000 on 4 MB */
#define REC_MAGIC       0x53455431u                                       /* "SET1" */
#define REC_COUNT       (FLASH_SECTOR_SIZE / sizeof(rec_t))
#define SAVE_DELAY_MS   5000

#if PICO_RP2040
#define READ_BASE XIP_BASE
#else
#define READ_BASE XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE   /* not mapped by the OTA slot window */
#endif

typedef struct {
    uint32_t magic;
    uint32_t seq;
    int16_t  station;
    uint8_t  volume;
    uint8_t  pad;
    uint32_t check;
} rec_t;
_Static_assert(sizeof(rec_t) == 16, "record size");

static const rec_t *recs(void) { return (const rec_t *)(READ_BASE + SETTINGS_OFFSET); }

static uint32_t rec_check(const rec_t *r) {
    return ~(r->magic ^ r->seq ^ (uint32_t)(uint16_t)r->station ^ ((uint32_t)r->volume << 16));
}

static bool rec_valid(const rec_t *r) { return r->magic == REC_MAGIC && r->check == rec_check(r); }

static int s_next = 0;               /* first free record */
static uint32_t s_seq;
static int s_saved_station = -1, s_saved_volume = -1;
static int s_pend_station = -1, s_pend_volume = -1;
static uint32_t s_pend_since;

/* finds the newest valid record and the first free slot */
static const rec_t *scan(void) {
    const rec_t *best = NULL;
    s_next = (int)REC_COUNT;
    for (int i = 0; i < (int)REC_COUNT; i++) {
        const rec_t *r = &recs()[i];
        if (r->magic == 0xFFFFFFFFu) { s_next = i; break; }      /* end of the log */
        if (rec_valid(r) && (!best || (int32_t)(r->seq - best->seq) > 0)) best = r;
    }
    return best;
}

void settings_load(void) {
    const rec_t *r = scan();
    if (!r) { printf("[set] no saved settings\n"); return; }
    s_seq = r->seq;
    s_saved_station = r->station;
    s_saved_volume = r->volume;
    if (r->station >= 0 && r->station < stations_count()) {
        g_radio.req_station = r->station;
        g_radio.last_station = r->station;
    }
    if (r->volume <= VOLUME_MAX) radio_set_volume(r->volume);
    printf("[set] restored station %d, volume %d\n", r->station + 1, r->volume);
}

typedef struct { uint32_t off; const uint8_t *page; } op_t;

static void do_write(void *p) {
    const op_t *op = p;
    if (!op->page) flash_range_erase(op->off, FLASH_SECTOR_SIZE);
    else flash_range_program(op->off, op->page, FLASH_PAGE_SIZE);
}

static bool flash_op(uint32_t off, const uint8_t *page) {
    op_t op = { off, page };
    return flash_safe_execute(do_write, &op, 1000) == PICO_OK;
}

static void save(int station, int volume) {
    if (s_next >= (int)REC_COUNT) {                              /* log full: start over */
        if (!flash_op(SETTINGS_OFFSET, NULL)) return;
        s_next = 0;
    }
    rec_t r = { .magic = REC_MAGIC, .seq = s_seq + 1, .station = (int16_t)station, .volume = (uint8_t)volume };
    r.check = rec_check(&r);

    /* program the page holding this record; 0xFF bytes leave the other records as they are */
    static uint8_t page[FLASH_PAGE_SIZE];
    uint32_t pos = (uint32_t)s_next * sizeof(rec_t);
    memset(page, 0xFF, sizeof page);
    memcpy(page + pos % FLASH_PAGE_SIZE, &r, sizeof r);
    if (!flash_op(SETTINGS_OFFSET + pos - pos % FLASH_PAGE_SIZE, page)) return;

    if (!rec_valid(&recs()[s_next])) {                           /* bad write: wipe and retry next time */
        flash_op(SETTINGS_OFFSET, NULL);
        s_next = 0;
        return;
    }
    s_next++;
    s_seq = r.seq;
    s_saved_station = station;
    s_saved_volume = volume;
}

void settings_poll(uint32_t now_ms) {
    int st = g_radio.last_station, vol = g_radio.volume;
    if (st == s_saved_station && vol == s_saved_volume) { s_pend_station = -1; return; }
    if (st != s_pend_station || vol != s_pend_volume) {        /* changed: wait until it settles */
        s_pend_station = st;
        s_pend_volume = vol;
        s_pend_since = now_ms;
        return;
    }
    if (now_ms - s_pend_since >= SAVE_DELAY_MS) save(st, vol);
}
