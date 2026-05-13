#include "giroscopio_driver.h"

static const char *TAG = "GIRO";
static i2c_master_dev_handle_t giro_dev_handle = NULL;
extern i2c_master_bus_handle_t bus_handle;

esp_err_t giro_init(void){
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ITG3200_ADDR_PRIMARY,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &giro_dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    /*
    // Faz reset ao giroscopio (0x80).
    ret = i2c_register_write_byte(giro_dev_handle, ITG3200_REG_PWR_MGM, 0x80);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Sai do modo de reset (0x00).
    ret = i2c_register_write_byte(giro_dev_handle, ITG3200_REG_PWR_MGM, 0x00);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    // Define escala de +/-2000 graus/s e filtro a 42 Hz (0x1B).
    ret = i2c_register_write_byte(giro_dev_handle, ITG3200_REG_DLPF_FS, 0x1B);
    if (ret != ESP_OK) {
        return ret;
    }

    // Define a taxa de amostragem para 100 Hz (divisor = 9).
    ret = i2c_register_write_byte(giro_dev_handle, ITG3200_REG_SMPLRT_DIV, 9);
    if (ret != ESP_OK) {
        return ret;
    }
    */
    // Ativa a interrupcao de dados prontos (0x01).
    ret = i2c_register_write_byte(giro_dev_handle, 0x3E, 0x01);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ret = i2c_register_write_byte(giro_dev_handle, 0x16, 0x18);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "Giroscopio inicializado");

    return ESP_OK;
}

esp_err_t giro_get_real_data(giro_data_t *data)
{
    if (data == NULL || giro_dev_handle == NULL) {
        return ESP_FAIL;
    }

    uint8_t raw_data[6] = {0};
    esp_err_t ret = i2c_register_read(giro_dev_handle, ITG3200_REG_GYRO_XOUT_H, raw_data, 6);
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t x = (int16_t)((raw_data[0] << 8) | raw_data[1]);
    int16_t y = (int16_t)((raw_data[2] << 8) | raw_data[3]);
    int16_t z = (int16_t)((raw_data[4] << 8) | raw_data[5]);

    // Converte para graus por segundo.
    data->x = (float)x / 14.375f;
    data->y = (float)y / 14.375f;
    data->z = (float)z / 14.375f;

    return ESP_OK;
}
