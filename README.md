# ESP32-S3 IoT Terminal

## 🔧 硬件环境

- 主控：ESP32-S3 开发板
- 开发环境：VSCode + ESP-IDF v5.5.5
- 连接方式：2.4GHz WiFi（手机热点/路由器）

## 📈 当前进度

- [x] 复制 `wifi/getting_started/station` 示例并成功编译烧录
- [x] 成功连接 WiFi，获取 IP：`192.168.xxx.xx`
- [x] 验证断线重连：路由器断电恢复，观察到 `WIFI_EVENT_STA_DISCONNECTED` 和自动重连过程

## 📝 今日笔记

1. **事件循环干嘛**：esp_event_loop_create_default() 起一个后台任务，把 WiFi/IP 驱动抛出的事件排队，再按注册关系逐个调用 event_handler，让连接过程变成“来事件再处理”，而不是在 wifi_init_sta 里死等
2. **回调为什么不能做重活**：event_handler 跑在这条事件循环任务里，卡住它，后面的 WIFI_EVENT / IP_EVENT 就发不出去，所以这里只该做 esp_wifi_connect()、改计数、置 event group 这种轻操作。
3. **重连在哪触发**：断开时驱动发 WIFI_EVENT_STA_DISCONNECTED，进 event_handler 后若 s_retry_num 还没到上限，就再调一次 esp_wifi_connect()，重连就是在这里触发的。

