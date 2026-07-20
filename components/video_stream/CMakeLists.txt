idf_component_register(
    SRCS "video_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES camera_service status_led esp_http_server json freertos
)
