idf_component_register(SRCS "Init.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_common
                    PRIV_REQUIRES ap_wifi camera_service status_led oled_display video_stream environment_sensor nvs_flash esp_psram spi_flash heap esp_hw_support)
