#include "spotify.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"

static const char *TAG = "Spotify";
static const char *TOKEN_URL = "https://accounts.spotify.com/api/token";
static const char *PLAYER_URL = "https://api.spotify.com/v1/me/player/currently-playing";
static SemaphoreHandle_t view_mutex;
static spotify_view_t view = {.status = "Conectando Wi-Fi"};
static fiad_config_t settings;
static char access_token[2048];
static int64_t expires_us;
static unsigned retry_seconds;

const spotify_view_t *spotify_lock(void)
{
    if (view_mutex) xSemaphoreTake(view_mutex, portMAX_DELAY);
    return &view;
}

void spotify_unlock(void)
{
    if (view_mutex) xSemaphoreGive(view_mutex);
}

static void clear_view(const char *status)
{
    spotify_lock();
    unsigned revision = view.revision + 1;
    free((void *)view.art);
    view = (spotify_view_t){.status = status, .revision = revision};
    spotify_unlock();
}

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t limit;
    bool full;
} body_t;

static esp_err_t receive(esp_http_client_event_t *event)
{
    body_t *body = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value
            && !strcasecmp(event->header_key, "Retry-After")) {
        unsigned long seconds = strtoul(event->header_value, NULL, 10);
        if (seconds && seconds <= 86400) retry_seconds = (unsigned)seconds;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        size_t size = (size_t)event->data_len;
        if (body->full) return ESP_FAIL;
        size_t needed = body->length + size + 1;
        if (needed > body->capacity) {
            size_t capacity = body->capacity * 2;
            if (capacity < needed) capacity = needed;
            if (capacity > body->limit) capacity = body->limit;
            uint8_t *data = capacity >= needed ? realloc(body->data, capacity) : NULL;
            if (!data) {
                body->full = true;
                return ESP_FAIL;
            }
            body->data = data;
            body->capacity = capacity;
        }
        memcpy(body->data + body->length, event->data, size);
        body->length += size;
    }
    return ESP_OK;
}

static int request(const char *url, const char *form, const char *authorization, body_t *body)
{
    body->length = 0;
    body->full = false;
    retry_seconds = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = form ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 6500,
        .buffer_size = 1024,
        .event_handler = receive,
        .user_data = body,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_err_t result = ESP_OK;
    if (authorization) {
        result = esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (form && result == ESP_OK) {
        result = esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        if (result == ESP_OK) result = esp_http_client_set_post_field(client, form, strlen(form));
    }
    if (result == ESP_OK) result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    body->data[body->length] = 0;
    if (result != ESP_OK || body->full) {
        ESP_LOGW(TAG, "HTTPS: %s%s", esp_err_to_name(result), body->full ? " (limite de memoria o de respuesta)" : "");
        return -1;
    }
    return status;
}

static const char *string_at(const cJSON *object, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

static uint32_t number_at(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble <= 0 || value->valuedouble > UINT32_MAX) return 0;
    return (uint32_t)value->valuedouble;
}

// The refresh token is opaque. Encode bytes outside the URL unreserved set.
static int refresh_access(body_t *body)
{
    char *form = malloc(128 + 3 * sizeof(settings.refresh_token));
    if (!form) return -1;
    char *out = form + sprintf(form, "grant_type=refresh_token&client_id=%s&refresh_token=", settings.client_id);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)settings.refresh_token; *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')
                || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            *out++ = (char)*p;
        } else {
            *out++ = '%';
            *out++ = hex[*p >> 4];
            *out++ = hex[*p & 15];
        }
    }
    *out = 0;
    int status = request(TOKEN_URL, form, NULL, body);
    free(form);
    if (status != 200) return status;

    cJSON *json = cJSON_Parse((const char *)body->data);
    const char *token = string_at(json, "access_token");
    const char *rotation = string_at(json, "refresh_token");
    uint32_t lifetime = number_at(json, "expires_in");
    if (!token || !*token || strlen(token) >= sizeof(access_token) || lifetime <= 30) {
        cJSON_Delete(json);
        return -1;
    }
    strlcpy(access_token, token, sizeof(access_token));
    expires_us = esp_timer_get_time() + (int64_t)(lifetime - 30) * 1000000;
    if (rotation && *rotation && strlen(rotation) < sizeof(settings.refresh_token)
            && strcmp(rotation, settings.refresh_token)) {
        strlcpy(settings.refresh_token, rotation, sizeof(settings.refresh_token));
        fiad_save_config(&settings);
    }
    cJSON_Delete(json);
    return 200;
}

enum { ART_URL_SIZE = 384, ART_MAX_SIZE = 36 * 1024, PLAYER_MAX_SIZE = 20 * 1024 };

static bool is_image_url(const char *url)
{
    return url && (strncmp(url, "https://i.scdn.co/", sizeof("https://i.scdn.co/") - 1) == 0
        || strncmp(url, "https://image-cdn-ak.spotifycdn.com/", sizeof("https://image-cdn-ak.spotifycdn.com/") - 1) == 0);
}

static void select_urls(const cJSON *album, char *preferred, char *smallest)
{
    const cJSON *images = cJSON_GetObjectItemCaseSensitive(album, "images");
    const cJSON *image;
    unsigned best = 0;
    unsigned smallest_size = UINT_MAX;
    cJSON_ArrayForEach(image, images) {
        const char *url = string_at(image, "url");
        unsigned width = number_at(image, "width");
        if (!is_image_url(url) || strlen(url) >= ART_URL_SIZE) continue;
        if (width > best && width <= 320) {
            best = width;
            strlcpy(preferred, url, ART_URL_SIZE);
        }
        if (!smallest[0] || (width && width < smallest_size)) {
            smallest_size = width ? width : UINT_MAX;
            strlcpy(smallest, url, ART_URL_SIZE);
        }
    }
    if (!preferred[0]) strlcpy(preferred, smallest, ART_URL_SIZE);
}

static uint8_t *fetch_art(const char *url, esp_jpeg_image_output_t *output)
{
    const size_t initial_size = 4096;
    body_t body = {.data = malloc(initial_size), .capacity = initial_size, .limit = ART_MAX_SIZE};
    if (!body.data) {
        ESP_LOGW(TAG, "Portada: falta memoria para iniciar la descarga");
        return NULL;
    }
    uint8_t *pixels = NULL;
    int status = request(url, NULL, NULL, &body);
    ESP_LOGI(TAG, "Portada: HTTP %d, %u bytes", status, (unsigned)body.length);
    if (status == 200) {
        esp_jpeg_image_cfg_t cfg = {
            .indata = body.data,
            .indata_size = body.length,
            .out_format = JPEG_IMAGE_FORMAT_RGB565,
            .out_scale = JPEG_IMAGE_SCALE_0,
            .flags.swap_color_bytes = 1,
        };
        const esp_jpeg_image_scale_t scales[] = {
            JPEG_IMAGE_SCALE_0, JPEG_IMAGE_SCALE_1_2, JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_8,
        };
        for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
            cfg.out_scale = scales[i];
            esp_err_t err = esp_jpeg_get_image_info(&cfg, output);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Portada: imagen JPEG no reconocida (%s)", esp_err_to_name(err));
                break;
            }
            if (!output->output_len || output->output_len > 48 * 1024) continue;
            pixels = malloc(output->output_len);
            if (!pixels) continue;
            cfg.outbuf = pixels;
            cfg.outbuf_size = output->output_len;
            err = esp_jpeg_decode(&cfg, output);
            if (err == ESP_OK) break;
            ESP_LOGW(TAG, "Portada: error al decodificar JPEG (%s)", esp_err_to_name(err));
            free(pixels);
            pixels = NULL;
        }
    }
    if (status == 200 && !pixels) ESP_LOGW(TAG, "Portada: sin imagen RGB565 utilizable");
    free(body.data);
    return pixels;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xTaskNotifyGive((TaskHandle_t)arg);
        ESP_LOGI(TAG, "Wi-Fi conectado");
    }
}

static esp_err_t wifi_setup(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;

    wifi_config_t station = {0};
    memcpy(station.sta.ssid, settings.ssid, strlen(settings.ssid));
    memcpy(station.sta.password, settings.password, strlen(settings.password));
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &station)) != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, xTaskGetCurrentTaskHandle());
    if (err != ESP_OK) return err;
    return esp_wifi_start();
}

static bool has_wifi(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t address = {0};
    return netif && esp_netif_is_netif_up(netif)
        && esp_netif_get_ip_info(netif, &address) == ESP_OK && address.ip.addr != 0;
}

static void spotify_task(void *arg)
{
    esp_err_t err = wifi_setup();
    if (err != ESP_OK) {
        clear_view("Error Wi-Fi");
        ESP_LOGW(TAG, "Inicio Wi-Fi: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
    bool sntp_started = false;
    static char current_url[ART_URL_SIZE];
    int64_t last_art_us = 0;
    for (;;) {
        unsigned wait = 4;
        if (!has_wifi()) {
            current_url[0] = 0;
            clear_view("Conectando Wi-Fi");
            esp_wifi_connect();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(12000));
            continue;
        }
        if (!sntp_started) {
            esp_netif_sntp_start();
            sntp_started = true;
        }
        if (time(NULL) < 1704067200) {
            clear_view("Ajustando reloj");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const size_t initial_size = 4096;
        body_t response = {.data = malloc(initial_size), .capacity = initial_size, .limit = PLAYER_MAX_SIZE};
        if (!response.data) {
            current_url[0] = 0;
            clear_view("Memoria baja");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        bool needs_token = esp_timer_get_time() >= expires_us;
        int token_status = needs_token ? refresh_access(&response) : 200;
        int status = token_status;
        if (token_status == 200) {
            char header[sizeof(access_token) + 8];
            snprintf(header, sizeof(header), "Bearer %s", access_token);
            status = request(PLAYER_URL, NULL, header, &response);
        }

        int64_t sampled = esp_timer_get_time();
        cJSON *json = status == 200 ? cJSON_Parse((const char *)response.data) : NULL;
        free(response.data);
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "item");
        const char *type = string_at(item, "type");
        const char *title = string_at(item, "name");
        if (!type || strcmp(type, "track") || !title || !*title) {
            const char *message = "Spotify sin cancion";
            if (status == 429) {
                message = "Spotify en espera";
                wait = retry_seconds ? retry_seconds : 30;
            } else if (status == 403 || (token_status == 400 && needs_token)) {
                message = "Revisa acceso Spotify";
                wait = 60;
            } else if (status == 401) {
                expires_us = 0;
            } else if (status != 200 && status != 204) {
                message = "Spotify sin respuesta";
                wait = 10;
            }
            if (status != 200 && status != 204) ESP_LOGW(TAG, "Estado HTTP %d", status);
            spotify_lock();
            bool keep_art = (status < 0 || (status >= 500 && status < 600)) && view.art;
            spotify_unlock();
            if (!keep_art) {
                current_url[0] = 0;
                clear_view(message);
            }
            cJSON_Delete(json);
            vTaskDelay(pdMS_TO_TICKS(wait * 1000));
            continue;
        }

        char desired[ART_URL_SIZE] = {0}, fallback[ART_URL_SIZE] = {0};
        select_urls(cJSON_GetObjectItemCaseSensitive(item, "album"), desired, fallback);
        const cJSON *artists = cJSON_GetObjectItemCaseSensitive(item, "artists");
        const char *artist = string_at(cJSON_GetArrayItem(artists, 0), "name");
        bool art_changed = strcmp(desired, current_url) != 0;
        spotify_lock();
        if (art_changed) {
            free((void *)view.art);
            view.art = NULL;
            view.art_width = 0;
            view.art_height = 0;
        }
        view.status = "Spotify";
        strlcpy(view.title, title, sizeof(view.title));
        strlcpy(view.artist, artist ? artist : "", sizeof(view.artist));
        view.progress_ms = number_at(json, "progress_ms");
        view.duration_ms = number_at(item, "duration_ms");
        view.sampled_us = sampled;
        view.playing = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "is_playing"));
        bool download = desired[0] && !view.art
            && (art_changed || sampled - last_art_us >= 30000000);
        if (art_changed) view.revision++;
        spotify_unlock();
        cJSON_Delete(json);
        strlcpy(current_url, desired, sizeof(current_url));

        if (!desired[0] && sampled - last_art_us >= 30000000) {
            ESP_LOGW(TAG, "Portada: Spotify no devolvio una URL compatible para la pista");
            last_art_us = sampled;
        }
        if (download) {
            esp_jpeg_image_output_t image = {0};
            const char *first = fallback[0] ? fallback : desired;
            uint8_t *pixels = fetch_art(first, &image);
            if (!pixels && strcmp(first, desired)) pixels = fetch_art(desired, &image);
            last_art_us = esp_timer_get_time();
            if (pixels) {
                spotify_lock();
                view.art = pixels;
                view.art_width = image.width;
                view.art_height = image.height;
                view.revision++;
                spotify_unlock();
                ESP_LOGI(TAG, "Portada %ux%u", (unsigned)image.width, (unsigned)image.height);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait * 1000));
    }
}

void spotify_start(const fiad_config_t *config)
{
    if (!config) {
        view.status = "Falta configurar Wi-Fi";
        return;
    }
    settings = *config;
    view_mutex = xSemaphoreCreateMutex();
    if (!view_mutex) {
        view.status = "Memoria baja";
        return;
    }
    if (xTaskCreate(spotify_task, "spotify", 9216, NULL, 3, NULL) != pdPASS) {
        view.status = "Memoria baja";
    }
}
