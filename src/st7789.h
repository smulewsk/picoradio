#pragma once
#include <stdbool.h>
#include <stdint.h>

void st7789_init(void);
void st7789_backlight(bool on);
void st7789_backlight_level(uint8_t level);   /* 0..255 */
/* Pushes a full TFT_WIDTH x TFT_HEIGHT framebuffer (byte-swapped RGB565). */
void st7789_push(const uint16_t *fb);
