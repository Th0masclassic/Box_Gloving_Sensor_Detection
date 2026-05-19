#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_driver.h"
#include "i2c_driver_init.h"
#include "accelerometer_driver.h"
#include "giroscopio_driver.h"
#include "magnometer_driver.h"
#include "fsr_driver.h"
#include "transmit_driver.h"
#include "led_driver.h"

#define CALIBRATION_SAMPLES 200
#define CALIBRATION_WAIT_MS 5000
#define ACC_EXPECTED_Z_G (-1.0f)

static const char *TAG = "MAIN";

static TaskHandle_t sensor_task_handle = NULL;

static accel_data_t accel_offset = {0};
static giro_data_t gyro_offset = {0};
static bool motion_calibrated = false;

/**
 * @brief Calibra os sensores de movimento com a luva parada.
 *
 * A funcao assume que a luva esta imovel em cima da mesa. Durante a calibracao
 * sao recolhidas varias amostras do acelerometro e do giroscopio. A media
 * dessas amostras e usada como offset para corrigir as leituras seguintes.
 *
 * No acelerometro, o eixo Z e ajustado para manter a componente da gravidade
 * esperada em repouso. Neste caso foi usado -1 g.
 *
 * @return ESP_OK se a calibracao terminar com sucesso.
 */
static esp_err_t calibrate_motion_sensors(void)
{
    accel_data_t acc_sample = {0};
    giro_data_t gyro_sample = {0};

    float acc_sum_x = 0.0f;
    float acc_sum_y = 0.0f;
    float acc_sum_z = 0.0f;
    float gyro_sum_x = 0.0f;
    float gyro_sum_y = 0.0f;
    float gyro_sum_z = 0.0f;

    ESP_LOGI(TAG, "Calibracao iniciada. Mantem a luva parada em cima da mesa.");

    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        esp_err_t err = accel_get_real_data(&acc_sample);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler acelerometro durante calibracao: %s", esp_err_to_name(err));
            return err;
        }

        err = giro_get_real_data(&gyro_sample);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler giroscopio durante calibracao: %s", esp_err_to_name(err));
            return err;
        }

        acc_sum_x += acc_sample.x;
        acc_sum_y += acc_sample.y;
        acc_sum_z += acc_sample.z;
        gyro_sum_x += gyro_sample.x;
        gyro_sum_y += gyro_sample.y;
        gyro_sum_z += gyro_sample.z;
    }

    accel_offset.x = acc_sum_x / CALIBRATION_SAMPLES;
    accel_offset.y = acc_sum_y / CALIBRATION_SAMPLES;
    accel_offset.z = (acc_sum_z / CALIBRATION_SAMPLES) - ACC_EXPECTED_Z_G;

    gyro_offset.x = gyro_sum_x / CALIBRATION_SAMPLES;
    gyro_offset.y = gyro_sum_y / CALIBRATION_SAMPLES;
    gyro_offset.z = gyro_sum_z / CALIBRATION_SAMPLES;

    motion_calibrated = true;

    ESP_LOGI(TAG, "Calibracao concluida");
    ESP_LOGI(TAG, "ACC offset [X:%.3f Y:%.3f Z:%.3f]",
             accel_offset.x, accel_offset.y, accel_offset.z);
    ESP_LOGI(TAG, "GIRO offset [X:%.3f Y:%.3f Z:%.3f]",
             gyro_offset.x, gyro_offset.y, gyro_offset.z);

    return ESP_OK;
}

/**
 * @brief Aplica os offsets calculados durante a calibracao.
 *
 * Corrige as leituras do acelerometro e giroscopio para reduzir o erro em
 * repouso. Se a calibracao ainda nao tiver sido feita, a funcao nao altera os
 * valores recebidos.
 *
 * @param acc_data Leitura atual do acelerometro.
 * @param gyro_data Leitura atual do giroscopio.
 */
static void apply_motion_calibration(accel_data_t *acc_data, giro_data_t *gyro_data)
{
    if (!motion_calibrated) {
        return;
    }

    acc_data->x -= accel_offset.x;
    acc_data->y -= accel_offset.y;
    acc_data->z -= accel_offset.z;

    gyro_data->x -= gyro_offset.x;
    gyro_data->y -= gyro_offset.y;
    gyro_data->z -= gyro_offset.z;
}

/**
 * @brief Tarefa responsavel pela calibracao inicial e leitura dos sensores.
 *
 * Primeiro aguarda alguns segundos para permitir colocar a luva parada em cima
 * da mesa. Depois calibra acelerometro e giroscopio. A partir dai, cada
 * interrupcao DATA_READY do acelerometro sincroniza uma nova leitura de:
 *
 * - FSR
 * - acelerometro
 * - giroscopio
 * - magnetometro
 *
 * Os dados sao impressos no monitor serie para validacao experimental.
 */
static void sensor_task(void *pvParameter)
{
    (void)pvParameter;

    accel_data_t acc_data = {0};
    giro_data_t gyro_data = {0};
    mag_data_t mag_data = {0};

    int punch_count = 0;
    bool in_punch = false;
    const float PUNCH_THRESHOLD_KG = 1.0f;

    // Inicia no estado de busca
    led_set_estado(ESTADO_BUSCA_BLE);

    ESP_LOGW(TAG, "Calibracao por fazer. Coloca a luva parada em cima da mesa.");
    ESP_LOGW(TAG, "A calibracao vai comecar em %d segundos.", CALIBRATION_WAIT_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(CALIBRATION_WAIT_MS));

    ESP_ERROR_CHECK(calibrate_motion_sensors());

    int64_t previous_time_us = esp_timer_get_time();

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t current_time_us = esp_timer_get_time();
        int64_t delta_time_us = current_time_us - previous_time_us;
        previous_time_us = current_time_us;

        float force_kg = read_fsr(FSR_PIN0);

        if (force_kg > PUNCH_THRESHOLD_KG) {
            if (!in_punch) {
                in_punch = true;
                punch_count++;
                ESP_LOGI(TAG, "Soco detetado! Forca: %.2f kg | Total socos: %d", force_kg, punch_count);
                led_set_estado(ESTADO_CONECTADO_BLE); // Liga o LED fixo enquanto houver forca
            }
        } else {
            if (in_punch) {
                in_punch = false;
                led_set_estado(ESTADO_BATERIA_FRACA); // Volta a piscar lento
            }
        }

        esp_err_t acc_err = accel_get_real_data(&acc_data);
        esp_err_t gyro_err = giro_get_real_data(&gyro_data);
        esp_err_t mag_err = mag_get_real_data(&mag_data);

        if (acc_err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler acelerometro: %s", esp_err_to_name(acc_err));
        }

        if (gyro_err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler giroscopio: %s", esp_err_to_name(gyro_err));
        }

        if (mag_err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao ler magnetometro: %s", esp_err_to_name(mag_err));
        }

        if (acc_err == ESP_OK && gyro_err == ESP_OK) {
            apply_motion_calibration(&acc_data, &gyro_data);
        }

        printf("FSR:%5.2f kg | ACC [X:%7.3f Y:%7.3f Z:%7.3f] | GIRO [X:%8.3f Y:%8.3f Z:%8.3f] | MAG [X:%7.4f Y:%7.4f Z:%7.4f]\n",
               force_kg,
               acc_data.x, acc_data.y, acc_data.z,
               gyro_data.x, gyro_data.y, gyro_data.z,
               mag_data.x, mag_data.y, mag_data.z);
    }
}

/**
 * @brief Ponto de entrada principal da aplicacao.
 *
 * Inicializa os servicos base, o barramento I2C e todos os sensores usados no
 * sistema. Depois cria a tarefa de leitura e associa a interrupcao do
 * acelerometro a essa tarefa.
 */
void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_transmit_driver());
    ESP_ERROR_CHECK(i2c_init());

    ESP_ERROR_CHECK(accel_init());
    ESP_ERROR_CHECK(giro_init());
    ESP_ERROR_CHECK(mag_init());
    ESP_ERROR_CHECK(fsr_init());

    led_init();

    ESP_ERROR_CHECK(xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, &sensor_task_handle) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(accel_setup_interrupt(sensor_task_handle));

    ESP_LOGI(TAG, "Sistema GY-85 + FSR pronto");
}

