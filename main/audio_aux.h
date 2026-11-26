#pragma once
#include <stdint.h>
void aux_init(void);
void aux_format(unsigned rate, unsigned channels);
void aux_pcm(const uint8_t *data, uint32_t bytes);
