#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "wifi_manage.h"

/*
 * 账号密码不再写死。存在 NVS 命名空间 wifi_cfg（键 ssid / pass）。
 * 没有记录就 SmartConfig（手机 EspTouch 广播）；有记录就直连。
 * main.c 不用改：配网成功后仍 esp_wifi_connect()，拿到 IP 时还是
 * 下面这个事件组的 WIFI_CONNECTED_BIT，wifi_wait_connected() 返回后 main 才起 MQTT。
 */
static const char *TAG = "MY_WIFI";
static int s_retry_num = 0; //重试次数
static EventGroupHandle_t s_wifi_event_group; //事件组句柄。main 在 wifi_wait_connected() 里等它
static bool s_have_saved_wifi; //本次启动时 NVS 里是否已有账号。为假时不要 connect，也不要计重试
static char s_ssid[33]; //多 1 字节放 '\0'，SSID 最长 32
static char s_pass[65]; //多 1 字节放 '\0'，密码最长 64

#define NVS_NS "wifi_cfg"

#define WIFI_CONNECTED_BIT BIT0 //连接成功位
#define WIFI_FAIL_BIT      BIT1 //连接失败位

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data); //事件处理回调

/* 读 wifi_cfg。没有命名空间或没有 ssid 时返回 false，调用方进入配网。 */
static bool wifi_load_cfg(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(s_pass);
    esp_err_t ssid_err = nvs_get_str(nvs, "ssid", s_ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs, "pass", s_pass, &pass_len);
    nvs_close(nvs);
    if (ssid_err != ESP_OK || s_ssid[0] == '\0') {
        s_ssid[0] = '\0';
        s_pass[0] = '\0';
        return false;
    }
    if (pass_err != ESP_OK) {
        s_pass[0] = '\0';
    }
    return true;
}

/* 必须 commit。nvs_set_str 只写缓存，不提交的话掉电后丢失，复位又会进配网。 */
static bool wifi_save_cfg(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(nvs, "ssid", s_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", s_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* 只把 s_ssid / s_pass 交给驱动，这里不 connect。连接放在 STA_START 或收到密码之后。 */
static void wifi_apply_sta_config(void)
{
    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_pass, sizeof(wifi_config.sta.password) - 1);
    esp_err_t err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());   //初始化网络接口 启动tcp/ip协议栈
    ESP_ERROR_CHECK(esp_event_loop_create_default()); //创建默认事件循环
    esp_netif_create_default_wifi_sta(); //创建默认wifi station接口
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); //初始化wifi配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); //初始化wifi

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    /* SC_EVENT 是 SmartConfig 的事件，和 WIFI_EVENT 一样进 event_handler，不另起任务 */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));

    s_have_saved_wifi = wifi_load_cfg();
    if (s_have_saved_wifi) {
        ESP_LOGI(TAG, "NVS读取到SSID，直连...");
        wifi_apply_sta_config();
    } else {
        ESP_LOGI(TAG, "NVS没有WiFi信息，启动SmartConfig配网...");
    }
    /* start 是异步的：驱动就绪后会发 WIFI_EVENT_STA_START，connect 或配网放在回调里做 */
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 配网和 DHCP 期间保持 WIFI_PS_NONE。MIN_MODEM 等到 GOT_IP 再开，避免还没拿到地址就进省电。 */
    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

bool wifi_wait_connected(void)
{
    /* GOT_IP / 失败由 event_handler 置位；main 在这里等，不要在 WiFi 回调里启动 MQTT */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", s_ssid);
        return true;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", s_ssid);
        return false;
    }
    ESP_LOGI(TAG, "UNEXPECTED EVENT");
    return false;
}
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) 
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_have_saved_wifi) {
            esp_wifi_connect(); /* 已有账号：驱动就绪后才 connect */
        } else {
            esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
            smartconfig_start_config_t sc_cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
            esp_err_t err = esp_smartconfig_start(&sc_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "smartconfig start failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 配网期间没有账号，不能拿空密码去重试，否则 5 次后会置 FAIL_BIT，main 跳过 MQTT */
        if (!s_have_saved_wifi) {
            return;
        }
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        /* 手机已发来账号。先落 Flash，再交给驱动去连。连上后靠 GOT_IP 通知 main，这里不置事件位 */
        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        memcpy(s_ssid, evt->ssid, sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = '\0';
        memcpy(s_pass, evt->password, sizeof(s_pass) - 1);
        s_pass[sizeof(s_pass) - 1] = '\0';
        ESP_LOGI(TAG, "got ssid:%s", s_ssid);
        if (!wifi_save_cfg()) {
            return;
        }
        s_have_saved_wifi = true;
        s_retry_num = 0;
        wifi_apply_sta_config();
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        esp_smartconfig_stop(); /* 手机收到应答后再停，App 上才会显示配网成功 */
        ESP_LOGI(TAG, "smartconfig stopped");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        /* 有地址后再按 DTIM 省电，MQTT 心跳还能过。空闲时 CPU 才进得了 Light Sleep。 */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_retry_num = 0;
        /* 直连和配网都走到这里。main 只等这一位，所以不用新的信号量 */
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