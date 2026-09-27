#ifndef MPU_MANAGE_H
#define MPU_MANAGE_H

#include <stddef.h>
#include "driver/gpio.h"
#include "esp_err.h"

/* SDA=GPIO8，SCL=GPIO9。AD0 接 GND 时地址是 0x68，接到 3V3 改成 0x69 */
#define MPU_SDA_GPIO GPIO_NUM_8
#define MPU_SCL_GPIO GPIO_NUM_9
#define MPU_I2C_ADDR 0x68

/* 装 I2C 并核对 WHO_AM_I。失败不会重启整机，调用方自己决定要不要停任务 */
esp_err_t mpu_init(void);

/*
 * 醒一下，读三轴加速度，再睡回去。单位是 g，两位小数。
 * json 至少 48 字节。句柄在本模块 static，外面不要直接碰 I2C。
 */
esp_err_t mpu_read_json(char *json, size_t len);

#endif
