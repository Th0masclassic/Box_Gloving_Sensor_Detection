#include "accelerometer_driver.h"

static const char *TAG = "ACCEL";

static i2c_master_dev_handle_t accel_dev_handle = NULL; // Handle do acelerometro.
extern i2c_master_bus_handle_t bus_handle;

void accel_read_bytes(uint8_t *data, size_t len) {
    if (data == NULL || len < ADXL345_SAMPLE_LEN) return;
    i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, data, ADXL345_SAMPLE_LEN);
}

uint8_t accel_get_int_source(void) {
    uint8_t int_source = 0;
    if (accel_dev_handle != NULL) {
        i2c_register_read(accel_dev_handle, 0x30, &int_source, 1); // Le o registo INT_SOURCE.
    }
    return int_source;
}

esp_err_t accel_init(){

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACCEL_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    // Adiciona o acelerometro ao barramento I2C.
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &accel_dev_handle);
    
    if (ret == ESP_OK) {
        // Entra em standby para configurar.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x00);
        
        // Configura os registos principais.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_BW_RATE, 0x0A); // Taxa de 100 Hz.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_DATA_FORMAT, 0x00); // Faixa de +/-2 g.
        i2c_register_write_byte(accel_dev_handle, 0x38, 0x00); // FIFO em bypass.
        
        // Ativa o modo de medicao.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x08);
        
        ESP_LOGI(TAG, "Accelerometer initialized successfully (POLLING MODE)");
    }
    return ret;
}

void accel_get_real_data(accel_data_t *accel_data) {
    uint8_t raw_data[ADXL345_SAMPLE_LEN];
    accel_read_bytes(raw_data, ADXL345_SAMPLE_LEN);

    // Converte os valores para g.
    accel_data->x = (float)(int16_t)((raw_data[1] << 8) | raw_data[0]) * 0.0039;
    accel_data->y = (float)(int16_t)((raw_data[3] << 8) | raw_data[2]) * 0.0039;
    accel_data->z = (float)(int16_t)((raw_data[5] << 8) | raw_data[4]) * 0.0039;
}
