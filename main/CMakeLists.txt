idf_component_register(SRCS "main.c" "wifi_manager.c" "ws_server.c" "ap_wifi.c"
                    INCLUDE_DIRS ".")
spiffs_create_partition_image(html ../html FLASH_INPROJECT)