#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t oled_display_init(void);
void oled_display_show_ip(void);
void oled_display_show_status(const char *status);
void oled_display_show_risk(const char *level, int people);
void oled_display_show_environment(bool dht_valid, float temperature_c,
                                   float humidity_pct, int mq2_adc);

#endif
