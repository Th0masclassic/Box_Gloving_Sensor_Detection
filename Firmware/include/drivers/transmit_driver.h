#ifndef TRANSMIT_DRIVER_H
#define TRANSMIT_DRIVER_H

#include "esp_err.h"

#define CHANNEL_ID 1

/**
 * @brief Init Transmit Driver To Allow Send Data trough ESP_NOW
 * @return esp_ok if Initialized, esp_err otherwise
 */
esp_err_t init_transmit_driver();

#endif