#ifndef _BUTTON_H_
#define _BUTTON_H_
#include "esp_err.h"

//按键回调函数
typedef void(*button_press_cb_t)(int gpio);

//获取电平函数
typedef int(*button_getleve_cb_t)(int gpio);

//按键配置结构体
typedef struct
{
    int gpio_num;           //gpio号
    int active_level;       //按下的电平
    int long_press_time;    //长按时间
    button_getleve_cb_t getlevel_cb;  //获取电平的回调函数
    button_press_cb_t short_cb;   //短按回调函数
    button_press_cb_t long_cb;    //长按回调函数
}button_config_t;

/**
 * @brief 初始化按键组件
 * 
 * 创建并启动按键扫描定时器(5ms周期)
 * 可选调用,如果不调用,button_event_set会自动初始化
 * 
 * @return ESP_OK 初始化成功
 *         ESP_FAIL 初始化失败
 */
esp_err_t button_init(void);

/** 
 * @brief 注册按键事件
 * 
 * 添加一个按键到扫描列表,支持多个按键
 * 如果定时器未运行,会自动调用button_init初始化
 * 
 * @param cfg 按键配置结构体指针
 * @return ESP_OK 注册成功
 *         ESP_FAIL 注册失败(内存不足等)
 */
esp_err_t button_event_set(button_config_t *cfg);


#endif
