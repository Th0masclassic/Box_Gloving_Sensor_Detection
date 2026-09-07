/**
 * @file giroscopio_driver.c
 * @brief ITG-3200 raw-rate acquisition.
 */
#include "giroscopio_driver.h"

#include "freertos/task.h"

static const char *TAG = "GYRO";
static i2c_master_dev_handle_t gyro_dev_handle = NULL;

esp_err_t giro_init(void)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gyro_dev_handle != NULL) {
        return ESP_OK;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ITG3200_ADDR_PRIMARY,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &gyro_dev_handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t who_am_i = 0u;
    err = i2c_register_read(gyro_dev_handle, ITG3200_REG_WHO_AM_I, &who_am_i, 1u);
    if (err != ESP_OK || (who_am_i & 0x7Eu) != ITG3200_WHO_AM_I_PRIMARY) {
        if (err == ESP_OK) {
            err = ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "ITG-3200 identity failed: 0x%02X (%s)",
                 who_am_i, esp_err_to_name(err));
        goto failed;
    }

    /* Select the x gyro PLL clock and wake the part. */
    err = i2c_register_write_byte(gyro_dev_handle, ITG3200_REG_PWR_MGM, 0x01u);
    if (err != ESP_OK) {
        goto failed;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 8 kHz internal rate / (SMPLRT_DIV + 1) = 800 Hz at 256 Hz DLPF. */
    err = i2c_register_write_byte(
        gyro_dev_handle, ITG3200_REG_DLPF_FS, ITG3200_DLPF_FS_2000DPS_256HZ);
    if (err == ESP_OK) {
        err = i2c_register_write_byte(
            gyro_dev_handle, ITG3200_REG_SMPLRT_DIV, ITG3200_SMPLRT_DIV_800HZ);
    }
    if (err == ESP_OK) {
        /* INT_STATUS RAW_RDY remains low unless its interrupt source is enabled. */
        err = i2c_register_write_byte(gyro_dev_handle, ITG3200_REG_INT_CFG, 0x01u);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ITG-3200 configuration failed: %s", esp_err_to_name(err));
        goto failed;
    }

    uint8_t dlpf_fs = 0u;
    uint8_t sample_div = 0u;
    uint8_t int_cfg = 0u;
    uint8_t pwr_mgm = 0u;
    err = i2c_register_read(gyro_dev_handle, ITG3200_REG_DLPF_FS, &dlpf_fs, 1u);
    if (err == ESP_OK) {
        err = i2c_register_read(gyro_dev_handle, ITG3200_REG_SMPLRT_DIV, &sample_div, 1u);
    }
    if (err == ESP_OK) {
        err = i2c_register_read(gyro_dev_handle, ITG3200_REG_INT_CFG, &int_cfg, 1u);
    }
    if (err == ESP_OK) {
        err = i2c_register_read(gyro_dev_handle, ITG3200_REG_PWR_MGM, &pwr_mgm, 1u);
    }
    if (err != ESP_OK || dlpf_fs != ITG3200_DLPF_FS_2000DPS_256HZ ||
        sample_div != ITG3200_SMPLRT_DIV_800HZ || (int_cfg & 0x01u) == 0u ||
        (pwr_mgm & 0x07u) != 0x01u) {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        ESP_LOGE(TAG, "ITG-3200 readback failed: dlpf=%02X div=%02X int=%02X pwr=%02X",
                 dlpf_fs, sample_div, int_cfg, pwr_mgm);
        goto failed;
    }

    ESP_LOGI(TAG, "ITG-3200 id=0x%02X, +/-2000 dps, nominal 800 Hz", who_am_i);
    return ESP_OK;

failed:
    (void)i2c_master_bus_rm_device(gyro_dev_handle);
    gyro_dev_handle = NULL;
    return err;
}

esp_err_t giro_get_raw_data(giro_raw_data_t *data, uint8_t *int_status)
{
    if (data == NULL || int_status == NULL || gyro_dev_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[ITG3200_GYRO_BURST_LEN] = {0};
    esp_err_t err = i2c_register_read(
        gyro_dev_handle, ITG3200_GYRO_BURST_START, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    *int_status = raw[0];
    /* Bytes 1 and 2 are temperature; X/Y/Z begin at byte 3. */
    data->x = (int16_t)(((uint16_t)raw[3] << 8u) | raw[4]);
    data->y = (int16_t)(((uint16_t)raw[5] << 8u) | raw[6]);
    data->z = (int16_t)(((uint16_t)raw[7] << 8u) | raw[8]);
    return ESP_OK;
}

esp_err_t giro_get_real_data(giro_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    giro_raw_data_t raw = {0};
    uint8_t status = 0u;
    esp_err_t err = giro_get_raw_data(&raw, &status);
    if (err != ESP_OK) {
        return err;
    }

    data->x = (float)raw.x / ITG3200_LSB_PER_DPS;
    data->y = (float)raw.y / ITG3200_LSB_PER_DPS;
    data->z = (float)raw.z / ITG3200_LSB_PER_DPS;
    return ESP_OK;
}
