/*
 * Two quadrature encoders sampled at 2 kHz (full state table, so
 * contact bounce cancels out). Push buttons are polled + debounced.
 */
#include <stdbool.h>
#include "encoder.h"
#include "config.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#define LONG_PRESS_MS 700

typedef struct {
    uint a, b, sw;
    int dir;
    volatile int32_t count;   /* written only by the timer callback */
    int32_t consumed;
    uint8_t state;
    /* button */
    bool stable, last_raw;
    uint8_t debounce;
    uint32_t press_ms;
    bool long_fired;
} enc_t;

static enc_t s_enc[2] = {
    { .a = PIN_ENC1_A, .b = PIN_ENC1_B, .sw = PIN_ENC1_SW, .dir = ENC1_REVERSE ? -1 : 1 },
    { .a = PIN_ENC2_A, .b = PIN_ENC2_B, .sw = PIN_ENC2_SW, .dir = ENC2_REVERSE ? -1 : 1 },
};

static const int8_t QDEC[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

/* Sampled from a 2 kHz timer instead of edge IRQs: a line sitting at a
 * half level (bad contact, missing pull-up) can no longer flood the CPU
 * with interrupts and freeze the radio. */
static repeating_timer_t s_timer;

static bool sample_cb(repeating_timer_t *t) {
    (void)t;
    for (int i = 0; i < 2; i++) {
        enc_t *e = &s_enc[i];
        uint8_t ab = (uint8_t)((gpio_get(e->a) << 1) | gpio_get(e->b));
        if (ab == e->state) continue;
        e->count += QDEC[((e->state << 2) | ab) & 0xF];
        e->state = ab;
    }
    return true;
}

void encoders_init(void) {
    for (int i = 0; i < 2; i++) {
        enc_t *e = &s_enc[i];
        uint pins[3] = { e->a, e->b, e->sw };
        for (int p = 0; p < 3; p++) {
            gpio_init(pins[p]);
            gpio_set_dir(pins[p], GPIO_IN);
            if (p == 2 || ENC_AB_PULLUP) gpio_pull_up(pins[p]);   /* KEY always pulled up */
            else gpio_disable_pulls(pins[p]);                     /* S1/S2: plain input */
        }
        e->state = (uint8_t)((gpio_get(e->a) << 1) | gpio_get(e->b));
    }
    add_repeating_timer_us(-500, sample_cb, NULL, &s_timer);
}

int encoder_read(int idx) {
    enc_t *e = &s_enc[idx];
    int32_t raw = e->count - e->consumed;
    int det = raw / ENC_STEPS_PER_DETENT;          /* truncates toward 0 */
    e->consumed += det * ENC_STEPS_PER_DETENT;
    return det * e->dir;
}

btn_event_t encoder_button(int idx, uint32_t now_ms) {
    enc_t *e = &s_enc[idx];
    bool raw = !gpio_get(e->sw);                     /* active low */
    btn_event_t ev = BTN_NONE;

    if (raw != e->last_raw) {
        e->last_raw = raw;
        e->debounce = 0;
    } else if (e->debounce < 3 && ++e->debounce == 3 && raw != e->stable) {
        e->stable = raw;
        if (raw) {
            e->press_ms = now_ms;
            e->long_fired = false;
        } else if (!e->long_fired) {
            ev = BTN_SHORT;
        }
    }
    if (e->stable && !e->long_fired && now_ms - e->press_ms >= LONG_PRESS_MS) {
        e->long_fired = true;
        ev = BTN_LONG;
    }
    return ev;
}
