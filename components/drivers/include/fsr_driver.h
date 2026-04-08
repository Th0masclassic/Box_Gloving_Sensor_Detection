#ifndef FSR_H
#define FSR_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h" //biblioteca para o interrupt
#include "freertos/task.h"
#include "freertos/semphr.h"   
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h" 

#define ADC_UNIT ADC_UNIT_1
#define ADC_PIN ADC_CHANNEL_0 //GPIO0

esp_err_t fsr_init();
int read_fsr();

#endif 