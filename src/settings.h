#pragma once
#include <stdint.h>

/* Last radio station and volume, kept in flash across power cycles. */

/* Restores the saved station and volume into g_radio. Call after stations_init(). */
void settings_load(void);

/* Call regularly (UI task): saves a change once it has stayed the same for a
 * few seconds, so turning the knob or browsing stations doesn't wear the flash. */
void settings_poll(uint32_t now_ms);
