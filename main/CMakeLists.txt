idf_component_register(SRCS "Demo_ap_config.c"
                    INCLUDE_DIRS "."
                    REQUIRES Init)
                    
spiffs_create_partition_image(html ../html FLASH_IN_PROJECT)
