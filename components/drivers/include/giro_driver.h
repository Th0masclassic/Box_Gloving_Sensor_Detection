#ifndef ACCEL_H
#define ACCEL_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

#define ACCEL_I2C_ADDR          0x53
#define READ_BYTES_ARRAY_SZ     100
#define ADXL345_SAMPLE_LEN      6
#define ADXL345_REG_DATAX0     0x32


/**
 * @brief read the last thing written in the output area of ADXL345
 * @return the read data in the buff
 */
void accel_read_bytes(uint8_t *data, size_t len);

esp_err_t accel_init();

bool has_init();

#endif 