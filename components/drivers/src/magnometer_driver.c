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
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMC5883L_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &mag_dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_register_write_byte(mag_dev_handle, QMC5883L_REG_SET_RESET, QMC5883L_SET_RESET_PERIOD);
    if (ret != ESP_OK) return ret;

    ret = i2c_register_write_byte(mag_dev_handle, QMC5883L_REG_CONTROL_1, QMC5883L_CONTROL_1_VALUE);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Magnetometro inicializado com sucesso");
    return ESP_OK;
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
