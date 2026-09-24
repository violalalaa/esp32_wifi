#ifndef OTA_MANAGE_H
#define OTA_MANAGE_H

#include <stdbool.h>

/* 建队列和 ota_task。任务马上睡在队列上，不会自己下载。有 IP 之后由 main 调一次。 */
void ota_init(void);

/* 把固件 URL 丢进队列就返回。真正下载在 ota_task。队列只有 1 格，忙时这次请求丢掉。 */
bool ota_request(const char *url);

#endif
