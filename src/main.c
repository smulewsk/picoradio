/*
 * Pico 2 W Internet Radio
 *   FreeRTOS SMP, lwIP sockets, minimp3, PCM5102 (I2S), ST7789, 2x encoder
 *
 *   core 0: net_task (WiFi + HTTP/ICY), ui_task (display + encoders)
 *   core 1: decoder_task (MP3 decode + audio DMA IRQ)
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "radio.h"
#include "tasks.h"
#include "stations.h"
#include "version.h"
#include "gfx.h"
#include "settings.h"
#include "cfg.h"

#define CORE0 (1u << 0)
#define CORE1 (1u << 1)

int main(void) {
    stdio_init_all();
    printf("\nPico Radio %s (built %s)\n", FW_VERSION_STR, FW_BUILD_STR);
    radio_init();
    cfg_init();                /* settings from the web page (or config.h defaults) */
    stations_init();
    gfx_lock_init();
    settings_load();           /* last station and volume */

    /* net task also runs the TLS handshake for the AI source */
    xTaskCreateAffinitySet(net_task, "net", 6144, NULL, tskIDLE_PRIORITY + 2, CORE0, &g_radio.net_task);
    xTaskCreateAffinitySet(ui_task, "ui", 2048, NULL, tskIDLE_PRIORITY + 1, CORE0, &g_radio.ui_task);
    xTaskCreateAffinitySet(web_task, "web", 1536, NULL, tskIDLE_PRIORITY + 1, CORE0, NULL);
    xTaskCreateAffinitySet(rssi_task, "rssi", 512, NULL, tskIDLE_PRIORITY, CORE0, NULL);   /* lowest: never delays the UI */
    /* minimp3 keeps ~17 KB of scratch on the stack */
    xTaskCreateAffinitySet(decoder_task, "dec", 8192, NULL, tskIDLE_PRIORITY + 3, CORE1, &g_radio.dec_task);

    vTaskStartScheduler();
    for (;;) { }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    printf("STACK OVERFLOW in %s\n", name);
    panic("stack overflow");
}

/* Allocation failures are handled by the callers (TLS, AAC report "out of
 * memory" on screen), so only log them. */
void vApplicationMallocFailedHook(void) {
    printf("FreeRTOS heap: allocation failed\n");
}
