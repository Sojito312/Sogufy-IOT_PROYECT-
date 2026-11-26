#include "audio_aux.h"
#include <string.h>
#include "driver/dac_continuous.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum { BLOCK = 256 };
typedef struct {
    unsigned rate;
    unsigned count;
    uint8_t samples[BLOCK];
} block_t;

static QueueHandle_t pending;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned sample_rate = 44100;
static unsigned source_channels = 2;
static dac_continuous_handle_t dac;
static unsigned output_rate;
static adc_oneshot_unit_handle_t volume_adc;

static void init_volume(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &volume_adc));
    // GPIO34 is ADC1 channel 6 on the ESP32.
    ESP_ERROR_CHECK(adc_oneshot_config_channel(volume_adc, ADC_CHANNEL_6, &channel));
}

static void output_at(unsigned rate)
{
    if (output_rate == rate) return;
    if (dac) {
        ESP_ERROR_CHECK(dac_continuous_disable(dac));
        ESP_ERROR_CHECK(dac_continuous_del_channels(dac));
    }
    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0, .freq_hz = rate,
        .desc_num = 4, .buf_size = 512, .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL
    };
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cfg, &dac));
    ESP_ERROR_CHECK(dac_continuous_enable(dac));
    output_rate = rate;
}

static void play(uint8_t *samples, size_t length)
{
    size_t written;
    while (length) {
        ESP_ERROR_CHECK(dac_continuous_write(dac, samples, length, &written, -1));
        if (!written) return;
        samples += written;
        length -= written;
    }
}

static void output_task(void *arg)
{
    block_t block;
    uint8_t silence[BLOCK];
    memset(silence,128,sizeof(silence));
    output_at(44100);
    // Quiet triangular test tone at 441 Hz, followed by PCM from Bluetooth.
    uint8_t tone[200];
    for (unsigned i=0; i<sizeof(tone); ++i) {
        int phase=i%100;
        tone[i] = 116+(phase<50 ? phase : 100-phase)/2;
    }
    for (unsigned i=0; i<65; ++i) play(tone,sizeof(tone));
    int filtered = -1;
    for (;;) {
        int raw;
        if (adc_oneshot_read(volume_adc, ADC_CHANNEL_6, &raw) == ESP_OK) {
            filtered = filtered < 0 ? raw : (filtered * 7 + raw + 4) / 8;
        }
        int gain = filtered <= 96 ? 0 : (filtered - 96) * 256 / (4000 - 96);
        if (gain > 256) gain = 256;
        if (xQueueReceive(pending,&block,0)==pdTRUE) {
            output_at(block.rate);
            for (unsigned i = 0; i < block.count; ++i) {
                int sample = block.samples[i] - 128;
                block.samples[i] = (uint8_t)(128 + sample * gain / 256);
            }
            play(block.samples,block.count);
        } else {
            play(silence,sizeof(silence));
        }
    }
}

void aux_init(void)
{
    init_volume();
    pending=xQueueCreate(32,sizeof(block_t));
    ESP_ERROR_CHECK(pending ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t ok=xTaskCreate(output_task,"aux_dac",3072,NULL,5,NULL);
    ESP_ERROR_CHECK(ok==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void aux_format(unsigned rate, unsigned channels)
{
    portENTER_CRITICAL(&lock);
    sample_rate=rate;
    source_channels=channels;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI("AUX","PCM %u Hz, %u canal(es)",rate,channels);
}

void aux_pcm(const uint8_t *data, uint32_t bytes)
{
    block_t block;
    portENTER_CRITICAL(&lock);
    block.rate=sample_rate;
    unsigned channels=source_channels;
    portEXIT_CRITICAL(&lock);
    unsigned stride=2*channels;
    while (bytes>=stride) {
        block.count=0;
        while (bytes>=stride && block.count<BLOCK) {
            int32_t value=(int16_t)(data[0] | (data[1]<<8));
            if (channels==2) value=(value+(int16_t)(data[2] | (data[3]<<8)))/2;
            // Convert signed PCM to unsigned 8-bit mono for the AUX output.
            block.samples[block.count++]=(uint8_t)(128+value/256);
            data+=stride;
            bytes-=stride;
        }
        if (xQueueSend(pending,&block,0)!=pdTRUE) return;
    }
}
