#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "esp_err.h"

typedef enum {
    STATUS_LED_RISK_LOW = 0,
    STATUS_LED_RISK_MEDIUM,
    STATUS_LED_RISK_HIGH,
} status_led_risk_t;

esp_err_t status_led_init(void);
esp_err_t status_led_set_risk(status_led_risk_t risk);
void status_led_clear(void);

#endif
