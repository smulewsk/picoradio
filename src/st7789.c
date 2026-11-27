#include <stdbool.h>
#include <stdio.h>
#include "st7789.h"
#include "config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "tft_spi.pio.h"

/* The panel is written through a PIO state machine (tft_spi.pio), so SCL/SDA
 * can be any GPIOs, not only the SPI block's pins. */
static int s_dma = -1;
static PIO s_pio;
static uint s_sm;

static inline void spi_put(uint8_t b) {
    while (pio_sm_is_tx_fifo_full(s_pio, s_sm)) tight_loop_contents();
    *(io_rw_8 *)&s_pio->txf[s_sm] = b;
}

/* waits until the last bit has left the pin */
static void spi_wait_idle(void) {
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm);
    while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm)) tight_loop_contents();
    s_pio->fdebug = stall;
    while (!(s_pio->fdebug & stall)) tight_loop_contents();
}

static void spi_write(const uint8_t *p, size_t n) {
    while (n--) spi_put(*p++);
    spi_wait_idle();
}

static inline void cs(bool level) {
    gpio_put(PIN_TFT_CS, level);
}

static void cmd(uint8_t c, const uint8_t *data, size_t n) {
    cs(0);
    gpio_put(PIN_TFT_DC, 0);
    spi_write(&c, 1);
    if (n) {
        gpio_put(PIN_TFT_DC, 1);
        spi_write(data, n);
    }
    cs(1);
}

static void cmd1(uint8_t c, uint8_t d) { cmd(c, &d, 1); }

/* Backlight brightness 0..255 (PWM on the BL pin) */
void st7789_backlight_level(uint8_t level) {
#if PIN_TFT_BL >= 0
    pwm_set_gpio_level(PIN_TFT_BL, (uint16_t)level * level / 255);   /* ~gamma: smoother fades */
#else
    (void)level;
#endif
}

void st7789_backlight(bool on) {
    st7789_backlight_level(on ? 255 : 0);
}

void st7789_init(void) {
    uint offset;
    bool ok = pio_claim_free_sm_and_add_program(&tft_spi_program, &s_pio, &s_sm, &offset);
    printf("[tft] PIO %s (pio%u sm%u)\n", ok ? "ok" : "NOT AVAILABLE", ok ? (unsigned)pio_get_index(s_pio) : 0u, ok ? s_sm : 0u);
    configASSERT(ok);
    (void)ok;
    /* 2 PIO cycles per bit; whole divider so the clock edges stay even */
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t div = (sys + 2u * TFT_SPI_HZ - 1) / (2u * TFT_SPI_HZ);
    tft_spi_program_init(s_pio, s_sm, offset, PIN_TFT_SCK, PIN_TFT_MOSI, (float)(div ? div : 1));

    const uint pins[] = { PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST };
    for (unsigned i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 1);
    }
#if PIN_TFT_BL >= 0
    gpio_set_function(PIN_TFT_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_TFT_BL);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_wrap(&pc, 254);                  /* 8-bit levels, ~590 kHz at 150 MHz */
    pwm_init(slice, &pc, true);
#if TFT_BL_ACTIVE_LOW
    gpio_set_outover(PIN_TFT_BL, GPIO_OVERRIDE_INVERT);   /* level 0 = BL high = off */
#endif
    pwm_set_gpio_level(PIN_TFT_BL, 0);
#endif

    printf("[tft] backlight on GP%d, reset\n", PIN_TFT_BL);
    /* hardware reset */
    gpio_put(PIN_TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_put(PIN_TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    printf("[tft] sending init commands\n");
    cmd(0x01, NULL, 0);                 /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11, NULL, 0);                 /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd1(0x3A, 0x55);                   /* COLMOD: 16 bit */
    cmd1(0x36, TFT_MADCTL | (TFT_BGR ? 0x08 : 0x00));
    static const uint8_t porch[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    cmd(0xB2, porch, sizeof porch);
    cmd1(0xB7, 0x35);                   /* gate control */
    cmd1(0xBB, 0x19);                   /* VCOM */
    cmd1(0xC0, 0x2C);
    cmd1(0xC2, 0x01);
    cmd1(0xC3, 0x12);
    cmd1(0xC4, 0x20);
    cmd1(0xC6, 0x0F);                   /* 60 Hz */
    static const uint8_t pwr[] = { 0xA4, 0xA1 };
    cmd(0xD0, pwr, sizeof pwr);
    cmd(TFT_INVERT ? 0x21 : 0x20, NULL, 0);
    cmd(0x13, NULL, 0);                 /* NORON */
    cmd(0x29, NULL, 0);                 /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));

    printf("[tft] init done\n");
    s_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));
    dma_channel_configure(s_dma, &c, (io_rw_8 *)&s_pio->txf[s_sm], NULL, 0, false);
}

void st7789_push(const uint16_t *fb) {
    const uint16_t x0 = TFT_X_OFFSET, x1 = TFT_X_OFFSET + TFT_WIDTH - 1;
    const uint16_t y0 = TFT_Y_OFFSET, y1 = TFT_Y_OFFSET + TFT_HEIGHT - 1;
    uint8_t ca[] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t ra[] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);

    uint8_t c = 0x2C;                   /* RAMWR */
    cs(0);
    gpio_put(PIN_TFT_DC, 0);
    spi_write(&c, 1);
    gpio_put(PIN_TFT_DC, 1);

    dma_channel_transfer_from_buffer_now(s_dma, fb, TFT_WIDTH * TFT_HEIGHT * 2);
    while (dma_channel_is_busy(s_dma)) vTaskDelay(1);
    spi_wait_idle();
    cs(1);
}
