/**
 * @file nvs_driver.c
 * @brief NVS initialization with recoverable partition-format handling.
 */
#include "nvs_driver.h"

#include "esp_log.h"

static const char *TAG = "NVS";
static bool initialized;

bool has_init_nvs(void)
{
    return initialized;
}

esp_err_t init_nvs(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires erase: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}
