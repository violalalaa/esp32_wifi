#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "wifi_manage.h"

static const char *TAG = "MY_WIFI"; 
static int s_retry_num = 0; //重试次数
static EventGroupHandle_t s_wifi_event_group; //事件组句柄

#define WIFI_CONNECTED_BIT BIT0 //连接成功位
#define WIFI_FAIL_BIT      BIT1 //连接失败位

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data); //事件处理回调

void wifi_init_sta(void)
 {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());   //初始化网络接口 启动tcp/ip协议栈
    ESP_ERROR_CHECK(esp_event_loop_create_default()); //创建默认事件循环
    esp_netif_create_default_wifi_sta(); //创建默认wifi station接口
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); //初始化wifi配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); //初始化wifi

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL)); //注册wifi事件处理回调
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL)); //注册ip事件处理回调

    wifi_config_t wifi_config = { //配置wifi参数
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config)); //设置wifi配置
    /* start 是异步的：驱动就绪后会发 WIFI_EVENT_STA_START，connect 放在回调里做 */
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,                    // 要等待的事件组句柄
      WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,    // 关心的位：连上 或 失败（任一即可）
      pdFALSE,                               // 返回时不清除这些位（pdTRUE 才会清）
      pdFALSE,                               // 不等“全部置位”，任一位置位就返回
      portMAX_DELAY);                        // 一直阻塞等到有结果（无限等待）
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);//连接成功
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);//连接失败
    } else {
        ESP_LOGI(TAG, "UNEXPECTED EVENT");//其他事件
    }
}
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) 
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); /* 第一次连接：驱动就绪后才 connect */
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

int wifi_get_retry_count(void) //获取重试次数
{
    return s_retry_num; //返回重试次数
}

bool wifi_get_ip(esp_netif_ip_info_t *ip) //获取IP地址
{
    if (!ip) {
        return false; //如果ip为空，返回false
    }
    /* WIFI_STA_DEF 是 esp_netif_create_default_wifi_sta() 创建的默认网卡名 */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); //获取网卡句柄
    if (!netif) {
        return false; //如果netif为空，返回false
    }
    if (esp_netif_get_ip_info(netif, ip) != ESP_OK) {
        return false; //如果获取ip失败，返回false
    }
    return ip->ip.addr != 0; //如果ip不为0，返回true    
}