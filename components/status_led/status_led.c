#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdbool.h>

#define STATUS_LED_GPIO GPIO_NUM_21
#define ACTIVE_BUZZER_GPIO GPIO_NUM_47
#define STATUS_LED_TIMEOUT_MS 5000
#define MEDIUM_BUZZER_PERIOD_MS 1000
#define HIGH_BUZZER_PERIOD_MS 250
#define STATUS_LED_TASK_STACK 3072
#define STATUS_LED_TASK_PRIORITY 5
#define STATUS_LED_TASK_CORE 0

static const char *TAG = "status_led";
static QueueHandle_t risk_queue;
static bool initialized;

static void apply_led(status_led_risk_t risk)
{
    esp_err_t err = gpio_set_level(STATUS_LED_GPIO, risk != STATUS_LED_RISK_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED output update failed: %s", esp_err_to_name(err));
    }
}

static void apply_buzzer(bool level)
{
    esp_err_t err = gpio_set_level(ACTIVE_BUZZER_GPIO, level);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer output update failed: %s", esp_err_to_name(err));
    }
}

static void status_led_task(void *context)
{
    (void)context;
    status_led_risk_t risk = STATUS_LED_RISK_LOW;
    TickType_t last_command = xTaskGetTickCount();
    TickType_t last_buzzer_toggle = last_command;
    bool buzzer_level = false;

    apply_led(risk);
    apply_buzzer(false);

    while (true) {
        status_led_risk_t requested;
        if (xQueueReceive(risk_queue, &requested, pdMS_TO_TICKS(50)) == pdTRUE) {
            last_command = xTaskGetTickCount();
            if (requested != risk) {
                risk = requested;
                ESP_LOGI(TAG, "Risk output update: level=%d", risk);
                apply_led(risk);
            }
            buzzer_level = false;
            last_buzzer_toggle = last_command;
            apply_buzzer(false);
        }

        const TickType_t now = xTaskGetTickCount();
        if (risk != STATUS_LED_RISK_LOW &&
            (now - last_command) >= pdMS_TO_TICKS(STATUS_LED_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Risk command timeout; alarm outputs cleared");
            risk = STATUS_LED_RISK_LOW;
            apply_led(risk);
            buzzer_level = false;
            apply_buzzer(false);
            last_buzzer_toggle = now;
        }

        if (risk == STATUS_LED_RISK_LOW) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const uint32_t period_ms = risk == STATUS_LED_RISK_HIGH
                                       ? HIGH_BUZZER_PERIOD_MS
                                       : MEDIUM_BUZZER_PERIOD_MS;
        if ((now - last_buzzer_toggle) >= pdMS_TO_TICKS(period_ms / 2)) {
            buzzer_level = !buzzer_level;
            last_buzzer_toggle = now;
            apply_buzzer(buzzer_level);
        }
    }
}

esp_err_t status_led_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO) | (1ULL << ACTIVE_BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize alarm GPIOs: %s", esp_err_to_name(err));
        return err;
    }
    apply_led(STATUS_LED_RISK_LOW);
    apply_buzzer(false);

    risk_queue = xQueueCreate(1, sizeof(status_led_risk_t));
    if (!risk_queue) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(status_led_task, "status_led", STATUS_LED_TASK_STACK, NULL,
                                STATUS_LED_TASK_PRIORITY, NULL, STATUS_LED_TASK_CORE) != pdPASS) {
        vQueueDelete(risk_queue);
        risk_queue = NULL;
        apply_led(STATUS_LED_RISK_LOW);
        apply_buzzer(false);
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    ESP_LOGI(TAG, "Alarm outputs ready: LED GPIO%d, active buzzer GPIO%d",
             STATUS_LED_GPIO, ACTIVE_BUZZER_GPIO);
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
