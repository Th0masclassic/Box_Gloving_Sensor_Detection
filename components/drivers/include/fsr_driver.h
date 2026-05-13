#ifndef FSR_H
#define FSR_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define ADC_UNIT ADC_UNIT_1      // Unidade ADC usada pelo FSR.
#define FSR_PIN0 ADC_CHANNEL_0   // Canal do FSR principal.
// #define FSR_PIN1 ADC_CHANNEL_1 // Canal do segundo FSR.

/**
 * @brief Inicializa o ADC do sensor FSR.
 *
 * @return ESP_OK se a inicializacao for bem sucedida.
 */
esp_err_t fsr_init();

/**
 * @brief Le o valor do sensor FSR.
 *
 * @param channel Canal ADC a ler.
 * @return Valor convertido do sensor.
 */
float read_fsr(adc_channel_t channel);

#endif
