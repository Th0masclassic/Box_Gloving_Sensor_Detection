#ifndef NVS_DRIVER_H
#define NVS_DRIVER_H

#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"
#include "nvs_flash.h"

/**
 * @brief If Has been Init Before then it will return true
 * @return true or false based on NVS Init State
 */
bool has_init_nvs();

/**
 * @brief Initialize the NVS in esp32
 * @return ESP_OK if success, ESP_FAIL otherwise
 */
esp_err_t init_nvs();

#endif