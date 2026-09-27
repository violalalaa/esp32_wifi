#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "wifi_manage.h"
#include "mqtt_manage.h"
#include "ota_manage.h"

static const char *TAG = "MAIN";

/*
 * ESP32-S3-DevKit 板载灯经常是 WS2812（GPIO48），不能当普通 GPIO 翻转。
 * 这里用 GPIO2 当普通 LED；没有灯就外接一颗，或改成你板上的推挽 LED 脚。
 */
#define LED_GPIO GPIO_NUM_2

/* 每 30 秒查一次 IP / 重连次数。周期短的话 CPU 刚要打盹又被叫醒 */
static void wifi_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        esp_netif_ip_info_t ip;
        int retry = wifi_get_retry_count();
        if (wifi_get_ip(&ip)) {
            ESP_LOGI(TAG, "wifi: ip=" IPSTR " retry=%d", IP2STR(&ip.ip), retry);
        } else {
            ESP_LOGI(TAG, "wifi: disconnected retry=%d", retry);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* 每 30 秒拼假 JSON，交给 mqtt_publish_status；MQTT 连没连上由那边判断 */
static void sensor_task(void *arg)
{
    (void)arg;
    char json[64];
    int fake_temp = 255; /* 25.5℃，整数避免浮点 */
    int fake_hum = 60;

    for (;;) {
        snprintf(json, sizeof(json), "{\"temp\": %d.%d, \"hum\": %d}",
                 fake_temp / 10, fake_temp % 10, fake_hum);
        /* 只发 JSON，不在这里碰 MQTT client；没连上时函数内部直接 return */
        mqtt_publish_status(json);
        fake_temp++;
        if (fake_temp > 300) {
            fake_temp = 250;
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* 每 30 秒翻转 GPIO。1 秒闪一次会把 Light Sleep 频繁打断 */
static void led_task(void *arg)
{
    (void)arg;
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    int level = 0;
    for (;;) {
        level = !level;
        gpio_set_level(LED_GPIO, level);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "版本 2.0 ");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /*
     * 这里只降频，先不要 Light Sleep。
     * 启动阶段 main 堵在等 IP，系统几乎空闲，立刻睡眠会打断 DHCP，
     * USB 串口也会断，日志就停在 wifi:pm start。
     */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    esp_log_level_set("sleep", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    /* 只搭栈、注册回调、esp_wifi_start()，马上返回；真正 connect 在 WiFi 回调里 */
    wifi_init_sta();
    /* 阻塞：等 GOT_IP（CONNECTED_BIT）或重试耗尽（FAIL_BIT） */
    if (wifi_wait_connected()) {
        /* 有 IP 之后才允许打盹。插着 USB 串口时，menuconfig 里的锁会阻止睡眠，否则监视器会断。 */
        pm_config.light_sleep_enable = true;
        ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
        ESP_LOGI(TAG, "light sleep enabled");
        /* 有 IP 才 start MQTT，且只这一次；之后断线由 MQTT 内部自己重连 */
        mqtt_app_start();
        /* 只挂起等待；真正下载要别处调用 ota_request(url) */
        ota_init();
    } else {
        ESP_LOGE(TAG, "wifi failed, skip mqtt");
    }

    /* 栈单位是字节；优先级 5 低于 WiFi/LwIP，避免抢协议栈 */
    xTaskCreate(wifi_monitor_task, "wifi_mon", 4096, NULL, 5, NULL);
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    /* 三个任务接着跑：断网时 WiFi 回调重连 AP，MQTT 内部重连 Broker */
}
