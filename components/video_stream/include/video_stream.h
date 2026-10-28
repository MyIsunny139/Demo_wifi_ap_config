#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t video_stream_init(void);
esp_err_t video_stream_start(void);
esp_err_t video_stream_stop(void);
bool video_stream_is_running(void);
esp_err_t video_stream_send_environment(bool dht_valid, float temperature_c,
                                         float humidity_pct, int mq2_adc,
                                         uint32_t sampled_at_ms);

#endif
