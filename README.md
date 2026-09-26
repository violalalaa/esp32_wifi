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

## MQTT 触发 OTA 笔记

板子打印 `Version 2.0` 才算升级成功。电脑上的源码、`build` 里的 bin、芯片里正在跑的程序，是三份东西。

### 分区

- 必须有 `ota_0`、`ota_1`。只有一块 `factory` 时，串口报 `Passive OTA partition could not be found`。
- `menuconfig` → **Partition Table** → **Factory app, two OTA definitions**。
- 双 OTA 大约 3.1MB。Flash 还设成 2MB 时，分区表生成失败，固件根本烧不进去。改成 8MB：**Serial Flasher Config** → Flash size。
- `idf.py erase-flash` 在工程根目录执行即可。擦除只清空 Flash，不会改分区表。
- 擦空后反复复位会刷 `invalid header: 0xffffffff`，电脑一直叮咚。按住 BOOT，点 RST，松开 BOOT，再执行 `idf.py -p COM4 flash`。只按 RST 还会继续空转。
- 分区表示例：`factory` 在 `0x10000`，`ota_0` 在 `0x110000`，`ota_1` 在 `0x210000`，各 1MB。`otadata` 决定下次启动哪一块。不要随便执行 `partition-table-flash`。

### 版本怎么看

- `Version 1.0` 在 `app_main` 最开头。监视器中途打开会错过，按 RST 从头看。
- USB 烧录写的是当时编出来的 bin。想演示「1.0 升到 2.0」：先烧 1.0，再把日志改成 2.0，只 `idf.py build`，不要再 flash。
- 板子仍打印 1.0，只说明芯片里还是旧程序。`build\esp32_wifi.bin` 里已经可以是 2.0。

### HTTP 服务器

- 在 `build` 目录里执行 `python -m http.server 8070`。在工程根目录开，访问 `/esp32_wifi.bin` 会 404。
- 浏览器能下到文件再发给板子。网址形如 `http://192.168.x.x:8070/esp32_wifi.bin`。电脑和板子要在同一个热点。
- 不要删 bin。重新 build 会覆盖它。服务开着时，下一次下载拿到的就是新文件。
- 日志里的 `GET ... 200` 只表示文件传完了，不表示已经换固件。

### MQTT

- 收网址的事件是 `MQTT_EVENT_DATA`，不是 `HTTP_EVENT_ON_DATA`。
- `CONNECTED` 里订阅 `esp32/viola/ota`。断线重连后要再订。
- `topic` 和 `data` 都没有 `\0`，按 `topic_len`、`data_len` 拷贝后再 `ota_request()`。
- MQTTX 用 **Publish** 发纯文本网址，不要停在 Subscribe 页。
- 订阅完成（日志里有 `subscribe esp32/viola/ota`）之前发出去的消息，Broker 不保存，直接丢。所以要等这行出现再 Publish。多按几次，是因为总有一次落在订阅之后。
## OTA 失败回滚测试

新固件启动就崩溃时，板子会自己回到上一份能启动的程序。这次好固件是工厂分区里的 `Version 2.0`，坏固件写进旁边的 `ota_0`，原来的 2.0 没有被盖掉。

### 触发条件

- `menuconfig` → **Bootloader config** → 打开 **Enable app rollback support**。回滚逻辑在 bootloader 里，不在 `ota_manage.c`。打开之后要先 USB 烧一次还能启动的 2.0，bootloader 才会换上。坏固件只 `idf.py build`，不要 USB 烧，也不要 `erase-flash`。
- `app_main` 最开头打印 `版本 3.0 - BAD`，接着 `int *p = NULL; *p = 123;`。确认函数 `esp_ota_mark_app_valid_cancel_rollback()` 来不及调用。
- 打印后马上崩，USB 串口会在重启空档里丢掉这行。两边各 `vTaskDelay` 3 秒，监视器才看得清。

### 现象

- 板上 Broker 是 `mqtt://test.mosquitto.org`。MQTTX 要连这一台，端口 `1883`。主题 `esp32/viola/ota`，等日志出现 `subscribe esp32/viola/ota` 再 **Publish** 网址。
- 浏览器下载 bin 只发生在电脑上，串口不会动。板子收到后才有 `MQTT_EVENT_DATA`、`queued`、`download`，然后 `Writing to <ota_0> partition at offset 0x110000`。
- `OTA succeed, rebooting` 之后第一次启动进 `ota_0`：`版本 3.0 - BAD`，接着 `Guru Meditation Error`，`StoreProhibited`，`EXCVADDR: 0x00000000`，回溯在 `main.c` 的 `*p = 123`。然后 `Rebooting...`，`rst:0xc (RTC_SW_CPU_RST)`。
- 第二次启动时 bootloader 看到这块新程序仍是待确认，把 `otadata`（`0xd000`）改回工厂分区 `0x10000`。后面一直是 `Loaded app from partition at offset 0x10000` 和 `Version 2.0`。
- 3.0 的数据还在 `ota_0`（`0x110000`）里，只是不会再被选中。按 RST 看到的也是 2.0。

### 结论

OTA 失败时设备回到上一份能启动的固件，不会变砖。`esp_https_ota()` 只负责写入空闲分区并重启；选哪一块启动，由 bootloader 看 `otadata` 决定。