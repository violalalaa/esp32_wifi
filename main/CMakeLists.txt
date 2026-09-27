idf_component_register(SRCS "mpu_manage.c" "wifi_manage.c" "main.c" "mqtt_manage.c" "ota_manage.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_driver_gpio esp_pm mqtt
                                  esp_http_client esp_https_ota app_update esp_driver_i2c
                    INCLUDE_DIRS ".")
