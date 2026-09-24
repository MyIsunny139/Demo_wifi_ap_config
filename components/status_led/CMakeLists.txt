idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__led_strip freertos esp_driver_ledc esp_driver_gpio
)
