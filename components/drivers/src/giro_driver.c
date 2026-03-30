#include "accelerometer_driver.h"
#include "i2c_driver_init.h"
#include "esp_log.h"

static const char *TAG = "ACCEL";
static uint16_t accel_raw_data[READ_BYTES_ARRAY_SZ];
static i2c_master_dev_handle_t accel_dev_handle = NULL;

extern i2c_master_bus_handle_t bus_handle;

bool init = false;

void accel_read_bytes(uint8_t *data, size_t len) {
    esp_err_t ret;

    if(!has_init()){
        ret = accel_init();
        if(ret != ESP_OK) {
            ESP_LOGE(TAG,"ACCEL ERROR ON INIT");
            return;
        }
    }

    if (data == NULL) {
        ESP_LOGE(TAG,"INVALID ARG");
        return ;
    }

    if (len < ADXL345_SAMPLE_LEN) {
        ESP_LOGE(TAG,"MAX SIZE REACHED");
        return ;
    }

    i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, data, ADXL345_SAMPLE_LEN);
}

bool has_init(){ return init; }

esp_err_t accel_init(){

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACCEL_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    init = true;

    return i2c_master_bus_add_device(bus_handle, &dev_cfg, &accel_dev_handle);
}
    