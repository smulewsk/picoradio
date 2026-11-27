/*
 * INMP441 I2S microphone: PIO clocks the mic at 32 kHz, DMA writes the raw
 * 32-bit words into a ring buffer, and mic_read() turns them into 16 kHz
 * int16 samples (DC removal, gain, 2:1 low-pass decimation).
 */
#include <string.h>
#include "mic.h"
#include "config.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "mic_i2s.pio.h"

#define CAPTURE_RATE   (MIC_RATE * 2)
#define RING_WORDS     4096                       /* 128 ms at 32 kHz */
#define RING_BITS      14                         /* 4096 * 4 bytes = 2^14 */

static uint32_t s_ring[RING_WORDS] __attribute__((aligned(RING_WORDS * 4)));
static PIO  s_pio;
static uint s_sm, s_offset;
static int  s_dma = -1;
static bool s_running;
static uint32_t s_rd;                             /* read index in words */

/* filter state */
static int32_t s_dc_x, s_dc_y;                    /* high-pass */
static int32_t s_h[3];                            /* decimator history */
static bool    s_odd;

void mic_init(void) {
    uint offset;
    bool ok = pio_claim_free_sm_and_add_program(&mic_i2s_program, &s_pio, &s_sm, &offset);
    (void)ok;
    s_offset = offset;
    pio_gpio_init(s_pio, PIN_MIC_SCK);
    pio_gpio_init(s_pio, PIN_MIC_WS);
    pio_gpio_init(s_pio, PIN_MIC_SD);
    mic_i2s_program_init(s_pio, s_sm, offset, PIN_MIC_SD, PIN_MIC_SCK);
    pio_sm_set_clkdiv(s_pio, s_sm, (float)clock_get_hz(clk_sys) / (float)(CAPTURE_RATE * 128));

    s_dma = dma_claim_unused_channel(true);
}

static uint32_t write_index(void) {
    uint32_t wa = dma_hw->ch[s_dma].write_addr;
    return ((wa - (uint32_t)s_ring) / 4) & (RING_WORDS - 1);
}

void mic_start(void) {
    if (s_running) return;
    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_BITS);   /* wrap the write address */
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));
    dma_channel_configure(s_dma, &c, s_ring, &s_pio->rxf[s_sm],
                          dma_encode_endless_transfer_count(), true);

    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_offset));
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_rd = write_index();
    s_running = true;
}

void mic_stop(void) {
    if (!s_running) return;
    pio_sm_set_enabled(s_pio, s_sm, false);
    dma_channel_abort(s_dma);
    s_running = false;
}

void mic_flush(void) {
    if (s_running) s_rd = write_index();
}

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

size_t mic_read(int16_t *out, size_t max) {
    if (!s_running) return 0;
    uint32_t wr = write_index();
    size_t n = 0;
    while (s_rd != wr && n < max) {
        uint32_t w = s_ring[s_rd];
        s_rd = (s_rd + 1) & (RING_WORDS - 1);

        /* drop the I2S delay bit, keep 24 signed data bits */
        int32_t x = (int32_t)(w << 1) >> 8;

        /* DC blocker: y = x - x1 + 0.996 * y1 */
        int32_t y = x - s_dc_x + ((s_dc_y * 255) >> 8);
        s_dc_x = x;
        s_dc_y = y;

        /* 2:1 decimation with a [1 3 3 1]/8 low-pass */
        int32_t lp = (s_h[0] + 3 * s_h[1] + 3 * s_h[2] + y) >> 3;
        s_h[0] = s_h[1];
        s_h[1] = s_h[2];
        s_h[2] = y;
        s_odd = !s_odd;
        if (s_odd) continue;

        /* 24-bit -> 16-bit with MIC_GAIN */
        out[n++] = clamp16((lp * MIC_GAIN) >> 8);
    }
    return n;
}
