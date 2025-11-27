idf_component_register(SRCS "ap_wifi.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_wifi esp_event nvs_flash esp_netif main wifi_manager spiffs
                    )
