#include "magnometer_driver.h"

#define QMC5883L_REG_DATA_X_LSB 0x00
#define QMC5883L_REG_CONTROL_1  0x09
#define QMC5883L_REG_SET_RESET  0x0B

#define QMC5883L_SET_RESET_PERIOD 0x01
#define QMC5883L_CONTROL_1_VALUE  0x0D
#define QMC5883L_LSB_PER_GAUSS   12000.0f

static const char *TAG = "MAG";
static i2c_master_dev_handle_t mag_dev_handle = NULL;
extern i2c_master_bus_handle_t bus_handle;

esp_err_t mag_init(void)
{
    ESP_LOGI(TAG, "A iniciar magnetometro");
    ESP_LOGI(TAG, "bus_handle=%p mag_dev_handle=%p",
             (void *)bus_handle,
             (void *)mag_dev_handle);

    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "bus_handle NULL");
        return ESP_ERR_INVALID_STATE;
    }

    if (mag_dev_handle != NULL) {
        ESP_LOGW(TAG, "Magnetometro ja estava adicionado ao bus");
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMC5883L_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(
        bus_handle,
        &dev_cfg,
        &mag_dev_handle
    );

    ESP_LOGI(TAG, "add_device: %s (0x%x)",
             esp_err_to_name(ret), ret);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_register_write_byte(
        mag_dev_handle,
        QMC5883L_REG_SET_RESET,
        QMC5883L_SET_RESET_PERIOD
    );

    ESP_LOGI(TAG, "write SET_RESET: %s (0x%x)",
             esp_err_to_name(ret), ret);

    if (ret != ESP_OK) {
        goto error;
    }

    ret = i2c_register_write_byte(
        mag_dev_handle,
        QMC5883L_REG_CONTROL_1,
        QMC5883L_CONTROL_1_VALUE
    );

    ESP_LOGI(TAG, "write CONTROL_1: %s (0x%x)",
             esp_err_to_name(ret), ret);

    if (ret != ESP_OK) {
        goto error;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Magnetometro inicializado com sucesso");
    return ESP_OK;

error:
    i2c_master_bus_rm_device(mag_dev_handle);
    mag_dev_handle = NULL;
    return ret;
}

esp_err_t mag_get_real_data(mag_data_t *data)
{
    if (data == NULL || mag_dev_handle == NULL) {
        return ESP_FAIL;
    }

    uint8_t raw_data[6] = {0};
    esp_err_t err = i2c_register_read(mag_dev_handle, QMC5883L_REG_DATA_X_LSB, raw_data, 6);
    if (err != ESP_OK) {
        return err;
    }

    int16_t x = (int16_t)((raw_data[1] << 8) | raw_data[0]);
    int16_t y = (int16_t)((raw_data[3] << 8) | raw_data[2]);
    int16_t z = (int16_t)((raw_data[5] << 8) | raw_data[4]);

    data->x = (float)x / QMC5883L_LSB_PER_GAUSS;
    data->y = (float)y / QMC5883L_LSB_PER_GAUSS;
    data->z = (float)z / QMC5883L_LSB_PER_GAUSS;

    return ESP_OK;
}
