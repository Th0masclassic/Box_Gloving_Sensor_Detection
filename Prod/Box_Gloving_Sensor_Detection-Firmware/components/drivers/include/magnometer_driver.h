#ifndef MAGNOMETER_DRIVER_H
#define MAGNOMETER_DRIVER_H

#include "esp_err.h"
#include "esp_log.h"
#include "i2c_driver_init.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define QMC5883L_ADDR 0x0D // Endereco I2C do magnetometro.
#define QMC5883L_REG_DATA_X_LSB 0x00
#define QMC5883L_REG_STATUS 0x06
#define QMC5883L_REG_CONTROL_1 0x09
#define QMC5883L_REG_SET_RESET 0x0B
#define QMC5883L_REG_CHIP_ID 0x0D
#define QMC5883L_CHIP_ID_VALUE 0xFF
#define QMC5883L_STATUS_DRDY (1U << 0)
#define QMC5883L_STATUS_OVL (1U << 1)
#define QMC5883L_STATUS_DOR (1U << 2)
#define QMC5883L_SAMPLE_RATE_HZ 200U
#define QMC5883L_LSB_PER_GAUSS 12000.0f

typedef struct {
    float x;
    float y;
    float z;
} mag_data_t;

/** Native QMC5883L field counts. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} mag_raw_data_t;

/**
 * @brief Inicializa o magnetometro.
 *
 * @return ESP_OK se a inicializacao for bem sucedida.
 */
esp_err_t mag_init(void);

/**
 * @brief Le os dados do magnetometro em Gauss.
 *
 * @param data Estrutura onde os dados sao guardados.
 * @return ESP_OK se a leitura for bem sucedida.
 */
esp_err_t mag_get_real_data(mag_data_t *data);

/** Reads the QMC5883L status register before deciding whether data are fresh. */
esp_err_t mag_get_status(uint8_t *status);

/** Reads all XYZ output bytes without applying calibration or conversion. */
esp_err_t mag_get_raw_data(mag_raw_data_t *data);

#endif
