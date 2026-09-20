idf_component_register(SRCS "wifi_manage.c" "main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_driver_gpio
                    INCLUDE_DIRS ".")
