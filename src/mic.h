#pragma once
#include <stdint.h>
#include <stddef.h>

/* INMP441 microphone, delivered as 16 kHz mono int16 (what Gemini expects). */
#define MIC_RATE 16000

void   mic_init(void);              /* once */
void   mic_start(void);             /* start clocks + DMA */
void   mic_stop(void);
void   mic_flush(void);             /* drop everything captured so far */
/* Reads up to max samples captured since the last call. Never blocks. */
size_t mic_read(int16_t *out, size_t max);
