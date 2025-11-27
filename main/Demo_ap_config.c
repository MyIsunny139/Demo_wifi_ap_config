#include "button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "main.h"
#include "nvs_flash.h"

#define LED_GPIO    GPIO_NUM_2      // LED 引脚
#define BUTTON_GPIO GPIO_NUM_14     // 按键引脚

static const char *TAG = "MAIN";
static bool led_state = false;     // LED 状态

// 获取按键电平
int get_level(int gpio) { 
    return gpio_get_level(gpio); 
}

// 短按回调: 切换 LED 状态
void short_press(int gpio) { 
    led_state = !led_state;                     // 反转状态
    gpio_set_level(LED_GPIO, led_state);        // 设置 LED
    ESP_LOGI(TAG, "短按 - LED %s", led_state ? "ON" : "OFF");
}

// 长按回调
void long_press(int gpio) { 
    ESP_LOGW(TAG, "长按 - 进入配网模式");
    ap_wifi_apcfg(true);  // 启动 AP 配网

}
void wifi_state_handle(WIFI_STATE state)
{
    if(state == WIFI_STATE_CONNECTED)
    {
        ESP_LOGI(TAG,"Wifi connected");
    }
    else if(state == WIFI_STATE_CONNECTED)
    {
        ESP_LOGI(TAG,"Wifi disconnected");
    }
}

void app_button_init(void)
{
    ESP_LOGI(TAG, "系统启动");
    
    // 初始化 LED GPIO (输出模式)
    gpio_config_t led_io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_io);
    gpio_set_level(LED_GPIO, 0);  // 初始状态: 关闭
    
    // 初始化按键 GPIO (输入模式)
    gpio_config_t btn_io = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_io);

    // 配置按键
    button_config_t btn = {
        .gpio_num = BUTTON_GPIO,
        .active_level = 0,
        .long_press_time = 3000,
        .getlevel_cb = get_level,
        .short_cb = short_press,
        .long_cb = long_press
    };
    
    // 注册按键
    button_event_set(&btn);
    
    ESP_LOGI(TAG, "按键初始化完成");
    ESP_LOGI(TAG, "短按 GPIO%d -> 切换 LED (GPIO%d)", BUTTON_GPIO, LED_GPIO);
}

void app_main(void) 
{
    ESP_ERROR_CHECK(nvs_flash_init());
    app_button_init();
    ap_wifi_init(wifi_state_handle);
    
    // 启动时自动进入配网模式
    ESP_LOGI(TAG, "启动 AP 配网模式...");
    ap_wifi_apcfg(true);
}