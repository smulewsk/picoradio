#pragma once
#include <stdint.h>
#include "config.h"

/* Framebuffer in panel byte order (big-endian RGB565). */
extern uint16_t g_fb[TFT_WIDTH * TFT_HEIGHT];

/* Held by the UI task while it draws a frame, and by the web server while it
 * copies one, so the web page never gets a half-drawn frame. */
void gfx_lock_init(void);     /* once, before the tasks start */
void gfx_lock(void);
void gfx_unlock(void);

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

typedef struct { int x0, y0, x1, y1; } clip_t;   /* inclusive-exclusive */

void gfx_clear(uint16_t color);
void gfx_fill(int x, int y, int w, int h, uint16_t color);
void gfx_rect(int x, int y, int w, int h, uint16_t color);
/* 5x7 font, advance = 6*scale px. Returns x after the text. */
int  gfx_text(int x, int y, const char *s, uint16_t color, int scale, const clip_t *clip);
int  gfx_text_width(const char *s, int scale);
