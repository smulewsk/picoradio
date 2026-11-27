#pragma once
#include <stdint.h>

typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG } btn_event_t;

void        encoders_init(void);
int         encoder_read(int idx);            /* detents since last call (+/-) */
btn_event_t encoder_button(int idx, uint32_t now_ms);  /* call every ~10-20 ms */
