#include "status_led.h"

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

static const char *TAG = "status_led";
static led_strip_handle_t led_strip;
static QueueHandle_t risk_queue;
static bool initialized;

static void apply_output(status_led_risk_t risk, bool on)
{
    if (!on || risk == STATUS_LED_RISK_LOW) {
        led_strip_clear(led_strip);
        return;
    }

    if (risk == STATUS_LED_RISK_MEDIUM) {
        led_strip_set_pixel(led_strip, 0, 48, 32, 0);
    } else {
        led_strip_set_pixel(led_strip, 0, 64, 0, 0);
    }
    led_strip_refresh(led_strip);
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
            apply_output(risk, on);
        }

        const TickType_t now = xTaskGetTickCount();
        if (risk != STATUS_LED_RISK_LOW &&
            (now - last_command) >= pdMS_TO_TICKS(STATUS_LED_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Risk command timeout; LED cleared");
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

    risk_queue = xQueueCreate(1, sizeof(status_led_risk_t));
    if (!risk_queue) {
        led_strip_del(led_strip);
        led_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(status_led_task, "status_led", STATUS_LED_TASK_STACK, NULL, 3,
                                NULL, 1) != pdPASS) {
        vQueueDelete(risk_queue);
        risk_queue = NULL;
        led_strip_del(led_strip);
        led_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "WS2812 ready on GPIO%d", STATUS_LED_GPIO);
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
