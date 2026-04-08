#ifndef ACCEL_H
#define ACCEL_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

#include "freertos/FreeRTOS.h" //biblioteca para o interrupt
#include "freertos/semphr.h" 

#define ACCEL_I2C_ADDR          0x53
#define READ_BYTES_ARRAY_SZ     100
#define ADXL345_SAMPLE_LEN      6
#define ADXL345_REG_DATAX0     0x32
#define ADXL345_REG_POWER_CTL   0x2D
#define ADXL345_REG_DATA_FORMAT 0x31
#define ADXL345_REG_BW_RATE     0x2C
#define ADXL345_REG_INT_ENABLE   0x2E

#define ACCEL_INT_PIN 6 //Pino do interrupt do acelerometro 

typedef struct {
    float x;
    float y;
    float z;
} accel_data_t;
/**
 * @brief read the last thing written in the output area of ADXL345
 * @return the read data in the buff
 */

 extern SemaphoreHandle_t accel_sem;

void accel_read_bytes(uint8_t *data, size_t len);
esp_err_t accel_init();
bool has_init();
uint8_t accel_get_int_source(void);
void accel_get_real_data(accel_data_t *accel_data);

#endif 