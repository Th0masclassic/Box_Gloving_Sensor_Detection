#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

#include "accelerometer_driver.h"
#include "fsr_driver.h"
#include "giroscopio_driver.h"
#include "i2c_driver_init.h"
#include "led_driver.h"
#include "magnometer_driver.h"
#include "nvs_driver.h"
#include "transmit_driver.h"

#define CALIBRATION_SAMPLES 200
#define CALIBRATION_WAIT_MS 5000
#define ACC_EXPECTED_Z_G (-1.0f)

#define SAMPLE_BUFFER_SIZE 200
#define FORCE_BUFFER_SIZE 200
#define PUNCH_THRESHOLD_KG 1.0f
#define PUNCH_RELEASE_KG 0.25f
#define LED_START_STATE ESTADO_BATERIA_FRACA


typedef struct {
    accel_data_t acc;
    giro_data_t gyro;
    mag_data_t mag;
    float force_kg;
} sensor_sample_t;

typedef struct {
    float force_kg;
} force_sample_t;

static const char *TAG = "MAIN";
static TaskHandle_t sensor_task_handle = NULL;

static accel_data_t accel_offset = {0};
static giro_data_t gyro_offset = {0};
static bool motion_calibrated = false;

static sensor_sample_t sample_buffer[SAMPLE_BUFFER_SIZE];
static int buffer_write_index = 0;
static int buffer_count = 0;

static sensor_sample_t frozen_motion_buffer[SAMPLE_BUFFER_SIZE];
static int frozen_motion_count = 0;

static force_sample_t force_buffer[FORCE_BUFFER_SIZE];
static int force_buffer_count = 0;
static float punch_peak_force_kg = 0.0f;

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

static void store_sample(const sensor_sample_t *sample)
{
    sample_buffer[buffer_write_index] = *sample;
    buffer_write_index = (buffer_write_index + 1) % SAMPLE_BUFFER_SIZE;

    if (buffer_count < SAMPLE_BUFFER_SIZE) {
        buffer_count++;
    }
}

static void freeze_motion_buffer(void)
{
    int oldest_index = (buffer_write_index - buffer_count + SAMPLE_BUFFER_SIZE) % SAMPLE_BUFFER_SIZE;

    frozen_motion_count = buffer_count;
    for (int i = 0; i < frozen_motion_count; i++) {
        int idx = (oldest_index + i) % SAMPLE_BUFFER_SIZE;
        frozen_motion_buffer[i] = sample_buffer[idx];
    }
}

static void reset_force_buffer(void)
{
    force_buffer_count = 0;
    punch_peak_force_kg = 0.0f;
}

static void store_force_sample(float force_kg)
{
    if (force_buffer_count < FORCE_BUFFER_SIZE) {
        force_buffer[force_buffer_count].force_kg = force_kg;
        force_buffer_count++;
    }

    if (force_kg > punch_peak_force_kg) {
        punch_peak_force_kg = force_kg;
    }
}

static void print_punch_data(void)
{
    ESP_LOGI(TAG, "========== SOCO DETETADO ==========");
    ESP_LOGI(TAG, "Pico de forca: %.2f kg", punch_peak_force_kg);
    ESP_LOGI(TAG, "Amostras FSR usadas para pico: %d", force_buffer_count);
    ESP_LOGI(TAG, "A imprimir %d amostras FIFO de movimento congeladas no impacto", frozen_motion_count);

    for (int i = 0; i < frozen_motion_count; i++) {
        const sensor_sample_t *sample = &frozen_motion_buffer[i];

        printf("[%03d] FSR:%5.2f kg | "
               "ACC [X:%7.3f Y:%7.3f Z:%7.3f] | "
               "GIRO [X:%8.3f Y:%8.3f Z:%8.3f] | "
               "MAG [X:%7.4f Y:%7.4f Z:%7.4f]\n",
               i,
               sample->force_kg,
               sample->acc.x, sample->acc.y, sample->acc.z,
               sample->gyro.x, sample->gyro.y, sample->gyro.z,
               sample->mag.x, sample->mag.y, sample->mag.z);
    }

    ESP_LOGI(TAG, "===================================");
}

static void sensor_task(void *pvParameter)
{
    (void)pvParameter;

    bool punch_active = false;

    ESP_LOGW(TAG, "Calibracao por fazer. Coloca a luva parada em cima da mesa.");
    ESP_LOGW(TAG, "A calibracao vai comecar em %d segundos.", CALIBRATION_WAIT_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(CALIBRATION_WAIT_MS));

    ESP_ERROR_CHECK(calibrate_motion_sensors());
    ESP_LOGI(TAG, "Sistema pronto. Amostragem controlada pelo DATA_READY do acelerometro.");

    while (1) {
        sensor_sample_t sample = {0};

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sample.force_kg = read_fsr(FSR_PIN0);

        esp_err_t acc_err = accel_get_real_data(&sample.acc);
        esp_err_t gyro_err = giro_get_real_data(&sample.gyro);
        esp_err_t mag_err = mag_get_real_data(&sample.mag);

        if (acc_err != ESP_OK || gyro_err != ESP_OK || mag_err != ESP_OK) {
            ESP_LOGW(TAG, "Falha leitura sensores ACC:%s GIRO:%s MAG:%s",
                     esp_err_to_name(acc_err),
                     esp_err_to_name(gyro_err),
                     esp_err_to_name(mag_err));
            continue;
        }

        apply_motion_calibration(&sample.acc, &sample.gyro);

        if (!punch_active) {
            store_sample(&sample);

            if (sample.force_kg >= PUNCH_THRESHOLD_KG) {
                punch_active = true;
                freeze_motion_buffer();
                reset_force_buffer();
                store_force_sample(sample.force_kg);
                ESP_LOGI(TAG, "Inicio de soco detetado. Buffer de movimento congelado.");
            }
            continue;
        }

        store_force_sample(sample.force_kg);

        if (sample.force_kg <= PUNCH_RELEASE_KG) {
            punch_active = false;
            print_punch_data();
        }
    }
}

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
    led_set_estado(LED_START_STATE);

    ESP_ERROR_CHECK(xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, &sensor_task_handle) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(accel_setup_interrupt(sensor_task_handle));

    ESP_LOGI(TAG, "Sistema GY-85 + FSR pronto");
}
