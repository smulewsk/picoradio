#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Station list, editable at run time and kept in flash.
 * The stations in config.h are the factory defaults, used until the list
 * is first edited (or after stations_reset()). */

#define ST_NAME_MAX  48     /* bytes incl. terminator (UTF-8) */
#define ST_URL_MAX   208
#define ST_MAX       31
/* A station found by the AI in the radio-browser.info directory plays under
 * this index without being added to the list (stations_set_temp). */
#define ST_TEMP      ST_MAX

void stations_init(void);
int  stations_count(void);
bool stations_get(int i, char *name, size_t nn, char *url, size_t un);
bool stations_is_default(void);   /* still using the config.h list */
void stations_set_temp(const char *name, const char *url);
int  stations_find(const char *name);   /* saved station whose name contains `name` (any case), -1 if none */

/* Edits (saved to flash right away; each costs ~0.1 s of audio). They also
 * fix up g_radio.req_station / last_station so the right station keeps playing.
 * Return false when the input is invalid, the list is full or saving failed. */
bool stations_add(const char *name, const char *url);
bool stations_update(int i, const char *name, const char *url);
bool stations_delete(int i);
bool stations_move(int i, int delta);          /* delta -1 = up, +1 = down */
bool stations_reset(void);                     /* back to the config.h list */

/* Import: text with one "name<TAB>url" per line. replace = drop the old list. */
int  stations_import(const char *lines, bool replace);   /* returns stations added, -1 on error */
