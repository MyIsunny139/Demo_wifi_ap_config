idf_component_register(
    SRCS "environment_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES dht esp_adc esp_driver_gpio freertos oled_display video_stream
)
