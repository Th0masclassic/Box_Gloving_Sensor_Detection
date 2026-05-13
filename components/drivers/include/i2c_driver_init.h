#ifndef I2C_DRIVER_INIT_H
#define I2C_DRIVER_INIT_H

#include "esp_err.h"
#include "driver/i2c_master.h"

// Trocar o que esta a -1
#define I2C_MASTER_SCL_IO          9                           /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO          10                           /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM             0                           /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ          400000                  /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE   0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS       1000

/**
 * @brief Use This Function to know if the i2c Has been initialized
 */
bool has_i2c_started();

/**
 * @brief Perform Initialization of the i2c in esp32
 */
esp_err_t i2c_init();

/**
 * @brief Read a sequence of bytes from a I2C Sensor
 */
esp_err_t i2c_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data, size_t len);

/**
 * @brief Write a byte to a MPU9250 sensor register
 */
esp_err_t i2c_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data);

#endif