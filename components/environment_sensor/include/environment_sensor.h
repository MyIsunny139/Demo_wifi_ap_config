#ifndef ENVIRONMENT_SENSOR_H
#define ENVIRONMENT_SENSOR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool dht_valid;
    float temperature_c;
    float humidity_pct;
    int mq2_adc;
    uint32_t sampled_at_ms;
} environment_reading_t;

esp_err_t environment_sensor_init(void);

#endif
