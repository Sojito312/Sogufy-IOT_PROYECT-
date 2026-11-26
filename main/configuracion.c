#include "configuracion.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "FIAD";

bool fiad_load_config(fiad_config_t *config)
{
    nvs_handle_t handle;
    if (nvs_open("fiad", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(*config);
    esp_err_t result = nvs_get_blob(handle, "cred_v1", config, &size);
    nvs_close(handle);
    return result == ESP_OK && size == sizeof(*config)
        && config->ssid[0] && config->client_id[0] && config->refresh_token[0];
}

bool fiad_save_config(const fiad_config_t *config)
{
    nvs_handle_t handle;
    if (nvs_open("fiad", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(handle, "cred_v1", config, sizeof(*config));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) ESP_LOGW(TAG, "NVS: %s", esp_err_to_name(result));
    return result == ESP_OK;
}

static bool get_string(cJSON *json, const char *key, char *dest, size_t size)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
    if (!value || strlen(value) >= size) return false;
    memcpy(dest, value, strlen(value) + 1);
    return true;
}

static bool accept_config(const char *payload)
{
    fiad_config_t config = {0};
    cJSON *json = cJSON_ParseWithOpts(payload, NULL, true);
    bool valid = json
        && get_string(json, "ssid", config.ssid, sizeof(config.ssid)) && config.ssid[0]
        && get_string(json, "password", config.password, sizeof(config.password))
        && get_string(json, "client_id", config.client_id, sizeof(config.client_id))
        && strlen(config.client_id) == 32
        && get_string(json, "refresh_token", config.refresh_token, sizeof(config.refresh_token))
        && config.refresh_token[0];
    cJSON_Delete(json);
    return valid && fiad_save_config(&config);
}

void fiad_wait_for_usb_config(void)
{
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, 115200));
    if (!uart_is_driver_installed(UART_NUM_0)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 3072, 0, 0, NULL, 0));
    }
    static char line[2048];
    size_t length = 0;
    bool overflow = false;
    const char prefix[] = "FIAD_CONFIG ";
    ESP_LOGI(TAG, "Ejecuta configurar_fiad.py para configurar Wi-Fi y Spotify");
    for (;;) {
        if (gpio_get_level(GPIO_NUM_33) == 0) return;
        unsigned char byte;
        if (uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(200)) != 1) continue;
        if (byte == '\r') continue;
        if (byte != '\n') {
            if (length < sizeof(line) - 1) line[length++] = (char)byte;
            else overflow = true;
            continue;
        }
        line[length] = 0;
        if (!overflow && strcmp(line, "FIAD?") == 0) {
            puts("FIAD_CONFIG_READY");
            fflush(stdout);
        } else if (!overflow && strncmp(line, prefix, sizeof(prefix) - 1) == 0) {
            const bool saved = accept_config(line + sizeof(prefix) - 1);
            memset(line, 0, sizeof(line));
            puts(saved ? "FIAD_CONFIG_OK" : "FIAD_CONFIG_ERROR");
            fflush(stdout);
            if (saved) return;
        }
        length = 0;
        overflow = false;
    }
}
