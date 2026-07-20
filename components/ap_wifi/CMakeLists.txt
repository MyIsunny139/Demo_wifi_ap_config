idf_component_register(
    SRCS "ap_wifi.c"
    INCLUDE_DIRS    "."
    REQUIRES esp_wifi esp_http_server lwip json spiffs nvs_flash esp_netif driver button wifi_manager ws_server
)