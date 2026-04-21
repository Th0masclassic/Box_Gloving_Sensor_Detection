#include "magnometer_driver.h"

static const char *TAG = "MAG";
static i2c_master_dev_handle_t mag_dev_handle = NULL;
extern i2c_master_bus_handle_t bus_handle;

esp_err_t mag_init(void) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HMC5883L_ADDR,
        .scl_speed_hz = 100000,
    };

    // Adiciona o magnetometro ao barramento I2C.
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &mag_dev_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Magnetometro a configurar");

    // Configura a media e a taxa de amostragem.
    ret = i2c_register_write_byte(mag_dev_handle, 0x00, 0x70);
    if (ret != ESP_OK) return ret;

    // Define o ganho.
    ret = i2c_register_write_byte(mag_dev_handle, 0x01, 0x20);
    if (ret != ESP_OK) return ret;

    // Ativa o modo continuo.
    ret = i2c_register_write_byte(mag_dev_handle, 0x02, 0x00);
    if (ret != ESP_OK) return ret;

    // Espera a primeira leitura estabilizar.
    vTaskDelay(pdMS_TO_TICKS(100));

    return ret;
}

esp_err_t mag_get_real_data(mag_data_t *data) {
    if (mag_dev_handle == NULL) return ESP_FAIL;

    uint8_t raw_data[6] = {0};

    // Le os 6 bytes de dados.
    esp_err_t err = i2c_register_read(mag_dev_handle, 0x03, raw_data, 6);
    if (err != ESP_OK) {
        return err;
    }

    // Ordem dos eixos: X, Z, Y.
    int16_t x = (raw_data[0] << 8) | raw_data[1];
    int16_t z = (raw_data[2] << 8) | raw_data[3];
    int16_t y = (raw_data[4] << 8) | raw_data[5];

    // Converte para Gauss.
    data->x = (float)x / 1090.0;
    data->y = (float)y / 1090.0;
    data->z = (float)z / 1090.0;

    return ESP_OK;
}
