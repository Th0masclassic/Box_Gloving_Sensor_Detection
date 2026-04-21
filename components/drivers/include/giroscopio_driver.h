#ifndef GIRO_H
#define GIRO_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "i2c_driver_init.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ITG3200_ADDR_PRIMARY 0x68 // Endereco I2C principal.
#define ITG3200_ADDR_ALT     0x69 // Endereco I2C alternativo.

#define ITG3200_REG_SMPLRT_DIV  0x15 // Registo do divisor de amostragem.
#define ITG3200_REG_DLPF_FS     0x16 // Registo do filtro e escala.
#define ITG3200_REG_INT_CFG     0x17 // Registo de configuracao da interrupcao.
#define ITG3200_REG_INT_STATUS  0x1A // Registo de estado da interrupcao.
#define ITG3200_REG_TEMP_OUT_H  0x1B // Primeiro byte da temperatura.
#define ITG3200_REG_GYRO_XOUT_H 0x1D // Primeiro byte dos dados do gyro.
#define ITG3200_REG_PWR_MGM     0x3E // Registo de energia.

typedef struct {
    float x;
    float y;
    float z;
} giro_data_t;

/**
 * @brief Inicializa o giroscopio.
 *
 * @return ESP_OK se a inicializacao for bem sucedida.
 */
esp_err_t giro_init(void);

/**
 * @brief Le os dados do giroscopio em graus por segundo.
 *
 * @param data Estrutura onde os dados sao guardados.
 * @return ESP_OK se a leitura for bem sucedida.
 */
esp_err_t giro_get_real_data(giro_data_t *data);

#endif
