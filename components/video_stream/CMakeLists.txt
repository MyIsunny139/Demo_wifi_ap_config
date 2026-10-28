idf_component_register(
    SRCS "video_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES camera_service status_led oled_display esp_http_server esp_wifi json freertos
)
