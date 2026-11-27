#pragma once
#include <stdint.h>
#include "FreeRTOS.h"

#define AUDIO_BUF_FRAMES 1152   /* one MPEG-1 Layer III frame */
#define AUDIO_NUM_BUFS   6

typedef struct {
    uint32_t frames;
    uint16_t rms_l, rms_r;             /* level of this block, for the VU meters */
    uint32_t data[AUDIO_BUF_FRAMES];   /* (R << 16) | L */
} audio_buf_t;

/* Must be called from the task whose core should service the DMA IRQ. */
void         audio_init(uint32_t sample_rate);
void         audio_set_sample_rate(uint32_t sample_rate);
audio_buf_t *audio_get_free(TickType_t timeout);
void         audio_submit(audio_buf_t *buf);
void         audio_release(audio_buf_t *buf);   /* return unused buffer */
uint32_t     audio_underruns(void);
/* RMS level (0..32767) of the block playing right now, per channel */
void         audio_levels(uint16_t *l, uint16_t *r);
/* Measures rms_l/rms_r of b->data (call before applying volume) */
void         audio_measure(audio_buf_t *b);
