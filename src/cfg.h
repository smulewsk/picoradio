#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Settings changeable from the web page, kept in flash. The values in
 * config.h are the factory defaults. Hardware (pins, display) stays in config.h. */

#define CFG_PROMPT_MAX 600

typedef struct {
    char    wifi_ssid[33];
    char    wifi_pass[64];
    char    hostname[32];             /* http://<hostname>.local */
    char    bt_name[32];              /* Bluetooth name shown on the phone */
    char    gemini_key[64];
    char    gemini_model[48];
    char    gemini_voice[24];
    char    ai_prompt[CFG_PROMPT_MAX];
    uint8_t tls_verify;               /* check HTTPS station certificates */
    /* fields added later go below: older saved settings simply lack them */
    char    ai_language[24];          /* "Polish", "English"... or "auto" */
    uint8_t ai_greet;                 /* say hello when AI mode starts */
} cfg_t;

/* Loads the saved settings (or the defaults). Call once in main(), before the
 * tasks start. Holding encoder 1 pressed at power-on ignores and erases the
 * saved settings: the way back in after a wrong WiFi password. */
void cfg_init(void);

/* The current settings. Strings may change when the web page saves; tasks
 * that need several fields at once copy them with cfg_copy(). */
const cfg_t *cfg(void);
void cfg_copy(cfg_t *out);

bool cfg_save(const cfg_t *c);        /* false if the flash write failed */
bool cfg_reset(void);                 /* back to the config.h defaults */
bool cfg_is_default(void);
bool cfg_wifi_set(void);              /* a real WiFi network is configured */
bool cfg_force_ap(void);              /* encoder 2 held at power-on */
void cfg_start(void);                 /* from a task, once: finishes the encoder-1 reset */
