#include "i2c_driver_init.h"
#include <stdio.h>
#include "esp_err.h"
#include <stdbool.h>
#include "esp_log.h"


static void i2c_master_init(i2c_master_dev_handle_t *dev_handle);

static const char *TAG = "I2C";
// Variable to know if its ready
static bool init = false;

i2c_master_bus_handle_t bus_handle = NULL;

bool has_i2c_started(){ return init;}

esp_err_t i2c_init(){
    
    i2c_master_dev_handle_t dev_handle;
    i2c_master_init(&dev_handle);
    ESP_LOGI(TAG, "I2C initialized successfully");

    init = true;
    return ESP_OK;
}

esp_err_t i2c_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t i2c_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(dev_handle, write_buf, sizeof(write_buf), I2C_MASTER_TIMEOUT_MS);
}

/**
 * @brief i2c master initialization
 */
static void i2c_master_init(i2c_master_dev_handle_t *dev_handle)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

}
