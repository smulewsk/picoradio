/*
 * Firmware update over WiFi (OTA) - see ota.h.
 *
 * Flash map with the partition table from pt.json (4 MB Pico 2 W):
 *
 *   0x000000  partition table (8 KB)
 *   0x002000  slot A  (1984 KB)
 *   0x1F2000  slot B  (1984 KB)
 *   0x3E2000  unpartitioned: config A/B (0x3F6000), station+volume log (0x3F8000),
 *             stations A/B, BTstack pairings, reserved sector
 *
 * The running slot is mapped at XIP_BASE by the boot ROM (address
 * translation), so flash outside it is read through the untranslated
 * XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE window, and written with
 * flash_range_*() which always take physical offsets.
 *
 * Writing order: the target slot's first sector is erased first and written
 * last, so a slot is never bootable with half an image in it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"   /* REBOOT2_FLAG_* */
#include "FreeRTOS.h"
#include "task.h"

#include "ota.h"
#include "mem.h"

#define SECTOR    FLASH_SECTOR_SIZE
#define WORKAREA  4096                 /* boot ROM work area (needs 3264 bytes) / explicit buy (4 KB) */
#define FLASH_RD(off) ((const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + (off)))

/* UF2 (as written by picotool for the build) */
#define UF2_MAGIC0         0x0A324655u
#define UF2_MAGIC1         0x9E5D5157u
#define UF2_MAGIC_END      0x0AB16F30u
#define UF2_NOT_MAIN_FLASH 0x00000001u
#define UF2_FAMILY_PRESENT 0x00002000u
#define FAMILY_ABSOLUTE    0xe48bff57u   /* RP2350-E10 workaround block, skipped */
#define FAMILY_RP2350_ARM_S 0xe48bff59u

typedef struct {
    uint32_t magic0, magic1, flags, addr, size, block_no, num_blocks, family;
    uint8_t  data[476];
    uint32_t magic_end;
} uf2_block_t;
_Static_assert(sizeof(uf2_block_t) == 512, "UF2 block");

volatile int g_ota_pct = -1;

/* ---------------- partitions ---------------- */

static int      s_cur = -1;            /* running partition, -1 = no partition table */
static int      s_tgt = -1;            /* the other slot of the A/B pair */
static uint32_t s_tgt_start, s_tgt_size;

static bool partition_range(int p, uint32_t *start, uint32_t *size) {
    uint32_t buf[3];
    int rc = rom_get_partition_table_info(buf, 3,
        PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | ((uint32_t)p << 24));
    if (rc != 3 || buf[0] != (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION))
        return false;
    uint32_t first = (buf[1] >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) & 0x1fffu;
    uint32_t last = (buf[1] >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB) & 0x1fffu;
    if (last < first) return false;
    *start = first * SECTOR;
    *size = (last + 1 - first) * SECTOR;
    return true;
}

void ota_init(void) {
    boot_info_t bi;
    if (!rom_get_boot_info(&bi) || bi.partition < 0) {
        printf("[ota] no partition table: OTA off (see README, flash build/pt.uf2 once)\n");
        return;
    }
    s_cur = bi.partition;

    uint8_t *work = xmem_alloc(WORKAREA);
    if (!work) return;
    /* loaded by the boot ROM on a normal boot, not after a watchdog reboot */
    int rc = rom_load_partition_table(work, WORKAREA, false);
    xmem_free(work);
    if (rc < 0) { printf("[ota] cannot load the partition table (%d)\n", rc); return; }

    /* pt.json: partition 0 = A, its linked B partition = B */
    int b = rom_get_b_partition(0);
    int t = b < 0 ? -1 : s_cur == 0 ? b : s_cur == b ? 0 : -1;
    if (t < 0 || !partition_range(t, &s_tgt_start, &s_tgt_size)) {
        printf("[ota] running from partition %d, no A/B pair: OTA off\n", s_cur);
        return;
    }
    s_tgt = t;
    printf("[ota] running from slot %c, updates go to slot %c (0x%06lx, %lu KB)\n",
           ota_slot(), ota_target_slot(), (unsigned long)s_tgt_start, (unsigned long)(s_tgt_size / 1024));
}

char ota_slot(void) { return s_cur < 0 ? '-' : s_cur == 0 ? 'A' : 'B'; }
char ota_target_slot(void) { return s_tgt < 0 ? '-' : s_tgt == 0 ? 'A' : 'B'; }
uint32_t ota_slot_size(void) { return s_tgt < 0 ? 0 : s_tgt_size; }

/* ---------------- flash ---------------- */

static bool sector_blank(uint32_t off) {
    for (uint32_t i = 0; i < SECTOR; i += 4)
        if (*(const uint32_t *)(FLASH_RD(off) + i) != 0xFFFFFFFFu) return false;
    return true;
}

typedef struct { uint32_t off; const uint8_t *data; } flash_op_t;

static void do_flash(void *p) {
    const flash_op_t *op = p;
    flash_range_erase(op->off, SECTOR);
    if (op->data) flash_range_program(op->off, op->data, SECTOR);
}

/* flash_safe_execute() starts a helper task on the other core each time
 * (~2.5 KB of FreeRTOS heap), which is only freed when the idle task gets to
 * run. ~200 writes in a row would use up the heap, so wait for it to be
 * freed before each write, and retry if it still fails. */
#define LOCKOUT_HEAP 3072

static bool safe_execute(void (*fn)(void *), void *arg) {
    for (int tries = 0; tries < 100; tries++) {
        for (int w = 0; w < 50 && xPortGetFreeHeapSize() < LOCKOUT_HEAP; w++)
            vTaskDelay(pdMS_TO_TICKS(10));
        int rc = flash_safe_execute(fn, arg, 2000);
        if (rc == PICO_OK) return true;
        if (rc != PICO_ERROR_INSUFFICIENT_RESOURCES && rc != PICO_ERROR_TIMEOUT) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("[ota] flash write refused (FreeRTOS heap free: %u)\n", (unsigned)xPortGetFreeHeapSize());
    return false;
}

/* erase one sector and program it (data NULL: erase only), then verify */
static bool write_sector(uint32_t off, const uint8_t *data) {
    flash_op_t op = { off, data };
    if (!safe_execute(do_flash, &op)) return false;
    vTaskDelay(1);                          /* let the idle task free the helper task */
    return data ? memcmp(FLASH_RD(off), data, SECTOR) == 0 : sector_blank(off);
}

/* sector buffers from the C heap, to leave the FreeRTOS heap to the helper tasks */
static uint8_t *buf_alloc(void) {
    uint8_t *p = malloc(SECTOR);
    return p ? p : xmem_alloc(SECTOR);
}

/* ---------------- confirm after boot ---------------- */

void ota_confirm_boot(void) {
    boot_info_t bi;
    if (s_cur < 0 || !rom_get_boot_info(&bi)) return;
    if ((bi.boot_type & ~BOOT_TYPE_CHAINED_FLAG) != BOOT_TYPE_FLASH_UPDATE) return;

    if (bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) {
        uint8_t *buf = xmem_alloc(WORKAREA);
        if (!buf) { printf("[ota] no memory to confirm the update\n"); return; }
        int rc = rom_explicit_buy(buf, WORKAREA);
        xmem_free(buf);
        /* the boot ROM arms the watchdog for a trial boot; this firmware doesn't use it */
        watchdog_disable();
        if (rc != BOOTROM_OK) { printf("[ota] explicit buy failed (%d)\n", rc); return; }
        printf("[ota] new firmware in slot %c confirmed\n", ota_slot());
    }
    /* Both slots now hold firmware with the same version, so a normal boot
     * could pick either: wipe the old one so this slot keeps booting. */
    if (s_tgt >= 0 && !sector_blank(s_tgt_start)) {
        bool ok = write_sector(s_tgt_start, NULL);
        printf("[ota] old firmware in slot %c %s\n", ota_target_slot(), ok ? "retired" : "could not be erased");
    }
}

/* ---------------- writer ---------------- */

static struct {
    bool     active, fmt_known, uf2, have0;
    uint8_t *sec0;                     /* first sector, held back until ota_end() */
    uint8_t *sec;                      /* sector being filled */
    int32_t  cur;                      /* its offset in the slot, -1 = none */
    uint32_t bin_off;                  /* .bin: next offset */
    uint32_t blocks, num_blocks;       /* .uf2: flash blocks taken / announced */
    uint32_t top;                      /* end of the image data received */
    uint32_t blk_n;
    union { uint8_t b[512]; uf2_block_t u; } blk;
} w;

static void writer_free(void) {
    xmem_free(w.sec0);
    xmem_free(w.sec);
    memset(&w, 0, sizeof w);
    w.cur = -1;
}

static const char *flush_sector(void) {
    if (w.cur < 0) return NULL;
    bool ok = write_sector(s_tgt_start + (uint32_t)w.cur, w.sec);
    w.cur = -1;
    return ok ? NULL : "flash write failed";
}

/* copies image bytes for slot offset `off` into the sector buffers */
static const char *put(uint32_t off, const uint8_t *d, size_t n) {
    if (off >= s_tgt_size || n > s_tgt_size - off) return "firmware is too big for the update slot";
    while (n) {
        uint32_t sec = off & ~(SECTOR - 1), in = off - sec;
        size_t k = SECTOR - in < n ? SECTOR - in : n;
        if (sec == 0) {
            memcpy(w.sec0 + in, d, k);
            w.have0 = true;
        } else {
            if ((int32_t)sec != w.cur) {
                if (w.cur >= 0 && (int32_t)sec < w.cur) return "UF2 blocks are out of order";
                const char *e = flush_sector();
                if (e) return e;
                w.cur = (int32_t)sec;
                memset(w.sec, 0xFF, SECTOR);
            }
            memcpy(w.sec + in, d, k);
        }
        off += (uint32_t)k;
        d += k;
        n -= k;
    }
    if (off > w.top) w.top = off;
    return NULL;
}

static const char *uf2_block(void) {
    const uf2_block_t *u = &w.blk.u;
    if (u->magic0 != UF2_MAGIC0 || u->magic1 != UF2_MAGIC1 || u->magic_end != UF2_MAGIC_END)
        return "not a valid UF2 file";
    if (u->flags & UF2_NOT_MAIN_FLASH) return NULL;
    if (u->flags & UF2_FAMILY_PRESENT) {
        if (u->family == FAMILY_ABSOLUTE) return NULL;
        if (u->family != FAMILY_RP2350_ARM_S) return "UF2 is not for the RP2350 in Arm mode (wrong board or platform?)";
    }
    if (u->size > sizeof u->data || u->addr < XIP_BASE) return "UF2 block has a bad address";
    if (!w.blocks) w.num_blocks = u->num_blocks;
    if (u->block_no != w.blocks || u->num_blocks != w.num_blocks) return "UF2 blocks are missing or out of order";
    w.blocks++;
    return put(u->addr - XIP_BASE, u->data, u->size);
}

const char *ota_begin(size_t total) {
    if (w.active) ota_abort();
    if (s_tgt < 0) return "OTA is not set up: flash build/pt.uf2 once over USB (see README)";
    if (total < 1024 || total > 2 * s_tgt_size + 4096) return "file size doesn't look like firmware";
    memset(&w, 0, sizeof w);
    w.cur = -1;
    w.sec0 = buf_alloc();
    w.sec = buf_alloc();
    if (!w.sec0 || !w.sec) { writer_free(); return "out of memory"; }
    memset(w.sec0, 0xFF, SECTOR);
    /* make the target slot unbootable until the new image is complete */
    if (!write_sector(s_tgt_start, NULL)) { writer_free(); return "cannot erase the update slot"; }
    w.active = true;
    g_ota_pct = 0;
    printf("[ota] receiving %lu bytes for slot %c\n", (unsigned long)total, ota_target_slot());
    return NULL;
}

const char *ota_write(const void *data, size_t n) {
    if (!w.active) return "no update in progress";
    const uint8_t *p = data;
    while (n) {
        size_t k = sizeof w.blk.b - w.blk_n < n ? sizeof w.blk.b - w.blk_n : n;
        memcpy(w.blk.b + w.blk_n, p, k);
        w.blk_n += (uint32_t)k;
        p += k;
        n -= k;
        if (w.blk_n < sizeof w.blk.b) break;
        if (!w.fmt_known) {
            w.uf2 = w.blk.u.magic0 == UF2_MAGIC0 && w.blk.u.magic1 == UF2_MAGIC1;
            w.fmt_known = true;
        }
        const char *e;
        if (w.uf2) e = uf2_block();
        else { e = put(w.bin_off, w.blk.b, sizeof w.blk.b); w.bin_off += sizeof w.blk.b; }
        w.blk_n = 0;
        if (e) return e;
    }
    return NULL;
}

/* The first block links to the next one (the end block, at the end of the
 * image): it must be inside what was received, so a cut-off file is refused. */
static const char *check_block_loop(const uint8_t *s0, uint32_t blk) {
    uint32_t p = blk + 4;
    while (p + 12 <= SECTOR) {
        uint8_t type = s0[p];
        uint32_t words = type & 0x80 ? (uint32_t)(s0[p + 1] | s0[p + 2] << 8) : s0[p + 1];
        if (type == PICOBIN_BLOCK_ITEM_2BS_LAST) {
            int32_t link;
            uint32_t end;
            memcpy(&link, s0 + p + 4, 4);
            memcpy(&end, s0 + p + 8, 4);
            if (end != PICOBIN_BLOCK_MARKER_END) break;
            int64_t next = (int64_t)blk + link;
            if (next == blk) return NULL;                    /* single block */
            if (next < SECTOR || next + 4 > w.top) return "firmware file is incomplete";
            uint32_t m;
            memcpy(&m, FLASH_RD(s_tgt_start + (uint32_t)next), 4);
            return m == PICOBIN_BLOCK_MARKER_START ? NULL : "firmware file is damaged (bad block link)";
        }
        if (!words) break;
        p += words * 4;
    }
    return "firmware image header is damaged";
}

/* checks the IMAGE_DEF in the first sector and sets its TBYB flag */
static const char *prepare_image_def(uint8_t *s0) {
    uint32_t vt[2];
    memcpy(vt, s0, sizeof vt);              /* Arm vector table: initial SP, reset handler */
    for (uint32_t i = 0; i + 8 <= SECTOR; i += 4) {
        uint32_t marker;
        memcpy(&marker, s0 + i, 4);
        if (marker != PICOBIN_BLOCK_MARKER_START || s0[i + 4] != PICOBIN_BLOCK_ITEM_1BS_IMAGE_TYPE || s0[i + 5] != 1)
            continue;
        uint16_t t;
        memcpy(&t, s0 + i + 6, 2);
        if ((t & PICOBIN_IMAGE_TYPE_IMAGE_TYPE_BITS) != PICOBIN_IMAGE_TYPE_IMAGE_TYPE_AS_BITS(EXE) ||
            (t & PICOBIN_IMAGE_TYPE_EXE_CHIP_BITS) != PICOBIN_IMAGE_TYPE_EXE_CHIP_AS_BITS(RP2350) ||
            (t & PICOBIN_IMAGE_TYPE_EXE_CPU_BITS) != PICOBIN_IMAGE_TYPE_EXE_CPU_AS_BITS(ARM))
            return "not an RP2350 Arm firmware";
        if (vt[0] < 0x20000000u || vt[0] > 0x20082000u || (vt[1] & 1) == 0)
            return "firmware doesn't start with a vector table (was it built for flash?)";
        const char *e = check_block_loop(s0, i);
        if (e) return e;
        /* try before you buy: boots once, kept only if it calls explicit buy */
        t |= PICOBIN_IMAGE_TYPE_EXE_TBYB_BITS;
        memcpy(s0 + i + 6, &t, 2);
        return NULL;
    }
    return "no RP2350 image header found (UF2/BIN from an RP2350 build needed)";
}

const char *ota_end(void) {
    if (!w.active) return "no update in progress";
    const char *e = NULL;
    if (w.blk_n) {
        if (w.uf2) e = "UF2 file is truncated";
        else e = put(w.bin_off, w.blk.b, w.blk_n);              /* .bin tail */
    }
    if (!e && w.uf2 && (!w.blocks || w.blocks != w.num_blocks)) e = "UF2 file is incomplete";
    if (!e) e = flush_sector();
    if (!e && !w.have0) e = "firmware has no start (first sector missing)";
    if (!e) e = prepare_image_def(w.sec0);
    if (!e && !write_sector(s_tgt_start, w.sec0)) e = "flash write failed";
    if (e) { ota_abort(); return e; }
    writer_free();
    g_ota_pct = 100;
    printf("[ota] slot %c written\n", ota_target_slot());
    return NULL;
}

void ota_abort(void) {
    if (w.active) {
        write_sector(s_tgt_start, NULL);    /* leave no half image behind */
        printf("[ota] update aborted\n");
    }
    writer_free();
    g_ota_pct = -1;
}

void ota_reboot(void) {
    printf("[ota] rebooting into slot %c\n", ota_target_slot());
    vTaskDelay(pdMS_TO_TICKS(100));
    /* flash-update boot: the boot ROM prefers the slot starting at p0 and
     * lets its TBYB image run once */
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
               500, XIP_BASE + s_tgt_start, 0);
    for (;;) tight_loop_contents();
}
