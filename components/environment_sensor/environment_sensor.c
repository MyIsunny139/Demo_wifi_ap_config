#include "environment_sensor.h"

#include "dht.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "oled_display.h"
#include "video_stream.h"
#include <string.h>

#define DHT11_GPIO GPIO_NUM_42
#define MQ2_ADC_GPIO GPIO_NUM_14
#define MQ2_ADC_UNIT ADC_UNIT_2
#define MQ2_ADC_CHANNEL ADC_CHANNEL_3
#define SAMPLE_PERIOD_MS 2000
#define SENSOR_TASK_STACK 3072

static const char *TAG = "environment_sensor";
static adc_oneshot_unit_handle_t adc_handle;
static SemaphoreHandle_t reading_mutex;
static environment_reading_t latest_reading;
static bool initialized;

static void sample_task(void *context)
{
    (void)context;
    TickType_t wake_at = xTaskGetTickCount();

    while (true) {
        float humidity = 0.0f;
        float temperature = 0.0f;
        int mq2_adc = 0;
        const esp_err_t dht_err = dht_read_float_data(
            DHT_TYPE_DHT11, DHT11_GPIO, &humidity, &temperature);
        const esp_err_t mq2_err = adc_oneshot_read(
            adc_handle, MQ2_ADC_CHANNEL, &mq2_adc);
        const bool dht_valid = dht_err == ESP_OK;

        xSemaphoreTake(reading_mutex, portMAX_DELAY);
        latest_reading.dht_valid = dht_valid;
        if (dht_valid) {
            latest_reading.temperature_c = temperature;
            latest_reading.humidity_pct = humidity;
        }
        if (mq2_err == ESP_OK) {
            latest_reading.mq2_adc = mq2_adc;
        }
        latest_reading.sampled_at_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const environment_reading_t snapshot = latest_reading;
        xSemaphoreGive(reading_mutex);

        if (dht_err != ESP_OK) {
            ESP_LOGW(TAG, "DHT11 read failed: %s", esp_err_to_name(dht_err));
        }
        if (mq2_err != ESP_OK) {
            ESP_LOGW(TAG, "MQ-2 ADC read failed: %s", esp_err_to_name(mq2_err));
        }
        oled_display_show_environment(
            snapshot.dht_valid, snapshot.temperature_c,
            snapshot.humidity_pct, snapshot.mq2_adc);
        const esp_err_t stream_err = video_stream_send_environment(
            snapshot.dht_valid, snapshot.temperature_c, snapshot.humidity_pct,
            snapshot.mq2_adc, snapshot.sampled_at_ms);
        if (stream_err != ESP_OK && stream_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Environment telemetry send failed: %s",
                     esp_err_to_name(stream_err));
        }
        vTaskDelayUntil(&wake_at, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

esp_err_t environment_sensor_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = MQ2_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &adc_handle);
    if (err != ESP_OK) {
        return err;
    }
    const adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(adc_handle, MQ2_ADC_CHANNEL, &channel_config);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return err;
    }

    reading_mutex = xSemaphoreCreateMutex();
    if (!reading_mutex) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(&latest_reading, 0, sizeof(latest_reading));
    if (xTaskCreatePinnedToCore(sample_task, "environment_sensor",
                                SENSOR_TASK_STACK, NULL, 3, NULL, 1) != pdPASS) {
        vSemaphoreDelete(reading_mutex);
        reading_mutex = NULL;
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    ESP_LOGI(TAG, "DHT11 GPIO%d, MQ-2 ADC GPIO%d", DHT11_GPIO, MQ2_ADC_GPIO);
    return ESP_OK;
}

