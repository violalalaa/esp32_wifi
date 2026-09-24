idf_component_register(SRCS "wifi_manage.c" "main.c" "mqtt_manage.c" "ota_manage.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_driver_gpio mqtt
                                  esp_http_client esp_https_ota app_update
                    INCLUDE_DIRS ".")
