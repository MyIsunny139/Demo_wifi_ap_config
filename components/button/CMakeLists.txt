idf_component_register(SRCS "button.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_wifi esp_event nvs_flash esp_netif main wifi_manager
                    )
