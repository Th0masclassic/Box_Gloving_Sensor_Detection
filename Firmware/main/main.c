#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_driver.h"
#include "transmit_driver.h"

void app_main(void)
{
    printf("Hello World!\n");
    
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_transmit_driver());

}
