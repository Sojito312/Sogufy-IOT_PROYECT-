#pragma once
#include <stdbool.h>

typedef struct {
    char ssid[33];
    char password[65];
    char client_id[33];
    char refresh_token[1024];
} fiad_config_t;

bool fiad_load_config(fiad_config_t *config);
bool fiad_save_config(const fiad_config_t *config);
void fiad_wait_for_usb_config(void);
