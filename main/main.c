#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_driver.h"
#include "i2c_driver_init.h"
#include "accelerometer_driver.h"
#include "giroscopio_driver.h"
#include "transmit_driver.h"
#include "fsr_driver.h"
#include "driver/i2c_master.h"
#include "magnometer_driver.h"

static const char *TAG = "MAIN";
static const int LIMITE_GOLPE = 1;

void sensor_task(void *pvParameter) {
    (void)pvParameter;

    accel_data_t acc_dados = {0};
    giro_data_t gyr_dados = {0};
    mag_data_t mag_dados = {0};

    while (1) {
        int forca = read_fsr(FSR_PIN0);

        // Le os sensores I2C.
        if (accel_get_real_data(&acc_dados) != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler o acelerometro");
        }
        if (giro_get_real_data(&gyr_dados) != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler o giroscopio");
        }
        if (mag_get_real_data(&mag_dados) != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler o magnetometro");
        }

        printf("FSR:%4d | ACC [X:%6.2f Y:%6.2f Z:%6.2f] | GIRO [X:%7.2f Y:%7.2f Z:%7.2f] | MAG [X:%6.2f Y:%6.2f Z:%6.2f]\n",
               forca,
               acc_dados.x, acc_dados.y, acc_dados.z,
               gyr_dados.x, gyr_dados.y, gyr_dados.z,
               mag_dados.x, mag_dados.y, mag_dados.z);

        if (forca > LIMITE_GOLPE) {
            printf("GOLPE DETETADO! FSR: %4d\n", forca);

            // Evita contar o mesmo golpe duas vezes.
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    // Inicializa os servicos base.
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_transmit_driver());
    
    // Inicializa os drivers.
    ESP_ERROR_CHECK(i2c_init());
    
    if (accel_init() != ESP_OK) {
        ESP_LOGE(TAG, "Erro critico: Acelerometro nao encontrado!");
    }

    if (giro_init() != ESP_OK) {
        ESP_LOGE(TAG, "Erro critico: Giroscopio nao encontrado!");
    }

    if (mag_init() != ESP_OK) {
        ESP_LOGE(TAG, "Erro critico: Magnetometro nao encontrado!");
    }

    if (fsr_init() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar FSR");
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Sistema de 6 eixos pronto. A iniciar leituras...");
    
    // Tarefa de leitura dos sensores.
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}
