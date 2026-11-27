#pragma once
#include <stdint.h>

/* Runs the AI voice source until the user switches away (tune_seq changes).
 * Called from the net task; WiFi must already be connected. */
void ai_mode(uint32_t seq);
