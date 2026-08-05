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

typedef struct {
    float x;
    float y;
    float z;
} mag_data_t;

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

#endif
