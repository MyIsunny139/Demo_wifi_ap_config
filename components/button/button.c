#include "button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "button";

typedef enum
{
    BUTTON_RELEASE,
    BUTTON_PRESS,
    BUTTON_HOLD,
    BUTTON_LONG_PRESS_HOLD,
} BUTTON_STATE;

typedef struct Button
{
    button_config_t btn_cfg;
    BUTTON_STATE state;
    int press_cnt;
    struct Button *next;
} button_dev_t;

static button_dev_t *s_button_head = NULL;

#define FILTER_TIMER_MS 20
#define BUTTON_SCAN_PERIOD_US 5000

static bool g_is_timer_running = false;
static esp_timer_handle_t g_button_timer_handle;

static void button_handle(void *param);

esp_err_t button_init(void)
{
    if (g_is_timer_running) {
        ESP_LOGW(TAG, "Button timer already running");
        return ESP_OK;
    }

    esp_timer_create_args_t button_timer_args = {
        .callback = button_handle,
        .arg = (void *)5,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "button_scan",
    };

    esp_err_t ret = esp_timer_create(&button_timer_args, &g_button_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button timer: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = esp_timer_start_periodic(g_button_timer_handle, BUTTON_SCAN_PERIOD_US);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start button timer: %s", esp_err_to_name(ret));
        esp_timer_delete(g_button_timer_handle);
        g_button_timer_handle = NULL;
        return ESP_FAIL;
    }

    g_is_timer_running = true;
    ESP_LOGI(TAG, "Button component initialized, scan period: %d ms", BUTTON_SCAN_PERIOD_US / 1000);
    return ESP_OK;
}

esp_err_t button_event_set(button_config_t *cfg)
{
    if (!cfg || !cfg->getlevel_cb) {
        return ESP_FAIL;
    }

    button_dev_t *btn = calloc(1, sizeof(button_dev_t));
    if (!btn) {
        return ESP_FAIL;
    }

    memcpy(&btn->btn_cfg, cfg, sizeof(button_config_t));

    if (!s_button_head) {
        s_button_head = btn;
    } else {
        button_dev_t *btn_p = s_button_head;
        while (btn_p->next != NULL) {
            btn_p = btn_p->next;
        }
        btn_p->next = btn;
    }

    if (!g_is_timer_running) {
        esp_err_t ret = button_init();
        if (ret != ESP_OK) {
            if (s_button_head == btn) {
                s_button_head = btn->next;
            }
            free(btn);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Button registered: GPIO%d, active_level=%d, long_press=%dms",
             cfg->gpio_num, cfg->active_level, cfg->long_press_time);
    return ESP_OK;
}

static void button_handle(void *param)
{
    int increase_cnt = (int)(intptr_t)param;

    for (button_dev_t *btn_target = s_button_head; btn_target; btn_target = btn_target->next) {
        int gpio_num = btn_target->btn_cfg.gpio_num;
        if (!btn_target->btn_cfg.getlevel_cb) {
            continue;
        }

        bool pressed = btn_target->btn_cfg.getlevel_cb(gpio_num) == btn_target->btn_cfg.active_level;

        switch (btn_target->state) {
        case BUTTON_RELEASE:
            if (pressed) {
                btn_target->press_cnt += increase_cnt;
                btn_target->state = BUTTON_PRESS;
            }
            break;

        case BUTTON_PRESS:
            if (pressed) {
                btn_target->press_cnt += increase_cnt;
                if (btn_target->press_cnt >= FILTER_TIMER_MS) {
                    if (btn_target->btn_cfg.short_cb) {
                        btn_target->btn_cfg.short_cb(gpio_num);
                    }
                    btn_target->state = BUTTON_HOLD;
                }
            } else {
                btn_target->state = BUTTON_RELEASE;
                btn_target->press_cnt = 0;
            }
            break;

        case BUTTON_HOLD:
            if (pressed) {
                btn_target->press_cnt += increase_cnt;
                if (btn_target->press_cnt >= btn_target->btn_cfg.long_press_time) {
                    if (btn_target->btn_cfg.long_cb) {
                        btn_target->btn_cfg.long_cb(gpio_num);
                    }
                    btn_target->state = BUTTON_LONG_PRESS_HOLD;
                }
            } else {
                btn_target->state = BUTTON_RELEASE;
                btn_target->press_cnt = 0;
            }
            break;

        case BUTTON_LONG_PRESS_HOLD:
            if (!pressed) {
                btn_target->state = BUTTON_RELEASE;
                btn_target->press_cnt = 0;
            }
            break;

        default:
            btn_target->state = BUTTON_RELEASE;
            btn_target->press_cnt = 0;
            break;
        }
    }
}
