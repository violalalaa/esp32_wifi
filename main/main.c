#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_manage.h"

static const char *TAG = "MAIN";

/*
 * ESP32-S3-DevKit 板载灯经常是 WS2812（GPIO48），不能当普通 GPIO 翻转。
 * 这里用 GPIO2 当普通 LED；没有灯就外接一颗，或改成你板上的推挽 LED 脚。
 */
#define LED_GPIO GPIO_NUM_2

/* 任务函数签名必须是 void (*)(void *)，和 STM32 CubeMX 生成的 StartXxxTask 一样 */
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
        /* ESP-IDF 默认 1 tick=10ms，必须用 pdMS_TO_TICKS，不要写 vTaskDelay(5000) */
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void sensor_task(void *arg)
{
    (void)arg;
    int fake_temp = 250; /* 25.0℃，用整数避免拉进浮点库 */
    for (;;) {
        fake_temp++;
        if (fake_temp > 300) {
            fake_temp = 250;
        }
        ESP_LOGI(TAG, "sensor: temp=%d.%d C", fake_temp / 10, fake_temp % 10);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void led_task(void *arg)
{
    (void)arg;
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    int level = 0;
    for (;;) {
        level = !level;
        gpio_set_level(LED_GPIO, level);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* 进 app_main 时 FreeRTOS 已经在跑，这里本身就是一个任务（不像 STM32 还要 osKernelStart） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta(); /* 内部 WaitBits，连上或失败后才往下创建任务 */

    /*
     * xTaskCreate 参数：函数, 名字, 栈字节数, 参数, 优先级, 句柄
     * 栈是字节不是 word；打日志的任务给大一点。优先级 5 低于 WiFi/LwIP 系统任务。
     */
    xTaskCreate(wifi_monitor_task, "wifi_mon", 4096, NULL, 5, NULL);
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 5, NULL);
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
}   
