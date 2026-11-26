#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "configuracion.h"

typedef struct {
    const char *status;
    char title[160];
    char artist[120];
    uint32_t duration_ms;
    uint32_t progress_ms;
    int64_t sampled_us;
    bool playing;
    const uint8_t *art;
    unsigned art_width;
    unsigned art_height;
    unsigned revision;
} spotify_view_t;

void spotify_start(const fiad_config_t *config);
// The cover pointer remains valid until spotify_unlock().
const spotify_view_t *spotify_lock(void);
void spotify_unlock(void);
