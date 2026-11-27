#pragma once
#include <stdbool.h>
#include <stdint.h>

/* All functions are safe to call from any FreeRTOS task (not from the
 * BTstack/CYW43 context itself): they are marshalled onto that context. */

void bt_init(void);                 /* once, after cyw43_arch_init() */
void bt_activate(bool on);          /* on: discoverable, connectable, audio on */
void bt_play_pause(void);
void bt_next(void);
void bt_prev(void);
int  bt_forget_all(void);           /* deletes all paired phones; returns keys left (0 = ok) */
void bt_sync_volume(int volume);    /* our 0..VOLUME_MAX -> phone */
