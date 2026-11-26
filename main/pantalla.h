#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *title;
    const char *artist;
    const char *source;
    bool playing;
    bool connected;
    uint32_t position;
    uint32_t duration;
    const uint8_t *cover;
    unsigned cover_width;
    unsigned cover_height;
} player_screen_t;

void pantalla_init(void);
void pantalla_dibujar(const player_screen_t *screen, bool completa);
