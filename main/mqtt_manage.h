#ifndef MQTT_MANAGE_H
#define MQTT_MANAGE_H

/* mqtt:// 明文 1883，不是 mqtts://。须和 MQTTX 填的 Broker 一致 */
#define MQTT_BROKER_URI   "mqtt://broker.emqx.io"
#define MQTT_STATUS_TOPIC "esp32/viola/status"

/* 有 IP 之后由 main 调用一次。start 是异步的，连没连上要看 MQTT_EVENT_CONNECTED */
void mqtt_app_start(void);

/* sensor_task 发 JSON 用；client 是本模块 static，外面不要直接碰 */
void mqtt_publish_status(const char *json);

#endif
