#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { MOSI = 23, SCK = 18, CS = 27, DC = 21, RESET = 22, WIDTH = 240, HEIGHT = 320 };
static spi_device_handle_t lcd;
#include "pantalla.h"
#include "letras.h"

static void send_bytes(int data_mode, const void *bytes, size_t count)
{
    ESP_ERROR_CHECK(gpio_set_level(DC, data_mode));
    spi_transaction_t tx = { .length = count * 8, .tx_buffer = bytes };
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &tx));
}

static void command(uint8_t opcode, const uint8_t *data, size_t count)
{
    send_bytes(0, &opcode, 1);
    if (count) send_bytes(1, data, count);
}

void pantalla_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << DC) | (1ULL << RESET),
        .mode = GPIO_MODE_OUTPUT
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));
    spi_bus_config_t bus = {
        .mosi_io_num = MOSI, .miso_io_num = -1, .sclk_io_num = SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = WIDTH * 2 * 8
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device = {
        .clock_speed_hz = 12000000, .mode = 0, .spics_io_num = CS, .queue_size = 1
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &lcd));
    ESP_ERROR_CHECK(gpio_set_level(RESET, 0));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(gpio_set_level(RESET, 1));
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x01, NULL, 0); // Software reset
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x11, NULL, 0); // Sleep out
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x3A, (uint8_t[]){0x55}, 1); // RGB565
    command(0x36, (uint8_t[]){0x48}, 1); // Portrait, BGR panel
    command(0x29, NULL, 0);
}

enum {
    BACK = 0x0862, PANEL = 0x1105, BORDER = 0x2289,
    TEXT = 0xF7DF, MUTED = 0xA5B7, GREEN = 0x2F28,
    FIRST_CONTROL_ROW = 241, ART_SIZE = 152, ART_Y = 42,
    STRIP = 8
};

static uint8_t pixels[STRIP][WIDTH * 2] __attribute__((aligned(4)));
static uint8_t *line;
static int row;

static void pixel(int x, uint16_t color)
{
    if (x < 0 || x >= WIDTH) return;
    line[x * 2] = (uint8_t)(color >> 8);
    line[x * 2 + 1] = (uint8_t)color;
}

static void box(int x, int y, int width, int height, uint16_t color)
{
    if (row < y || row >= y + height) return;
    for (int col = x; col < x + width; col++) pixel(col, color);
}

static void disk(int cx, int cy, int radius, uint16_t color, bool outline)
{
    int dy = row - cy;
    if (dy < -radius || dy > radius) return;
    for (int dx = -radius; dx <= radius; dx++) {
        int distance = dx * dx + dy * dy;
        if (distance <= radius * radius && (!outline || distance > (radius - 2) * (radius - 2))) {
            pixel(cx + dx, color);
        }
    }
}

static void arrow(int x, int y, int direction, uint16_t color)
{
    int dy = row - y;
    if (dy < 0) dy = -dy;
    for (int i = 0; i < 14 - dy; i++) pixel(x + i * direction, color);
}

static unsigned glyph(const unsigned char **cursor)
{
    const unsigned char *s = *cursor;
    unsigned c = *s++;
    if (c >= 128) {
        unsigned first = c;
        c = '?';
        if ((first == 194 || first == 195) && (*s & 192) == 128) c = ((first & 31) << 6) | (*s & 63);
        while ((*s & 192) == 128) ++s;
    }
    *cursor = s;
    return c < 32 ? '?' : c;
}

static void text_at(int x, int y, int scale, int columns, const char *text, uint16_t color)
{
    int dy = row - y;
    if (dy < 0 || dy >= 16 * scale || !text) return;
    const unsigned char *cursor = (const unsigned char *)text;
    unsigned codes[30];
    int count = 0;
    while (*cursor && count < columns) codes[count++] = glyph(&cursor);
    if (*cursor && count >= 3) {
        codes[count - 1] = codes[count - 2] = codes[count - 3] = '.';
    }
    if (x < 0) x = (WIDTH - count * 8 * scale) / 2;
    for (int i = 0; i < count; ++i) {
        unsigned bits = letras[codes[i] - 32][dy / scale];
        for (int j = 0; j < 8 * scale; ++j) {
            if (bits & (128 >> (j / scale))) pixel(x + i * 8 * scale + j, color);
        }
    }
}

static void title_at(const char *title)
{
    const int y = 197, scale = 2, available = WIDTH - 24;
    int dy = row - y;
    if (dy < 0 || dy >= 16 * scale || !title) return;
    const unsigned char *cursor = (const unsigned char *)title;
    unsigned codes[30];
    uint8_t left[30], widths[30];
    int count = 0, width = 0;
    while (*cursor && count < 30) {
        unsigned code = glyph(&cursor) - 32;
        unsigned mask = 0;
        for (int i = 0; i < 16; i++) mask |= letras[code][i];
        int first = 0, last = 7;
        if (mask) {
            while (!(mask & (128 >> first))) first++;
            while (!(mask & (128 >> last))) last--;
        } else {
            last = 2;
        }
        int advance = (last - first + 2) * scale;
        if (width + advance > available - (*cursor ? 18 : 0)) {
            for (int i = 0; i < 3; i++) box((WIDTH - width - 18) / 2 + width + i * 6, y + 21, 3, 3, TEXT);
            width += 18;
            break;
        }
        codes[count] = code;
        left[count] = first;
        widths[count++] = advance;
        width += advance;
    }
    int x = (WIDTH - width) / 2;
    for (int i = 0; i < count; ++i) {
        unsigned bits = letras[codes[i]][dy / scale];
        for (int j = 0; j < widths[i] - scale; ++j) {
            if (bits & (128 >> (left[i] + j / scale))) pixel(x + j, TEXT);
        }
        x += widths[i];
    }
}

static void draw_cover(const player_screen_t *s)
{
    if (row < ART_Y || row >= ART_Y + ART_SIZE) return;
    int origin = (WIDTH - ART_SIZE) / 2;
    if (s->cover && s->cover_width && s->cover_height) {
        unsigned width = ART_SIZE, height = ART_SIZE;
        if (s->cover_width > s->cover_height) height = ART_SIZE * s->cover_height / s->cover_width;
        if (s->cover_height > s->cover_width) width = ART_SIZE * s->cover_width / s->cover_height;
        int y = row - ART_Y - (ART_SIZE - (int)height) / 2;
        if (y < 0 || y >= (int)height || !width) return;
        int x = (WIDTH - width) / 2;
        const uint8_t *src = s->cover + (y * s->cover_height / height) * s->cover_width * 2;
        for (unsigned i = 0; i < width; ++i) {
            unsigned j = (i * s->cover_width / width) * 2;
            line[2 * (x + i)] = src[j];
            line[2 * (x + i) + 1] = src[j + 1];
        }
        return;
    }
    box(origin, ART_Y, ART_SIZE, ART_SIZE, PANEL);
    box(origin, ART_Y, ART_SIZE, 1, BORDER);
    box(origin, ART_Y + ART_SIZE - 1, ART_SIZE, 1, BORDER);
    disk(120, 119, 34, BORDER, true);
    disk(120, 119, 22, BORDER, true);
    disk(120, 119, 8, s->connected ? GREEN : MUTED, false);
}

static void controls(const player_screen_t *s, const char *elapsed, const char *total)
{
    unsigned length = s->duration ? 200ULL * s->position / s->duration : 0;
    if (length > 200) length = 200;
    box(20, 245, 200, 3, BORDER);
    box(20, 245, (int)length, 3, GREEN);
    if (s->duration) disk(20 + (int)length, 246, 3, TEXT, false);
    text_at(20, 252, 1, 8, elapsed, MUTED);
    text_at(220 - 8 * (int)strlen(total), 252, 1, 8, total, MUTED);

    uint16_t key = s->connected ? TEXT : MUTED;
    disk(120, 293, 21, s->connected ? GREEN : BORDER, true);
    if (s->playing) {
        box(114, 286, 4, 15, key);
        box(122, 286, 4, 15, key);
    } else {
        arrow(116, 293, 1, key);
    }

    arrow(61, 293, -1, key);
    box(44, 286, 3, 15, key);
    arrow(177, 293, 1, key);
    box(193, 286, 3, 15, key);
}

static void time_text(char *dest, size_t size, uint32_t ms, bool valid)
{
    if (valid) snprintf(dest, size, "%u:%02u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60));
    else snprintf(dest, size, "--:--");
}

void pantalla_dibujar(const player_screen_t *s, bool completa)
{
    char elapsed[10], total[10];
    time_text(elapsed, sizeof(elapsed), s->position, s->duration != 0);
    time_text(total, sizeof(total), s->duration, s->duration != 0);
    int first = completa ? 0 : FIRST_CONTROL_ROW;
    command(0x2A, (uint8_t[]){0, 0, 0, WIDTH - 1}, 4);
    command(0x2B, (uint8_t[]){first >> 8, first & 255, 1, 63}, 4);
    command(0x2C, NULL, 0);

    for (int start = first; start < HEIGHT; start += STRIP) {
        int size = HEIGHT - start;
        if (size > STRIP) size = STRIP;
        for (int i = 0; i < size; ++i) {
            row = start + i;
            line = pixels[i];
            box(0, row, WIDTH, 1, BACK);
            if (row < FIRST_CONTROL_ROW) {
                text_at(16, 10, 1, 10, "FIAD MUSIC", TEXT);
                disk(151, 18, 2, s->connected ? GREEN : MUTED, false);
                text_at(158, 10, 1, 8, s->source, MUTED);
                box(16, 32, 208, 1, BORDER);
                draw_cover(s);
                title_at(s->title);
                text_at(-1, 228, 1, 28, s->artist, MUTED);
            } else {
                controls(s, elapsed, total);
            }
        }
        send_bytes(1, pixels, size * sizeof(pixels[0]));
    }
}
