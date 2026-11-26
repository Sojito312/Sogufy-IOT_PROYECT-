#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "spotify.h"
#include "configuracion.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "audio_aux.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pantalla.h"

#define DEVICE_NAME "FIAD-Music"
typedef struct {
    bool connected;
    bool playing;
    bool audio_active;
    bool has_status;
    uint32_t duration;
    uint32_t position;
    int64_t sampled_us;
    char title[160];
    char artist[120];
} track_t;
static track_t track;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;

static void remote_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p)
{
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        portENTER_CRITICAL(&guard);
        bool audio_active = track.audio_active && p->conn_stat.connected;
        memset(&track, 0, sizeof(track));
        track.audio_active = audio_active;
        track.connected = p->conn_stat.connected;
        portEXIT_CRITICAL(&guard);
    } else if (event == ESP_AVRC_CT_METADATA_RSP_EVT) {
        const bool title = p->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_TITLE;
        if (!title && p->meta_rsp.attr_id != ESP_AVRC_MD_ATTR_ARTIST) return;
        if (p->meta_rsp.attr_length <= 0 || !p->meta_rsp.attr_text) return;
        size_t length = p->meta_rsp.attr_length;
        size_t capacity = title ? sizeof(track.title) : sizeof(track.artist);
        if (length >= capacity) length = capacity - 1;
        portENTER_CRITICAL(&guard);
        if (track.connected) {
            char *dest = title ? track.title : track.artist;
            if (title && (strlen(dest) != length || memcmp(dest, p->meta_rsp.attr_text, length))) {
                track.duration = 0;
                track.position = 0;
            }
            memcpy(dest, p->meta_rsp.attr_text, length);
            dest[length] = 0;
        }
        portEXIT_CRITICAL(&guard);
    } else if (event == ESP_AVRC_CT_PLAY_STATUS_RSP_EVT &&
            p->play_status_rsp.play_status != ESP_AVRC_PLAYBACK_ERROR) {
        int64_t sampled = esp_timer_get_time();
        portENTER_CRITICAL(&guard);
        if (track.connected) {
            track.has_status = true;
            track.playing = p->play_status_rsp.play_status == ESP_AVRC_PLAYBACK_PLAYING;
            track.duration = p->play_status_rsp.song_length == UINT32_MAX ? 0 : p->play_status_rsp.song_length;
            track.position = p->play_status_rsp.song_position == UINT32_MAX ? 0 : p->play_status_rsp.song_position;
            track.sampled_us = sampled;
        }
        portEXIT_CRITICAL(&guard);
    }
}

static void audio_event(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p)
{
    if (event == ESP_A2D_AUDIO_CFG_EVT && p->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
        const esp_a2d_cie_sbc_t *sbc = &p->audio_cfg.mcc.cie.sbc_info;
        // SBC selected configuration: bits 3..0 = 16, 32, 44.1, 48 kHz.
        unsigned rate = 44100;
        switch (sbc->samp_freq) {
        case 8: rate = 16000; break;
        case 4: rate = 32000; break;
        case 1: rate = 48000; break;
        }
        aux_format(rate, sbc->ch_mode == 8 ? 1 : 2);
    }
    if (event == ESP_A2D_AUDIO_STATE_EVT) {
        portENTER_CRITICAL(&guard);
        track.audio_active = p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        portEXIT_CRITICAL(&guard);
    }
    if (event == ESP_A2D_CONNECTION_STATE_EVT)
        ESP_LOGI("BT","Estado del enlace A2DP: %d",p->conn_stat.state);
}
static void gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p)
{
    if (event == ESP_BT_GAP_AUTH_CMPL_EVT)
        ESP_LOGI("BT","Emparejamiento: %s",p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK" : "fallo");
}

static void bluetooth_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t ctl = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&ctl));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    esp_bluedroid_config_t host = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&host));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_event));
    esp_bt_io_cap_t cap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,&cap,sizeof(cap)));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(DEVICE_NAME));
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(remote_event));
    ESP_ERROR_CHECK(esp_a2d_register_callback(audio_event));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(aux_pcm));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,ESP_BT_GENERAL_DISCOVERABLE));

}

static const int key_pins[] = {32, 33, 13};

static void init_keys(void)
{
    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << 32) | (1ULL << 33) | (1ULL << 13),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&inputs));
}

static void send_key(uint8_t *label, uint8_t command)
{
    esp_err_t err = esp_avrc_ct_send_passthrough_cmd(*label, command, ESP_AVRC_PT_CMD_STATE_PRESSED);
    *label = (*label + 1) % 16;
    if (err == ESP_OK) {
        err = esp_avrc_ct_send_passthrough_cmd(*label, command, ESP_AVRC_PT_CMD_STATE_RELEASED);
        *label = (*label + 1) % 16;
    }
    if (err != ESP_OK) ESP_LOGW("BT", "Mando: %s", esp_err_to_name(err));
}

static void read_keys(bool connected, bool playing, uint8_t *label)
{
    static uint8_t history[] = {255, 255, 255};
    static bool pressed[3];
    uint8_t commands[] = {
        ESP_AVRC_PT_CMD_BACKWARD,
        playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY,
        ESP_AVRC_PT_CMD_FORWARD,
    };
    for (unsigned i = 0; i < 3; ++i) {
        history[i] = (uint8_t)((history[i] << 1) | gpio_get_level(key_pins[i]));
        if (history[i] == 0 && !pressed[i]) {
            pressed[i] = true;
            if (connected) send_key(label, commands[i]);
        }
        if (history[i] == 255) pressed[i] = false;
    }
}

static uint32_t position_now(const player_screen_t *screen, int64_t sampled)
{
    uint64_t position = screen->position;
    if (screen->playing && sampled) position += (esp_timer_get_time() - sampled) / 1000;
    return position > screen->duration ? screen->duration : (uint32_t)position;
}

static bool source_matches(const track_t *bt, const spotify_view_t *web)
{
    return bt->connected && *bt->title && *web->title && !strcasecmp(bt->title, web->title)
        && (!*bt->artist || !*web->artist || !strncasecmp(bt->artist, web->artist, strlen(web->artist)));
}

void app_main(void)
{
    pantalla_init();
    init_keys();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    static fiad_config_t config;
    bool configured = fiad_load_config(&config);
    if (!configured || gpio_get_level(key_pins[0]) == 0) {
        player_screen_t setup = {.title = "Configura USB", .artist = "Script USB o botón D33", .source = "USB"};
        pantalla_dibujar(&setup, true);
        fiad_wait_for_usb_config();
        configured = fiad_load_config(&config);
    }
    aux_init();
    bluetooth_init();
    spotify_start(configured ? &config : NULL);
    memset(&config, 0, sizeof(config));

    ESP_LOGI("BT", "Busca %s en Bluetooth", DEVICE_NAME);
    uint8_t label = 0;
    TickType_t last_request = 0;
    TickType_t last_draw = 0;
    bool was_connected = false;
    bool first = true;
    static char previous_title[160];
    static char previous_artist[120];
    static char previous_source[16];
    unsigned previous_revision = 0;
    bool previous_playing = false;
    uint32_t previous_position = 0, previous_duration = 0;
    for (;;) {
        track_t bt;
        portENTER_CRITICAL(&guard);
        bt = track;
        portEXIT_CRITICAL(&guard);
        TickType_t now = xTaskGetTickCount();
        bool changed_link = bt.connected != was_connected;
        if (bt.connected && (changed_link || now - last_request >= pdMS_TO_TICKS(2000))) {
            esp_avrc_ct_send_metadata_cmd(label, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
            label = (label + 1) % 16;
            esp_avrc_ct_send_get_play_status_cmd(label);
            label = (label + 1) % 16;
            last_request = now;
        }
        was_connected = bt.connected;

        const spotify_view_t *web = spotify_lock();
        bool matches = source_matches(&bt, web);
        bool playing = bt.has_status ? bt.playing : bt.audio_active;
        player_screen_t screen = {
            .title = *bt.title ? bt.title : (bt.connected ? "Pon una canción" : "FIAD-Music"),
            .artist = *bt.artist ? bt.artist : (bt.connected ? web->status : "Empareja tu iPhone"),
            .source = matches ? "Spotify" : "BT",
            .playing = playing, .connected = bt.connected,
            .position = bt.position, .duration = bt.duration,
        };
        int64_t sampled = bt.sampled_us;
        if (matches) {
            screen.title = web->title;
            screen.artist = web->artist;
            screen.cover = web->art;
            screen.cover_width = web->art_width;
            screen.cover_height = web->art_height;
            if (!bt.duration) {
                screen.duration = web->duration_ms;
                screen.position = web->progress_ms;
                sampled = web->sampled_us;
            }
        }
        unsigned revision = matches ? web->revision : 0;
        bool full = first || changed_link || previous_revision != revision
            || strcmp(previous_title, screen.title) || strcmp(previous_artist, screen.artist)
            || strcmp(previous_source, screen.source);
        screen.position = position_now(&screen, sampled);
        bool clock_changed = screen.position / 1000 != previous_position / 1000
            || screen.duration != previous_duration;
        if (full || playing != previous_playing
                || (clock_changed && now - last_draw >= pdMS_TO_TICKS(1000))) {
            pantalla_dibujar(&screen, full);
            strlcpy(previous_title, screen.title, sizeof(previous_title));
            strlcpy(previous_artist, screen.artist, sizeof(previous_artist));
            strlcpy(previous_source, screen.source, sizeof(previous_source));
            previous_revision = revision;
            previous_playing = playing;
            previous_position = screen.position;
            previous_duration = screen.duration;
            last_draw = now;
            first = false;
        }
        spotify_unlock();
        read_keys(bt.connected, playing, &label);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
