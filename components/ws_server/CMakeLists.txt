idf_component_register(SRCS "ws_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_wifi esp_event nvs_flash esp_netif esp_http_server
                    )
