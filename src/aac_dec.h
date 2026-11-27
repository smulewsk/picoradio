#pragma once
#include <stdbool.h>
#include <stdint.h>

/* AAC-LC / HE-AAC (AAC+) decoder for ADTS radio streams (Helix AAC). */

#define AAC_MAX_FRAMES  2048     /* per channel; HE-AAC doubles the 1024 of AAC-LC */

typedef struct {
    int  frames;        /* PCM frames (per channel) produced */
    int  channels;      /* 1 or 2 */
    int  sample_rate;   /* output rate */
    int  core_rate;     /* AAC core rate (half the output rate with SBR) */
    bool sbr;           /* HE-AAC (spectral band replication) */
} aac_frame_t;

/* Allocates ~80 KB (spread over both heaps by xmem_alloc); close when not playing AAC. */
bool aac_open(void);
void aac_close(void);
bool aac_is_open(void);

/* Decodes one ADTS frame from buf (interleaved int16 output, up to
 * AAC_MAX_FRAMES * 2 samples).
 *   > 0  bytes consumed, a frame was decoded into pcm / fi
 *   = 0  need more data
 *   < 0  skip this many bytes (garbage / bad frame) */
int aac_decode(uint8_t *buf, int len, int16_t *pcm, aac_frame_t *fi);
