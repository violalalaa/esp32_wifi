#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "mpu_manage.h"

static const char *TAG = "MPU";

#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_WHO_AM_I     0x75
#define MPU_WHO_AM_I_VAL     0x68
/* ±2g 是上电默认量程，16384 LSB = 1g */
#define MPU_ACCEL_LSB_PER_G  16384

static i2c_master_dev_handle_t s_mpu;

static esp_err_t mpu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_mpu, buf, sizeof(buf), 1000);
}

static esp_err_t mpu_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_mpu, &reg, 1, data, len, 1000);
}

/* 换成两位小数的整数，避免 printf 浮点。100 表示 1.00g */
static int raw_to_centi(int16_t raw)
{
    int32_t v = (int32_t)raw * 100;
    if (v >= 0) {
        return (int)((v + (MPU_ACCEL_LSB_PER_G / 2)) / MPU_ACCEL_LSB_PER_G);
    }
    return (int)((v - (MPU_ACCEL_LSB_PER_G / 2)) / MPU_ACCEL_LSB_PER_G);
}

/* 负号只放在整数部分，避免 -1.05 被打成 -1.-05 */
static int append_centi(char *dst, size_t n, int centi)
{
    int neg = centi < 0;
    int v = neg ? -centi : centi;
    return snprintf(dst, n, "%s%d.%02d", neg ? "-" : "", v / 100, v % 100);
}

esp_err_t mpu_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = MPU_SDA_GPIO,
        .scl_io_num = MPU_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_mpu);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t who = 0;
    err = mpu_read_reg(MPU_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK || who != MPU_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "whoami=0x%02x err=%s", who, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* 先醒一次确认能写寄存器，再睡下。醒着约 3.8mA，睡了才配得上 Light Sleep */
    err = mpu_write_reg(MPU_REG_PWR_MGMT_1, 0x00);
    if (err != ESP_OK) {
        return err;
    }
    err = mpu_write_reg(MPU_REG_PWR_MGMT_1, 0x40);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "ready addr=0x%02x", MPU_I2C_ADDR);
    return ESP_OK;
}

esp_err_t mpu_read_json(char *json, size_t len)
{
    if (s_mpu == NULL || json == NULL || len < 48) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = mpu_write_reg(MPU_REG_PWR_MGMT_1, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wake failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 醒来后等转换稳定，再读 6 字节加速度 */
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t raw[6];
    err = mpu_read_reg(MPU_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    mpu_write_reg(MPU_REG_PWR_MGMT_1, 0x40);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        return err;
    }

    char sx[16];
    char sy[16];
    char sz[16];
    append_centi(sx, sizeof(sx), raw_to_centi((int16_t)((raw[0] << 8) | raw[1])));
    append_centi(sy, sizeof(sy), raw_to_centi((int16_t)((raw[2] << 8) | raw[3])));
    append_centi(sz, sizeof(sz), raw_to_centi((int16_t)((raw[4] << 8) | raw[5])));
    int n = snprintf(json, len, "{\"ax\": %s, \"ay\": %s, \"az\": %s}", sx, sy, sz);
    if (n < 0 || (size_t)n >= len) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "imu %s", json);
    return ESP_OK;
}
