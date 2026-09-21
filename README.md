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

# ESP32 WiFi + MQTT 断线重连机制

> 模块说明：ESP32 WiFi 与 MQTT 连接管理  
> 核心原则：**WiFi 管连网，MQTT 管消息；掉线各自重连，应用只启动一次。**

## 整体关系

- WiFi 是底层连接，MQTT 跑在 TCP 上，TCP 跑在 IP 上。
- **必须先有 WiFi IP，才能连 MQTT Broker。**
- WiFi 和 MQTT 各自有内部重连机制，不需要额外编写 MQTT 重连代码。

```text
应用层   MQTT   CONNECT / CONNACK / PUBLISH / 你的回调
传输层   TCP    三次握手、断开
网络层   IP     WiFi DHCP 拿到的 192.168.x.x，才能出网
```

---

## WiFi 断线重连

### 触发事件

`WIFI_EVENT_STA_DISCONNECTED`

### 处理逻辑

```c
if (s_retry_num < MAXIMUM_RETRY) {
    esp_wifi_connect();   // 再次连接
    s_retry_num++;
    ESP_LOGI(TAG, "retry to connect to the AP");
} else {
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
}
```

### 关键点

- `MAXIMUM_RETRY` 通常为 5。
- **满 5 次还不行** → 置 `WIFI_FAIL_BIT`。
  - 这个 bit 只影响第一次 `wifi_wait_connected()` 的返回值。
  - 已经连上过之后，掉线仍会按计数再试。
- **拿到 IP 后**：`s_retry_num = 0`，计数清零。
  - 所以以后再掉线，又重新有 5 次重试机会。
- **达到最大重试后**：不再自动调用 `esp_wifi_connect()`，因此不会自动重连。
  - 热点恢复后，需要手动触发重连或重启设备才会重新连接。
  - 如果之前已经连上过，掉线后计数被清零，会重新有 5 次重试机会。

---

## MQTT 断线重连

> MQTT 内部自动重连，应用层不需要写重连代码。

### 触发流程

```text
TCP 断
  → MQTT 内部任务自动检测
  → 触发 MQTT_EVENT_DISCONNECTED
  → Broker 可能代发 LWT {"online":0}
```

### WiFi 恢复后

```text
WiFi 重新拿到 IP
  → MQTT 客户端内部自动：
     1. 重新 DNS（可能用缓存）
     2. 重新 TCP 三次握手
     3. 重新发 MQTT CONNECT（带 LWT）
     4. 收 CONNACK
     5. 触发 MQTT_EVENT_CONNECTED
  → 你在回调里再发 {"online":1}
```

### 关键：不要再调 `mqtt_app_start()`

- `mqtt_app_start()` 里有 `if (s_mqtt) return;` 保护。
- `s_mqtt` 句柄一直存在，内部会自动重连。
- 重复调用会尝试重新 init，产生多个客户端，浪费资源。

---

## 常见问题 Q&A

### Q1：WiFi 重试次数满了之后，热点恢复还会自动连吗？

**A1：** 不会自动连。达到最大重试后不再调用 `esp_wifi_connect()`，需要手动触发或重启。  
但如果之前已经连上过，`GOT_IP` 会把 `s_retry_num` 清零，掉线后会重新有 5 次机会。

### Q2：MQTT 掉线后需要我手动重连吗？

**A2：** 不需要。MQTT 客户端内部任务自动重连，你只需在 `MQTT_EVENT_CONNECTED` 里发在线状态即可。

### Q3：为什么不能在 `MQTT_EVENT_DISCONNECTED` 里调 `mqtt_app_start()`？

**A3：** 因为 `s_mqtt` 已存在，`mqtt_app_start()` 会直接 `return`，调了也没用；而且重复 init 会产生多个客户端，浪费资源。

### Q4：LWT 什么时候发？

**A4：** 连接时交给 Broker 保管。板子异常掉线（断电、断网）来不及自己发，Broker 代发 `{"online":0}`。  
正常断开（先发 DISCONNECT 再关 TCP）不会触发 LWT。

### Q5：WiFi 的 `FAIL_BIT` 和 MQTT 有关系吗？

**A5：** 没有直接关系。`FAIL_BIT` 只用于第一次 `wifi_wait_connected()` 判断是否跳过 MQTT 启动。之后 WiFi 掉线重连不再影响 MQTT 启动逻辑。

---

## 关键代码位置

| 文件 | 关键内容 |
|---|---|
| `wifi_manage.c` | `event_handler`、`wifi_wait_connected`、`s_retry_num` |
| `mqtt_manage.c` | `mqtt_event_handler`、`mqtt_app_start`、`s_mqtt` |
| `main.c` | `app_main` 中 `wifi_wait_connected()` 后调 `mqtt_app_start()` |

---

## 总结

- **WiFi 断线**：自己重试，最多 5 次，拿到 IP 清零。
- **MQTT 断线**：内部自动重连，不要手动干预。
- **启动 MQTT**：只调一次 `mqtt_app_start()`。
- **LWT**：负责异常掉线通知 Broker 代发离线消息。
- **retain**：在线状态 `retain=1`，温度 `retain=0`。
- **回调原则**：只打日志、发短消息，不干重活，不 `vTaskDelay`。

---

> 记住一句话：  
> **WiFi 管连网，MQTT 管消息；掉线各自重连，应用只启动一次。**