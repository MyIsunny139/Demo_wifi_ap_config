#include "status_led.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"

#define STATUS_LED_GPIO 48
#define STATUS_LED_TIMEOUT_MS 5000
#define STATUS_LED_MEDIUM_BLINK_MS 500
#define STATUS_LED_HIGH_BLINK_MS 150
#define STATUS_LED_TASK_STACK 3072
#define STATUS_LED_TASK_PRIORITY 5
#define STATUS_LED_TASK_CORE 0
#define STATUS_LED_SELF_TEST_MS 300
#define BUZZER_SELF_TEST_MS 100
#define BUZZER_GPIO 14
#define BUZZER_FREQUENCY_HZ 2000
#define BUZZER_DUTY_RESOLUTION LEDC_TIMER_10_BIT
#define BUZZER_DUTY_ON 512
#define BUZZER_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1

static const char *TAG = "status_led";
static led_strip_handle_t led_strip;
static QueueHandle_t risk_queue;
static bool initialized;

static esp_err_t buzzer_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_DUTY_RESOLUTION,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = BUZZER_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,
    };
    return ledc_channel_config(&channel_config);
}

static esp_err_t buzzer_set_enabled(bool enabled)
{
    esp_err_t err = ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL,
                                  enabled ? BUZZER_DUTY_ON : 0);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void apply_output(status_led_risk_t risk, bool on)
{
    esp_err_t buzzer_err = buzzer_set_enabled(on && risk != STATUS_LED_RISK_LOW);
    if (buzzer_err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer update failed: %s", esp_err_to_name(buzzer_err));
    }

    esp_err_t err;
    if (!on || risk == STATUS_LED_RISK_LOW) {
        err = led_strip_clear(led_strip);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LED clear failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (risk == STATUS_LED_RISK_MEDIUM) {
        err = led_strip_set_pixel(led_strip, 0, 96, 64, 0);
    } else {
        err = led_strip_set_pixel(led_strip, 0, 128, 0, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED set failed: %s", esp_err_to_name(err));
        return;
    }

    err = led_strip_refresh(led_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED refresh failed: %s", esp_err_to_name(err));
    }
}

static void status_led_task(void *context)
{
    status_led_risk_t risk = STATUS_LED_RISK_LOW;
    TickType_t last_command = xTaskGetTickCount();
    TickType_t last_toggle = last_command;
    bool on = false;

    while (true) {
        status_led_risk_t requested;
        if (xQueueReceive(risk_queue, &requested, pdMS_TO_TICKS(50)) == pdTRUE) {
            risk = requested;
            last_command = xTaskGetTickCount();
            last_toggle = last_command;
            on = risk != STATUS_LED_RISK_LOW;
            ESP_LOGI(TAG, "Risk output update: level=%d", risk);
            apply_output(risk, on);
        }

        const TickType_t now = xTaskGetTickCount();
        if (risk != STATUS_LED_RISK_LOW &&
            (now - last_command) >= pdMS_TO_TICKS(STATUS_LED_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Risk command timeout; alarm outputs cleared");
            risk = STATUS_LED_RISK_LOW;
            on = false;
            apply_output(risk, false);
            continue;
        }

        const uint32_t interval_ms = risk == STATUS_LED_RISK_HIGH ? STATUS_LED_HIGH_BLINK_MS
                                                                  : STATUS_LED_MEDIUM_BLINK_MS;
        if (risk != STATUS_LED_RISK_LOW && (now - last_toggle) >= pdMS_TO_TICKS(interval_ms)) {
            last_toggle = now;
            on = !on;
            apply_output(risk, on);
        }
    }
}

esp_err_t status_led_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WS2812: %s", esp_err_to_name(err));
        return err;
    }

    err = buzzer_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize passive buzzer: %s", esp_err_to_name(err));
        led_strip_del(led_strip);
        led_strip = NULL;
        return err;
    }

    risk_queue = xQueueCreate(1, sizeof(status_led_risk_t));
    if (!risk_queue) {
        buzzer_set_enabled(false);
        led_strip_del(led_strip);
        led_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    led_strip_set_pixel(led_strip, 0, 0, 48, 0);
    led_strip_refresh(led_strip);
    err = buzzer_set_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer self-test start failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(BUZZER_SELF_TEST_MS));
    err = buzzer_set_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer self-test stop failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(STATUS_LED_SELF_TEST_MS - BUZZER_SELF_TEST_MS));
    led_strip_clear(led_strip);

    if (xTaskCreatePinnedToCore(status_led_task, "status_led", STATUS_LED_TASK_STACK, NULL,
                                STATUS_LED_TASK_PRIORITY, NULL, STATUS_LED_TASK_CORE) != pdPASS) {
        buzzer_set_enabled(false);
        vQueueDelete(risk_queue);
        risk_queue = NULL;
        led_strip_del(led_strip);
        led_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "WS2812 ready on GPIO%d; passive buzzer ready on GPIO%d at %d Hz",
             STATUS_LED_GPIO, BUZZER_GPIO, BUZZER_FREQUENCY_HZ);
    return ESP_OK;
}

esp_err_t status_led_set_risk(status_led_risk_t risk)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (risk < STATUS_LED_RISK_LOW || risk > STATUS_LED_RISK_HIGH) {
        return ESP_ERR_INVALID_ARG;
    }
    return xQueueOverwrite(risk_queue, &risk) == pdPASS ? ESP_OK : ESP_FAIL;
}

void status_led_clear(void)
{
    if (initialized) {
        status_led_risk_t risk = STATUS_LED_RISK_LOW;
        xQueueOverwrite(risk_queue, &risk);
    }
}
