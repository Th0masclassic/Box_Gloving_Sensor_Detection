#ifndef ACCEL_H
#define ACCEL_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "i2c_driver_init.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h" 
#include "freertos/task.h"

#define ACCEL_I2C_ADDR          0x53 // Endereco I2C do ADXL345.
#define READ_BYTES_ARRAY_SZ     100  // Tamanho maximo de leitura.
#define ADXL345_SAMPLE_LEN      6    // Numero de bytes por amostra.
#define ADXL345_REG_DATAX0      0x32 // Primeiro registo dos dados.
#define ADXL345_REG_POWER_CTL   0x2D // Registo de controlo de energia.
#define ADXL345_REG_DATA_FORMAT 0x31 // Registo do formato dos dados.
#define ADXL345_REG_BW_RATE     0x2C // Registo da taxa de amostragem.
#define ADXL345_REG_INT_ENABLE  0x2E // Registo de interrupcoes.
#define ADXL345_REG_INT_MAP     0x2F // Mapeamento das interrupcoes para INT1/INT2.
#define ADXL345_REG_INT_SOURCE  0x30 // Origem/limpeza das interrupcoes.
#define ADXL345_REG_FIFO_CTL    0x38 // Controlo da FIFO.

#define ADXL345_INT_DATA_READY  (1U << 7)

#define ACCEL_INT_PIN 6 // Pino de interrupcao do acelerometro.

typedef struct {
    float x;
    float y;
    float z;
} accel_data_t;


esp_err_t accel_setup_interrupt(TaskHandle_t task_to_notify);
/**
 * @brief Le bytes consecutivos do acelerometro.
 *
 * @param data Buffer de destino.
 * @param len Numero de bytes a ler.
 */
void accel_read_bytes(uint8_t *data, size_t len);

/**
 * @brief Inicializa o acelerometro.
 *
 * @return ESP_OK se a inicializacao for bem sucedida.
 */
esp_err_t accel_init();

/**
 * @brief Le a origem da ultima interrupcao.
 *
 * @return Valor do registo INT_SOURCE.
 */
uint8_t accel_get_int_source(void);

/**
 * @brief Le os dados do acelerometro em g.
 *
 * @param accel_data Estrutura onde os dados sao guardados.
 * @return ESP_OK se a leitura for bem sucedida.
 */
esp_err_t accel_get_real_data(accel_data_t *accel_data);

#endif
