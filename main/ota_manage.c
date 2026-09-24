#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "ota_manage.h"

static const char *TAG = "OTA";

#define OTA_URL_SIZE 256
/* 容量 1：同一时间只接受一次升级请求 */
static QueueHandle_t s_ota_queue;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* 按队列里的 URL 下载到空闲 OTA 槽，成功才重启。失败继续等下一次 ota_request。 */
static void ota_task(void *arg)
{
    char url[OTA_URL_SIZE];
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_ota_queue, url, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * url 指向本函数栈上的数组，必须活到 esp_https_ota() 返回。
         * https:// 用乐鑫证书包；http:// 局域网文件服务器可以不带证书。
         */
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .keep_alive_enable = true,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
            .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        };
        esp_https_ota_config_t ota_config = {
            .http_config = &config,
        };

        ESP_LOGI(TAG, "download %s", config.url);
        esp_err_t ret = esp_https_ota(&ota_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA succeed, rebooting");
            esp_restart();
        }
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }
}

void ota_init(void)
{
    if (s_ota_queue) {
        return;
    }
    s_ota_queue = xQueueCreate(1, OTA_URL_SIZE);
    configASSERT(s_ota_queue);
    /* HTTPS 栈吃内存，给 8192 字节。优先级 5，不抢 WiFi/LwIP */
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "ota ready, waiting for url");
}

bool ota_request(const char *url)
{
    if (!s_ota_queue || !url || url[0] == '\0') {
        return false;
    }

    char buf[OTA_URL_SIZE];
    strncpy(buf, url, OTA_URL_SIZE - 1);
    buf[OTA_URL_SIZE - 1] = '\0';

    if (xQueueSend(s_ota_queue, buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "OTA busy, drop request");
        return false;
    }
    ESP_LOGI(TAG, "queued %s", buf);
    return true;
}
