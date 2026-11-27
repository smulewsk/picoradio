/* Thin wrapper around the Helix AAC decoder (RealNetworks RPSL/RCSL). */
#include <stdlib.h>
#include "aac_dec.h"
#if PICO_ON_DEVICE
#include "mem.h"
#endif

/* Helix only knows a few platforms; its portable C path is selected by ARDUINO */
#define ARDUINO 1
#include "aacdec.h"
#undef ARDUINO

static HAACDecoder s_dec;

/* Allocator hooks of this Helix port. On the Pico both the ~29 KB core
 * state and the ~51 KB SBR state (sbr.c's malloc is mapped to xmem_alloc in
 * CMakeLists) go to whichever heap has room. */
#if PICO_ON_DEVICE
void *helix_malloc(int size) { return xmem_alloc((size_t)size); }
void helix_free(void *ptr) { xmem_free(ptr); }
#else
void *helix_malloc(int size) { return malloc((size_t)size); }
void helix_free(void *ptr) { free(ptr); }
#endif

bool aac_open(void) {
    if (!s_dec) s_dec = AACInitDecoder();
    return s_dec != NULL;
}

void aac_close(void) {
    if (s_dec) AACFreeDecoder(s_dec);
    s_dec = NULL;
}

bool aac_is_open(void) { return s_dec != NULL; }

int aac_decode(uint8_t *buf, int len, int16_t *pcm, aac_frame_t *fi) {
    if (!s_dec || len < 7) return 0;

    int off = AACFindSyncWord(buf, len);
    if (off < 0) return -(len - 1);               /* keep the last byte: sync may straddle */
    if (off > 0) return -off;

    unsigned char *p = buf;
    int left = len;
    int err = AACDecode(s_dec, &p, &left, pcm);
    if (err == ERR_AAC_INDATA_UNDERFLOW) return 0;
    if (err) {
        int used = len - left;
        return -(used > 0 ? used : 1);           /* resync after a bad frame */
    }

    AACFrameInfo info;
    AACGetLastFrameInfo(s_dec, &info);
    fi->channels = info.nChans;
    fi->frames = info.nChans ? info.outputSamps / info.nChans : 0;
    fi->sample_rate = info.sampRateOut;
    fi->core_rate = info.sampRateCore;
    fi->sbr = info.sampRateOut != info.sampRateCore;
    return len - left;
}
