#ifndef _BUTTON_H_
#define _BUTTON_H_

#include "esp_err.h"

typedef void (*button_press_cb_t)(int gpio);
typedef int (*button_getleve_cb_t)(int gpio);

typedef struct
{
    int gpio_num;
    int active_level;
    int long_press_time;
    button_getleve_cb_t getlevel_cb;
    button_press_cb_t short_cb;
    button_press_cb_t long_cb;
} button_config_t;

esp_err_t button_init(void);
esp_err_t button_event_set(button_config_t *cfg);

#endif
