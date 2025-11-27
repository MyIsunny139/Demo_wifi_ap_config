idf_component_register(SRCS "wifi_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_wifi esp_event nvs_flash esp_netif main ap_wifi spiffs
                    )
