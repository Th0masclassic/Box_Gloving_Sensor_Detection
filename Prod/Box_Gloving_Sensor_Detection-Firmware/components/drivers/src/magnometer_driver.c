/**
 * @file magnometer_driver.c
 * @brief QMC5883L raw-field acquisition.
 */
#include "magnometer_driver.h"

static const char *TAG = "MAG";
static i2c_master_dev_handle_t mag_dev_handle = NULL;

#define QMC5883L_SET_RESET_PERIOD 0x01u
/* OSR=512, ODR=200 Hz, range +/-2 G, continuous mode. */
#define QMC5883L_CONTROL_1_VALUE 0x0Du

esp_err_t mag_init(void)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mag_dev_handle != NULL) {
        return ESP_OK;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMC5883L_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &mag_dev_handle);
    if (err != ESP_OK) {
        return err;
    }

    /* The QMC5883L ID is commonly 0xFF and is not a unique board identity. */
    uint8_t chip_id = 0u;
    err = i2c_register_read(mag_dev_handle, QMC5883L_REG_CHIP_ID, &chip_id, 1u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QMC5883L probe failed: %s", esp_err_to_name(err));
        goto failed;
    }

    err = i2c_register_write_byte(
        mag_dev_handle, QMC5883L_REG_SET_RESET, QMC5883L_SET_RESET_PERIOD);
    if (err == ESP_OK) {
        err = i2c_register_write_byte(
            mag_dev_handle, QMC5883L_REG_CONTROL_1, QMC5883L_CONTROL_1_VALUE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QMC5883L configuration failed: %s", esp_err_to_name(err));
        goto failed;
    }

    uint8_t control_1 = 0u;
    uint8_t set_reset = 0u;
    err = i2c_register_read(mag_dev_handle, QMC5883L_REG_CONTROL_1, &control_1, 1u);
    if (err == ESP_OK) {
        err = i2c_register_read(mag_dev_handle, QMC5883L_REG_SET_RESET, &set_reset, 1u);
    }
    if (err != ESP_OK || control_1 != QMC5883L_CONTROL_1_VALUE ||
        set_reset != QMC5883L_SET_RESET_PERIOD) {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        ESP_LOGE(TAG, "QMC5883L readback failed: ctl=%02X setreset=%02X",
                 control_1, set_reset);
        goto failed;
    }

    ESP_LOGI(TAG, "QMC5883L register id=0x%02X, %u Hz, +/-2 G", chip_id, QMC5883L_SAMPLE_RATE_HZ);
    return ESP_OK;

failed:
    (void)i2c_master_bus_rm_device(mag_dev_handle);
    mag_dev_handle = NULL;
    return err;
}

esp_err_t mag_get_status(uint8_t *status)
{
    if (status == NULL || mag_dev_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_register_read(mag_dev_handle, QMC5883L_REG_STATUS, status, 1u);
}

esp_err_t mag_get_raw_data(mag_raw_data_t *data)
{
    if (data == NULL || mag_dev_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[6] = {0};
    esp_err_t err = i2c_register_read(mag_dev_handle, QMC5883L_REG_DATA_X_LSB, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    data->x = (int16_t)(((uint16_t)raw[1] << 8u) | raw[0]);
    data->y = (int16_t)(((uint16_t)raw[3] << 8u) | raw[2]);
    data->z = (int16_t)(((uint16_t)raw[5] << 8u) | raw[4]);
    return ESP_OK;
}

esp_err_t mag_get_real_data(mag_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mag_raw_data_t raw = {0};
    esp_err_t err = mag_get_raw_data(&raw);
    if (err != ESP_OK) {
        return err;
    }

    data->x = (float)raw.x / QMC5883L_LSB_PER_GAUSS;
    data->y = (float)raw.y / QMC5883L_LSB_PER_GAUSS;
    data->z = (float)raw.z / QMC5883L_LSB_PER_GAUSS;
    return ESP_OK;
}
