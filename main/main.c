#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_driver.h"
#include "i2c_driver_init.h"
#include "accelerometer_driver.h"
#include "transmit_driver.h"
#include "fsr_driver.h"

static const char *TAG = "MAIN";

void sensor_task(void *pvParameter) {
    accel_data_t dados;
    uint8_t int_source = 0;
    
    while (1) {
        // Chamamos a função do driver em vez de usar a variável diretamente
        uint8_t src = accel_get_int_source();
        ESP_LOGI("ACCEL", "INT_SOURCE = 0x%02X", src);
        vTaskDelay(pdMS_TO_TICKS(200));
        // Espera apenas 1 segundo pelo interrupt físico
        if (xSemaphoreTake(accel_sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int_source = accel_get_int_source();
            
            int força = read_fsr();
            printf("Força: %d\n", força);
        
            // Verifica se o bit DATA_READY (Bit 7 / 0x80) está ativo [cite: 2095]
            if ((int_source & 0x80) == 0x80) {
                ESP_LOGW("DEBUG", "SENSOR TEM DADOS! (0x%02X) - Problema no fio/pino fisico", int_source);
            } else {
                ESP_LOGE("DEBUG", "SENSOR NAO GEROU INTERRUPCAO! (0x%02X) - Problema de config", int_source);
            }
            accel_get_real_data(&dados);
            printf("Aceleração! X:%.2f Y:%.2f Z:%.2f\n", dados.x, dados.y, dados.z);
        } else {
            printf("À espera do sinal elétrico no pino...\n");
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_transmit_driver());
    ESP_ERROR_CHECK(i2c_init());

    // Inicializa o Acelerometro
    if (accel_init() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar acelerómetro");
        return; 
    }

    // Inicializa o FSR no Pino 
    if (fsr_init() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar FSR");
        return;
    }

    ESP_LOGI(TAG, "Hardware pronto. A lançar task...");
    
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}
