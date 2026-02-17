#ifndef ACCEL_H
#define ACCEL_H

#include "esp_err.h"
#include "gpio.h"

#define ACCEL_INIT_PIN (1ULL << GPIO_NUM_12) 

/**
 * @brief Initialize the Accelerometer Hardware
 * @return ESP_OK if initialized , ESP_ERR otherwise
 */
esp_err_t accel_init();

#endif 