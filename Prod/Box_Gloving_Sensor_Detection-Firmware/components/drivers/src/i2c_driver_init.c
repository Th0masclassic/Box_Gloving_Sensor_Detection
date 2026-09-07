/**
 * @file i2c_driver_init.c
 * @brief Single low-latency I2C bus used by the GY-85 sensors.
 */
#include "i2c_driver_init.h"

#include "esp_log.h"

static const char *TAG = "I2C";
static bool initialized = false;

i2c_master_bus_handle_t bus_handle = NULL;

bool has_i2c_started(void)
{
    return initialized;
}

esp_err_t i2c_init(void)
{
    if (initialized && bus_handle != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        bus_handle = NULL;
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "I2C at %d Hz on SDA=%d SCL=%d",
             I2C_MASTER_FREQ_HZ, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    return ESP_OK;
}

esp_err_t i2c_register_read(
    i2c_master_dev_handle_t dev_handle,
    uint8_t reg_addr,
    uint8_t *data,
    size_t len
)
{
    if (dev_handle == NULL || data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(
        dev_handle, &reg_addr, 1u, data, len, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t i2c_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    if (dev_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(dev_handle, write_buf, sizeof(write_buf), I2C_MASTER_TIMEOUT_MS);
}
