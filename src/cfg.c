/*
 * Settings store: two 4 KB flash sectors written alternately, each with a
 * sequence number and CRC (like the stations), so a power cut while saving
 * keeps the previous settings. Below the settings log (settings.c), outside
 * the OTA firmware slots.
 *
 *   0x3F6000 / 0x3F7000 (4 MB flash): cfg A / cfg B
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "cfg.h"
#include "config.h"

#define CFG_OFFSET  (PICO_FLASH_SIZE_BYTES - 10 * FLASH_SECTOR_SIZE)   /* A; B is the next sector */
#define CFG_MAGIC   0x43464732u                                       /* "CFG2" */
#define CFG1_MAGIC  0x43464731u                                       /* older layout, converted */

#if PICO_RP2040
#define READ_BASE XIP_BASE
#else
#define READ_BASE XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#endif

typedef struct {
    uint32_t magic, seq, len, crc;
    cfg_t    c;
} store_t;
_Static_assert(sizeof(store_t) <= FLASH_SECTOR_SIZE, "cfg size");

static cfg_t s_cfg;
static int s_active = -1;            /* slot in use, -1 = defaults */
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;

static const store_t *slot(int i) {
    return (const store_t *)(READ_BASE + CFG_OFFSET + (uint32_t)i * FLASH_SECTOR_SIZE);
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* "CFG1": the layout before the WiFi country was removed, with or without
 * ai_language at the end. Read and converted, so saved settings survive. */
typedef struct {
    char    wifi_ssid[33], wifi_pass[64], wifi_country[3], hostname[32], bt_name[32];
    char    gemini_key[64], gemini_model[48], gemini_voice[24], ai_prompt[CFG_PROMPT_MAX];
    uint8_t tls_verify;
    char    ai_language[24];
} cfg_v1_t;

/* len: the cfg_t size of the firmware that saved it; newer firmware adds
 * fields at the end, so a shorter record is fine (the rest keep defaults) */
#define CFG_MIN_LEN  offsetof(cfg_t, ai_language)
#define CFG1_MIN_LEN offsetof(cfg_v1_t, ai_language)
static bool valid(const store_t *s) {
    size_t lo = s->magic == CFG1_MAGIC ? CFG1_MIN_LEN : CFG_MIN_LEN;
    size_t hi = s->magic == CFG1_MAGIC ? sizeof(cfg_v1_t) : sizeof(cfg_t);
    return (s->magic == CFG_MAGIC || s->magic == CFG1_MAGIC) && s->len >= lo && s->len <= hi &&
           s->crc == crc32((const uint8_t *)&s->c, s->len);
}

/* fills c (already holding the defaults) from a saved record */
static void load_record(cfg_t *c, const store_t *s) {
    if (s->magic == CFG_MAGIC) { memcpy(c, &s->c, s->len); return; }
    cfg_v1_t v;
    memset(&v, 0, sizeof v);
    memcpy(&v, &s->c, s->len);
#define CP(f) memcpy(c->f, v.f, sizeof c->f)
    CP(wifi_ssid); CP(wifi_pass); CP(hostname); CP(bt_name);
    CP(gemini_key); CP(gemini_model); CP(gemini_voice); CP(ai_prompt);
    if (s->len > offsetof(cfg_v1_t, ai_language)) CP(ai_language);
#undef CP
    c->tls_verify = v.tls_verify;
    printf("[cfg] converted settings from the older format\n");
}

/* the default instructions before the language setting existed: replaced by
 * the new ones, which leave the language to ai_language */
static const char OLD_DEFAULT_PROMPT[] =
    "Jesteś asystentem głosowym wbudowanym w małe radio internetowe. "
    "Zawsze odpowiadaj po polsku, krótko i naturalnie, zwykle w 1-3 zdaniach, "
    "chyba że ktoś poprosi o więcej. Nie używaj list ani formatowania.";

static void set(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src); }

static void defaults(cfg_t *c) {
    memset(c, 0, sizeof *c);
    set(c->wifi_ssid, sizeof c->wifi_ssid, WIFI_SSID);
    set(c->wifi_pass, sizeof c->wifi_pass, WIFI_PASSWORD);
    set(c->hostname, sizeof c->hostname, WEB_HOSTNAME);
    set(c->bt_name, sizeof c->bt_name, BT_DEVICE_NAME);
    set(c->gemini_key, sizeof c->gemini_key, GEMINI_API_KEY);
    set(c->gemini_model, sizeof c->gemini_model, GEMINI_MODEL);
    set(c->gemini_voice, sizeof c->gemini_voice, GEMINI_VOICE);
    set(c->ai_prompt, sizeof c->ai_prompt, AI_SYSTEM_PROMPT);
    c->tls_verify = RADIO_TLS_VERIFY;
    set(c->ai_language, sizeof c->ai_language, AI_LANGUAGE);
    c->ai_greet = AI_GREETING;
}

/* flash writes, retried while the FreeRTOS heap is short (see ota.c) */
typedef struct { uint32_t off; const void *data; } op_t;
static void do_op(void *p) {
    const op_t *op = p;
    flash_range_erase(op->off, FLASH_SECTOR_SIZE);
    if (op->data) flash_range_program(op->off, op->data, FLASH_SECTOR_SIZE);
}
static bool flash_op(uint32_t off, const void *data) {
    op_t op = { off, data };
    for (int i = 0; i < 20; i++) {
        if (flash_safe_execute(do_op, &op, 1000) == PICO_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static bool button_held(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
    busy_wait_ms(5);
    for (int i = 0; i < 10; i++) {              /* pressed for 50 ms */
        if (gpio_get(pin)) return false;
        busy_wait_ms(5);
    }
    return true;
}

static bool s_force_ap;
bool cfg_force_ap(void) { return s_force_ap; }

bool cfg_wifi_set(void) {
    return s_cfg.wifi_ssid[0] && strncmp(s_cfg.wifi_ssid, "your-", 5) != 0;
}

void cfg_init(void) {
    s_lock = xSemaphoreCreateMutex();
    defaults(&s_cfg);
    s_force_ap = button_held(PIN_ENC2_SW);
    if (s_force_ap) printf("[cfg] encoder 2 held at power-on: WiFi setup access point\n");
    if (button_held(PIN_ENC1_SW)) {
        printf("[cfg] encoder 1 held at power-on: factory settings (saved ones erased)\n");
        /* erased once the scheduler runs (flash_safe_execute needs it): mark slots */
        s_active = -2;
        return;
    }
    const store_t *a = slot(0), *b = slot(1);
    bool va = valid(a), vb = valid(b);
    if (va && vb) s_active = (int32_t)(b->seq - a->seq) > 0 ? 1 : 0;
    else s_active = va ? 0 : vb ? 1 : -1;
    if (s_active >= 0) {
        load_record(&s_cfg, slot(s_active));                             /* newer fields keep defaults */
        if (!strcmp(s_cfg.ai_prompt, OLD_DEFAULT_PROMPT)) set(s_cfg.ai_prompt, sizeof s_cfg.ai_prompt, AI_SYSTEM_PROMPT);
        s_seq = slot(s_active)->seq;
        /* always terminated, whatever is in flash */
        s_cfg.wifi_ssid[sizeof s_cfg.wifi_ssid - 1] = 0;
        s_cfg.wifi_pass[sizeof s_cfg.wifi_pass - 1] = 0;
        s_cfg.hostname[sizeof s_cfg.hostname - 1] = 0;
        s_cfg.bt_name[sizeof s_cfg.bt_name - 1] = 0;
        s_cfg.gemini_key[sizeof s_cfg.gemini_key - 1] = 0;
        s_cfg.gemini_model[sizeof s_cfg.gemini_model - 1] = 0;
        s_cfg.gemini_voice[sizeof s_cfg.gemini_voice - 1] = 0;
        s_cfg.ai_prompt[sizeof s_cfg.ai_prompt - 1] = 0;
        s_cfg.ai_language[sizeof s_cfg.ai_language - 1] = 0;
        if (!s_cfg.ai_language[0]) set(s_cfg.ai_language, sizeof s_cfg.ai_language, AI_LANGUAGE);
        printf("[cfg] saved settings loaded (WiFi \"%s\")\n", s_cfg.wifi_ssid);
    } else {
        printf("[cfg] no saved settings: defaults from config.h\n");
    }
}

const cfg_t *cfg(void) { return &s_cfg; }

void cfg_copy(cfg_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_cfg, sizeof *out);
    xSemaphoreGive(s_lock);
}

bool cfg_is_default(void) { return s_active < 0; }

static bool erase_marked(void) {
    if (s_active != -2) return true;
    bool ok = flash_op(CFG_OFFSET, NULL) && flash_op(CFG_OFFSET + FLASH_SECTOR_SIZE, NULL);
    if (ok) s_active = -1;
    return ok;
}

void cfg_start(void) {
    if (s_active == -2) printf("[cfg] saved settings %s\n", erase_marked() ? "erased" : "could not be erased");
}

bool cfg_save(const cfg_t *c) {
    erase_marked();
    store_t *st = pvPortMalloc(FLASH_SECTOR_SIZE);
    if (!st) return false;
    memset(st, 0xFF, FLASH_SECTOR_SIZE);
    st->magic = CFG_MAGIC;
    st->seq = s_seq + 1;
    st->len = sizeof(cfg_t);
    memcpy(&st->c, c, sizeof *c);
    st->crc = crc32((const uint8_t *)&st->c, sizeof st->c);
    int target = s_active == 0 ? 1 : 0;
    bool ok = flash_op(CFG_OFFSET + (uint32_t)target * FLASH_SECTOR_SIZE, st) && valid(slot(target));
    vPortFree(st);
    if (!ok) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_cfg, c, sizeof s_cfg);
    xSemaphoreGive(s_lock);
    s_active = target;
    s_seq++;
    return true;
}

bool cfg_reset(void) {
    bool ok = flash_op(CFG_OFFSET, NULL) && flash_op(CFG_OFFSET + FLASH_SECTOR_SIZE, NULL);
    if (!ok) return false;
    cfg_t d;
    defaults(&d);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_cfg, &d, sizeof s_cfg);
    xSemaphoreGive(s_lock);
    s_active = -1;
    return true;
}
