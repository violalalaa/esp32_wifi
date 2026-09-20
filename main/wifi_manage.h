#ifndef WIFI_MANAGE_H
#define WIFI_MANAGE_H

#include <stdbool.h>
#include "esp_netif.h"

#define WIFI_SSID "viola"
#define WIFI_PASS "55555555"
#define MAXIMUM_RETRY 5

void wifi_init_sta(void);

/* 给其它任务查询用：s_retry_num 是 wifi_manage.c 里的 static，外面不能直接读 */
int wifi_get_retry_count(void);

/* 读到有效 IP 返回 true；未连接或 DHCP 还没拿到则 false */

bool wifi_get_ip(esp_netif_ip_info_t *ip);
/* 这是函数声明，不是函数体。实现写在 wifi_manage.c 里。
 * bool：返回值。拿到有效 IP 为 true，没连上或 DHCP 还没分配则为 false。
 * esp_netif_ip_info_t：ESP-IDF 的结构体（定义在已包含的 esp_netif.h），
 *   里面包含三个 IPv4 地址：
 *   ip（本机地址）、netmask（子网掩码）、gw（网关）。
 *  *ip：输出参数。调用者自己准备一个 esp_netif_ip_info_t，把地址传进来，
 *   函数把读到的地址填进这个结构体，所以用指针而不是按值返回。 */

#endif
