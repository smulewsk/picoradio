#pragma once
#include <stdbool.h>
#include "stations.h"

/* radio-browser.info: free directory of internet radio stations.
 * Searched over plain HTTP (the TLS connection is busy with Gemini). */

typedef struct {
    char name[ST_NAME_MAX];
    char url[ST_URL_MAX];
    char codec[8];
    char country[4];
    int  bitrate;
} rb_station_t;

/* Finds playable stations (MP3 / AAC, not HLS), most popular first.
 * Any of name / tag / countrycode may be NULL or empty. Returns how many were
 * found (0..max), or -1 if the directory couldn't be reached. */
int rb_search(const char *name, const char *tag, const char *countrycode, rb_station_t *out, int max);
