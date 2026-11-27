#pragma once
/*
 * Firmware update over WiFi (OTA), using the RP2350 boot ROM's A/B partitions.
 *
 * Flash holds two firmware slots, A and B (see pt.json). The radio runs from
 * one; an update is written into the other, marked "try before you buy"
 * (TBYB), and started with a flash-update reboot. The new firmware confirms
 * itself with ota_confirm_boot() once it runs. If it never does (it crashes
 * or hangs, and you power-cycle it), the boot ROM starts the old firmware.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* update progress for the display: -1 = idle, 0..100 while writing */
extern volatile int g_ota_pct;

/* Reads the partition table. Call once at startup, before the others. */
void ota_init(void);

/* Accepts the running firmware after an update (or a BOOTSEL / picotool
 * flash) and wipes the start of the other slot, so normal boots keep using
 * this one. Does nothing on a normal boot. */
void ota_confirm_boot(void);

char     ota_slot(void);          /* running slot: 'A', 'B', or '-' (no partition table) */
char     ota_target_slot(void);   /* slot an update goes to, '-' if OTA isn't possible */
uint32_t ota_slot_size(void);     /* bytes available for an update, 0 if not possible */

/* Streaming writer: begin, write the file in pieces (a .uf2 from the build
 * or a raw .bin), end. They return NULL on success or an error text. */
const char *ota_begin(size_t total);
const char *ota_write(const void *data, size_t n);
const char *ota_end(void);
void        ota_abort(void);

/* Reboots into the freshly written slot. Does not return. */
void ota_reboot(void);
