/*
 * I2S output for PCM5102 using PIO + DMA.
 *
 * A pool of PCM buffers circulates between two queues:
 *   free_q  -> decoder fills -> full_q -> DMA IRQ plays -> free_q
 * When full_q is empty the DMA plays a block of silence (underrun),
 * so the I2S clocks never stop (PCM5102 would otherwise mute/pop).
 */
#include <math.h>
#include "audio_i2s.h"
#include "config.h"
#include "queue.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "i2s.pio.h"

#define SILENCE_FRAMES 256
#define AUDIO_DMA_IRQ  DMA_IRQ_1

static PIO s_pio;          /* picked at runtime; CYW43 WiFi also uses a PIO */
static uint s_sm;
static int s_dma = -1;

static audio_buf_t s_bufs[AUDIO_NUM_BUFS];
static uint32_t s_silence[SILENCE_FRAMES];
static QueueHandle_t s_free_q, s_full_q;
static audio_buf_t *volatile s_playing;
static volatile uint32_t s_underruns;
static volatile uint16_t s_lvl_l, s_lvl_r;   /* level of the playing block */

static void __isr audio_dma_irq(void) {
    if (!dma_irqn_get_channel_status(1, s_dma)) return;
    dma_irqn_acknowledge_channel(1, s_dma);

    BaseType_t woken = pdFALSE;
    if (s_playing) {
        audio_buf_t *done = s_playing;
        xQueueSendFromISR(s_free_q, &done, &woken);
        s_playing = NULL;
    }
    audio_buf_t *next;
    if (xQueueReceiveFromISR(s_full_q, &next, &woken) == pdTRUE) {
        s_playing = next;
        s_lvl_l = next->rms_l;
        s_lvl_r = next->rms_r;
        dma_channel_transfer_from_buffer_now(s_dma, next->data, next->frames);
    } else {
        s_underruns++;
        s_lvl_l = s_lvl_r = 0;
        dma_channel_transfer_from_buffer_now(s_dma, s_silence, SILENCE_FRAMES);
    }
    portYIELD_FROM_ISR(woken);
}

void audio_set_sample_rate(uint32_t sample_rate) {
    float div = (float)clock_get_hz(clk_sys) / (float)(sample_rate * 128u);   /* 4 PIO cycles per bit, 32 bits per frame */
    pio_sm_set_clkdiv(s_pio, s_sm, div);
}

void audio_init(uint32_t sample_rate) {
    s_free_q = xQueueCreate(AUDIO_NUM_BUFS, sizeof(audio_buf_t *));
    s_full_q = xQueueCreate(AUDIO_NUM_BUFS, sizeof(audio_buf_t *));
    for (int i = 0; i < AUDIO_NUM_BUFS; i++) {
        audio_buf_t *b = &s_bufs[i];
        xQueueSend(s_free_q, &b, 0);
    }

    uint offset;
    bool ok = pio_claim_free_sm_and_add_program(&audio_i2s_program, &s_pio, &s_sm, &offset);
    configASSERT(ok);
    (void)ok;
    pio_gpio_init(s_pio, PIN_I2S_DATA);
    pio_gpio_init(s_pio, PIN_I2S_BCLK);
    pio_gpio_init(s_pio, PIN_I2S_LRCK);
    audio_i2s_program_init(s_pio, s_sm, offset, PIN_I2S_DATA, PIN_I2S_BCLK, PIN_I2S_LRCK);
    audio_set_sample_rate(sample_rate);

    s_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));
    dma_channel_configure(s_dma, &c, &s_pio->txf[s_sm], s_silence, SILENCE_FRAMES, false);

    dma_irqn_set_channel_enabled(1, s_dma, true);
    irq_add_shared_handler(AUDIO_DMA_IRQ, audio_dma_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(AUDIO_DMA_IRQ, true);

    pio_sm_set_enabled(s_pio, s_sm, true);
    dma_channel_start(s_dma);
}

audio_buf_t *audio_get_free(TickType_t timeout) {
    audio_buf_t *b = NULL;
    if (xQueueReceive(s_free_q, &b, timeout) != pdTRUE) return NULL;
    return b;
}

void audio_submit(audio_buf_t *buf) {
    xQueueSend(s_full_q, &buf, portMAX_DELAY);
}

void audio_release(audio_buf_t *buf) {
    xQueueSend(s_free_q, &buf, portMAX_DELAY);
}

uint32_t audio_underruns(void) { return s_underruns; }

void audio_levels(uint16_t *l, uint16_t *r) {
    *l = s_lvl_l;
    *r = s_lvl_r;
}

void audio_measure(audio_buf_t *b) {
    float sl = 0, sr = 0;
    for (uint32_t i = 0; i < b->frames; i++) {
        float l = (float)(int16_t)(b->data[i] & 0xFFFF);
        float r = (float)(int16_t)(b->data[i] >> 16);
        sl += l * l;
        sr += r * r;
    }
    float n = b->frames ? (float)b->frames : 1.0f;
    b->rms_l = (uint16_t)sqrtf(sl / n);
    b->rms_r = (uint16_t)sqrtf(sr / n);
}
