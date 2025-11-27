#include "button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
static const char* TAG = "button";

typedef enum
{
    BUTTON_RELEASE,             //按键没有按下
    BUTTON_PRESS,               //按键按下了，等待一点延时（消抖），然后触发短按回调事件，进入BUTTON_HOLD
    BUTTON_HOLD,                //按住状态，如果时间长度超过设定的超时计数，将触发长按回调函数，进入BUTTON_LONG_PRESS_HOLD
    BUTTON_LONG_PRESS_HOLD,     //此状态等待电平消失，回到BUTTON_RELEASE状态
}BUTTON_STATE;

typedef struct Button
{
    button_config_t btn_cfg;    //按键配置
    BUTTON_STATE    state;      //当前状态
    int press_cnt;              //按下计数
    struct Button* next;        //下一个按键参数
}button_dev_t;

//按键处理列表
static button_dev_t *s_button_head = NULL;

//消抖过滤时间
#define FILITER_TIMER   20

//定时器释放运行标志
static bool g_is_timer_running = false;

//定时器句柄
static esp_timer_handle_t g_button_timer_handle;

//按键扫描周期(微秒)
#define BUTTON_SCAN_PERIOD_US   5000  // 5ms

static void button_handle(void *param);

/**
 * @brief 初始化按键组件
 * 
 * 创建并启动按键扫描定时器
 * 在使用 button_event_set 注册按键前,可以选择性调用此函数
 * 如果不调用,第一次调用 button_event_set 时会自动初始化
 * 
 * @return ESP_OK 初始化成功
 *         ESP_FAIL 初始化失败
 */
esp_err_t button_init(void)
{
    if (g_is_timer_running) {
        ESP_LOGW(TAG, "Button timer already running");
        return ESP_OK;
    }

    // 创建定时器配置
    esp_timer_create_args_t button_timer_args = {
        .callback = button_handle,
        .arg = (void*)5,  // 传入扫描周期(ms)
        .dispatch_method = ESP_TIMER_TASK,
        .name = "button_scan"
    };

    // 创建定时器
    esp_err_t ret = esp_timer_create(&button_timer_args, &g_button_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button timer: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    // 启动周期性定时器 (5ms)
    ret = esp_timer_start_periodic(g_button_timer_handle, BUTTON_SCAN_PERIOD_US);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start button timer: %s", esp_err_to_name(ret));
        esp_timer_delete(g_button_timer_handle);
        return ESP_FAIL;
    }

    g_is_timer_running = true;
    ESP_LOGI(TAG, "Button component initialized, scan period: %d ms", BUTTON_SCAN_PERIOD_US / 1000);
    
    return ESP_OK;
}

/** 设置按键事件
 * @param cfg   配置结构体
 * @return ESP_OK or ESP_FAIL 
*/
esp_err_t button_event_set(button_config_t *cfg)
{
    button_dev_t* btn = (button_dev_t*)malloc(sizeof(button_dev_t));
    if(!btn)
        return ESP_FAIL;
    memset(btn,0,sizeof(button_dev_t));
    if(!s_button_head)
    {
        s_button_head = btn;
    }
    else
    {
        button_dev_t* btn_p = s_button_head;
        while(btn_p->next != NULL)
            btn_p = btn_p->next;
        btn_p->next = btn;
    }
    memcpy(&btn->btn_cfg,cfg,sizeof(button_config_t));

    // 如果定时器还未运行,自动初始化
    if (false == g_is_timer_running) {
        esp_err_t ret = button_init();
        if (ret != ESP_OK) {
            free(btn);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Button registered: GPIO%d, active_level=%d, long_press=%dms", 
             cfg->gpio_num, cfg->active_level, cfg->long_press_time);

    return ESP_OK;
}

/** 定时器回调函数，本例中是5ms执行一次
 * @param cfg   配置结构体
 * @return ESP_OK or ESP_FAIL 
*/
static void button_handle(void *param)
{
    int increase_cnt = (int)param;  //传入的参数是5，表示定时器运行周期是5ms
    button_dev_t* btn_target = s_button_head;
    //遍历链表
    for(;btn_target;btn_target = btn_target->next)
    {
        int gpio_num = btn_target->btn_cfg.gpio_num;
        if(!btn_target->btn_cfg.getlevel_cb)
            continue;
        switch(btn_target->state)
        {
            case BUTTON_RELEASE:             //按键没有按下状态
                if(btn_target->btn_cfg.getlevel_cb(gpio_num) == btn_target->btn_cfg.active_level)
                {
                    btn_target->press_cnt += increase_cnt;
                    btn_target->state = BUTTON_PRESS;   //调转到按下状态
                }
                break;
            case BUTTON_PRESS:               //按键按下了，等待一点延时（消抖），然后触发短按回调事件，进入BUTTON_HOLD
                if(btn_target->btn_cfg.getlevel_cb(gpio_num) == btn_target->btn_cfg.active_level)
                {
                    btn_target->press_cnt += increase_cnt;
                    if(btn_target->press_cnt >= FILITER_TIMER)  //过了滤波时间，执行短按回调函数
                    {
                        if(btn_target->btn_cfg.short_cb)
                            btn_target->btn_cfg.short_cb(gpio_num);
                        btn_target->state = BUTTON_HOLD;    //状态转入按下状态
                    }
                }
                else
                {
                    btn_target->state = BUTTON_RELEASE;
                    btn_target->press_cnt = 0;
                }
                break;
            case BUTTON_HOLD:                //按住状态，如果时间长度超过设定的超时计数，将触发长按回调函数，进入BUTTON_LONG_PRESS_HOLD
                if(btn_target->btn_cfg.getlevel_cb(gpio_num) == btn_target->btn_cfg.active_level)
                {
                    btn_target->press_cnt += increase_cnt;
                    if(btn_target->press_cnt >= btn_target->btn_cfg.long_press_time)  //已经检测到按下大于预设长按时间,执行长按回调函数
                    {
                        if(btn_target->btn_cfg.long_cb)
                            btn_target->btn_cfg.long_cb(gpio_num);
                        btn_target->state = BUTTON_LONG_PRESS_HOLD;
                    }
                }
                else
                {
                    btn_target->state = BUTTON_RELEASE;
                    btn_target->press_cnt = 0;
                }
                break;
            case BUTTON_LONG_PRESS_HOLD:     //此状态等待电平消失，回到BUTTON_RELEASE状态
                if(btn_target->btn_cfg.getlevel_cb(gpio_num) != btn_target->btn_cfg.active_level)    //检测到释放，就回到初始状态
                {
                    btn_target->state = BUTTON_RELEASE;
                    btn_target->press_cnt = 0;
                }
                break;
            default:break;
        }
    }
}
