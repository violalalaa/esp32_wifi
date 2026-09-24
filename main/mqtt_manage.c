#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "mqtt_manage.h"
#include "ota_manage.h"

static const char *TAG = "MQTT_MANAGER";
/* 客户端句柄：init 成功后一直复用。WiFi 再 GOT_IP 也不要重新 init */
static esp_mqtt_client_handle_t s_mqtt = NULL;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * 跑在 MQTT 内部任务里，和 WiFi 的 event_handler 一样：只打日志、发短消息，不要 Delay。
 * 没有 pthread 去 recv，包来了就是这些 case。
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        /* TCP+MQTT 握手完成，这才算连上 Broker。掉线重连成功也会再进这里 */
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        /* retain=1：后打开 MQTTX 订阅也能立刻看到在线 */
        esp_mqtt_client_publish(client, MQTT_STATUS_TOPIC, "{\"online\":1}", 0, 1, 1);
        /* 断线重连后会话是干净的，必须再订一次，否则收不到升级 URL */
        esp_mqtt_client_subscribe(client, MQTT_OTA_TOPIC, 1);
        ESP_LOGI(TAG, "subscribe %s", MQTT_OTA_TOPIC);
        break;
    case MQTT_EVENT_DISCONNECTED:
        /* 只记录。重连由客户端内部做，不要在这里再调 mqtt_app_start() */
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_PUBLISHED:
        /* QoS1：Broker 确认收到了刚才那条 */
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        /* topic/data 都没有 '\\0'，长度分别是 topic_len、data_len */
        ESP_LOGI(TAG, "MQTT_EVENT_DATA TOPIC=%.*s DATA=%.*s",
                 event->topic_len, event->topic, event->data_len, event->data);
        if (event->topic_len == (int)strlen(MQTT_OTA_TOPIC) &&
            strncmp(event->topic, MQTT_OTA_TOPIC, event->topic_len) == 0 &&
            event->data_len > 0) {
            char url[256];
            int n = event->data_len;
            if (n >= (int)sizeof(url)) {
                n = sizeof(url) - 1;
            }
            memcpy(url, event->data, n);
            url[n] = '\0';
            /* xQueueSend 超时为 0，不堵住这条 MQTT 回调 */
            ota_request(url);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",
                                 event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void mqtt_app_start(void)
{
    if (s_mqtt) {
        return; /* 只 init/start 一次，WiFi 重连不要再调 */
    }

    /*
     * LWT（遗嘱）：连上时交给 Broker 保管。
     * 板子突然断电/掉网，来不及自己 publish，Broker 代发 {"online":0}。
     */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.last_will.topic = MQTT_STATUS_TOPIC,
        .session.last_will.msg = "{\"online\":0}",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    /* start 立即返回；内部任务去连 Broker，结果走上面的 CONNECTED/ERROR */
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "mqtt_app_start broker=%s", MQTT_BROKER_URI);
}

void mqtt_publish_status(const char *json)
{
    if (!s_mqtt || !json) {
        return; /* 还没 start，或 WiFi 第一次就失败跳过了 MQTT */
    }
    /* 第 4 个 0=按字符串算长度；QoS1；retain=0（状态点不要覆盖在线 retain） */
    int msg_id = esp_mqtt_client_publish(s_mqtt, MQTT_STATUS_TOPIC, json, 0, 1, 0);
    ESP_LOGI(TAG, "publish %s topic=%s msg_id=%d", json, MQTT_STATUS_TOPIC, msg_id);
}
