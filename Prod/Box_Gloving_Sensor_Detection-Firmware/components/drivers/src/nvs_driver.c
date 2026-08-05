#include <stdio.h>
#include "nvs_driver.h"
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"

static bool init = false;
static const char *TAG = "NVS";

bool has_init_nvs(){ return init; }

esp_err_t init_nvs(){

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret =nvs_flash_init();
    }

    if(ret != ESP_OK){
        ESP_LOGE(TAG,"Error on init nvs");
    }

    init = true;
    
    ESP_LOGI(TAG,"Init NVS successfully");
    
    return ESP_OK;
}


