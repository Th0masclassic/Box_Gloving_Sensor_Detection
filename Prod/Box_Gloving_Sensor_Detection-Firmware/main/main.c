#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
#include "sensor_payload_codec.h"
#include "transmit_driver.h"

#define CALIBRATION_SAMPLES 200
#define CALIBRATION_WAIT_MS 5000
#define ACC_EXPECTED_Z_G (-1.0f)

#define SAMPLE_BUFFER_SIZE 200
#define PUNCH_POST_TRIGGER_SAMPLES 80
#define PUNCH_PACKET_SAMPLE_COUNT 32
#define PUNCH_THRESHOLD_KG 1.0f
#define PUNCH_RELEASE_KG 0.25f
#define LED_START_STATE ESTADO_BATERIA_FRACA
#define PUNCH_TX_QUEUE_LENGTH 1
#define PUNCH_TX_TASK_STACK_SIZE 6144
#define PUNCH_TX_TASK_PRIORITY 4
#define COMPACT_MSG_META 0x05
#define COMPACT_MSG_ACK 0x06
#define PUNCH_SETUP_PACKET_SIZE 21
#define PUNCH_SETUP_ACK_PACKET_SIZE 2
#define PUNCH_META_PACKET_SIZE 9
#define PUNCH_DATA_PACKET_HEADER_SIZE 3
#define PUNCH_RX_BUFFER_SIZE 16
#define PUNCH_STREAM_PAYLOAD_MAX 193
#define PUNCH_PACKET_PAYLOAD_MAX (PUNCH_DATA_PACKET_HEADER_SIZE + PUNCH_STREAM_PAYLOAD_MAX)
#define PUNCH_SEND_RETRY_COUNT 3
#define PUNCH_SEND_RETRY_DELAY_MS 20
#define PUNCH_PACKET_DELAY_MS 3
#define SETUP_RESEND_INTERVAL_MS 350
#define FSR_SCALE_CENTI_KG 100.0f
#define ACC_SCALE_CENTI_G 100.0f
#define GYRO_SCALE_DECI_DPS 10.0f
#define MAG_SCALE_TEN_THOUSAND_GAUSS 10000.0f
#define SENSOR_DEADBAND_NONE 0


typedef struct {
    accel_data_t acc;
    giro_data_t gyro;
    mag_data_t mag;
    float force_kg;
} sensor_sample_t;

typedef struct {
    uint8_t punch_id;
    int count;
    int trigger_index;
    float trigger_force_kg;
    sensor_sample_t samples[SAMPLE_BUFFER_SIZE];
} punch_snapshot_t;

typedef enum {
    PUNCH_STREAM_ACC = 0x01,
    PUNCH_STREAM_GYRO = 0x02,
    PUNCH_STREAM_MAG = 0x03
} punch_stream_id_t;

static const char *TAG = "MAIN";
static TaskHandle_t sensor_task_handle = NULL;
static TaskHandle_t punch_tx_task_handle = NULL;
static TaskHandle_t setup_tx_task_handle = NULL;
static QueueHandle_t punch_tx_queue = NULL;

static accel_data_t accel_offset = {0};
static giro_data_t gyro_offset = {0};
static bool motion_calibrated = false;
static mag_data_t mag_reference = {0};

static sensor_sample_t sample_buffer[SAMPLE_BUFFER_SIZE];
static int buffer_write_index = 0;
static int buffer_count = 0;

static punch_snapshot_t punch_queue_snapshot;
static punch_snapshot_t punch_transmit_snapshot;
static uint8_t next_punch_id = 0;

static esp_err_t calibrate_motion_sensors(void)
{
    accel_data_t acc_sample = {0};
    giro_data_t gyro_sample = {0};
    mag_data_t mag_sample = {0};

    float acc_sum_x = 0.0f;
    float acc_sum_y = 0.0f;
    float acc_sum_z = 0.0f;
    float gyro_sum_x = 0.0f;
    float gyro_sum_y = 0.0f;
    float gyro_sum_z = 0.0f;
    float mag_sum_x = 0.0f;
    float mag_sum_y = 0.0f;
    float mag_sum_z = 0.0f;
    int mag_sample_count = 0;

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

        err = mag_get_real_data(&mag_sample);
        if (err == ESP_OK) {
            mag_sum_x += mag_sample.x;
            mag_sum_y += mag_sample.y;
            mag_sum_z += mag_sample.z;
            mag_sample_count++;
        } else if (mag_sample_count == 0) {
            ESP_LOGW(TAG, "Falha ao ler magnetometro durante calibracao: %s", esp_err_to_name(err));
        }
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

    if (mag_sample_count <= 0) {
        ESP_LOGW(TAG, "Sem amostras validas do magnetometro durante calibracao");
        mag_reference.x = 0.0f;
        mag_reference.y = 0.0f;
        mag_reference.z = 0.0f;
    } else {
        mag_reference.x = mag_sum_x / (float)mag_sample_count;
        mag_reference.y = mag_sum_y / (float)mag_sample_count;
        mag_reference.z = mag_sum_z / (float)mag_sample_count;
        ESP_LOGI(TAG, "MAG referencia [X:%.4f Y:%.4f Z:%.4f]",
                 mag_reference.x, mag_reference.y, mag_reference.z);
    }

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

static int16_t float_to_i16_scaled(float value, float scale)
{
    float scaled = value * scale;

    if (scaled > 32767.0f) {
        return 32767;
    }

    if (scaled < -32768.0f) {
        return -32768;
    }

    return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static void write_u16_le(uint8_t *buffer, int offset, uint16_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFF);
    buffer[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
}

static void write_i16_le(uint8_t *buffer, int *offset, int16_t value)
{
    buffer[*offset] = (uint8_t)(value & 0xFF);
    buffer[*offset + 1] = (uint8_t)((value >> 8) & 0xFF);
    *offset += 2;
}

static int get_punch_packet_count(int sample_count)
{
    return (sample_count + PUNCH_PACKET_SAMPLE_COUNT - 1) / PUNCH_PACKET_SAMPLE_COUNT;
}

static void queue_latest_punch_snapshot(float trigger_force_kg, int trigger_buffer_index)
{
    if (punch_tx_queue == NULL) {
        ESP_LOGW(TAG, "Fila de transmissao de socos ainda nao criada");
        return;
    }

    int oldest_index = (buffer_write_index - buffer_count + SAMPLE_BUFFER_SIZE) % SAMPLE_BUFFER_SIZE;
    int trigger_index = 0;

    if (buffer_count > 0) {
        trigger_index = (trigger_buffer_index - oldest_index + SAMPLE_BUFFER_SIZE) % SAMPLE_BUFFER_SIZE;
        if (trigger_index >= buffer_count) {
            trigger_index = buffer_count - 1;
        }
    }

    punch_queue_snapshot.punch_id = next_punch_id++;
    punch_queue_snapshot.count = buffer_count;
    punch_queue_snapshot.trigger_index = trigger_index;
    punch_queue_snapshot.trigger_force_kg = trigger_force_kg;

    for (int i = 0; i < punch_queue_snapshot.count; i++) {
        int idx = (oldest_index + i) % SAMPLE_BUFFER_SIZE;
        punch_queue_snapshot.samples[i] = sample_buffer[idx];
    }

    xQueueOverwrite(punch_tx_queue, &punch_queue_snapshot);

    ESP_LOGI(TAG,
             "Soco %u colocado na fila BLE com %d/%d amostras",
             punch_queue_snapshot.punch_id,
             punch_queue_snapshot.count,
             SAMPLE_BUFFER_SIZE);
}

static int encode_stream_samples(
    const punch_snapshot_t *snapshot,
    punch_stream_id_t stream_id,
    int sample_offset,
    int sample_count,
    uint8_t *encoded,
    int encoded_max
)
{
    int16_t samples[PUNCH_PACKET_SAMPLE_COUNT * AXIS_COUNT_3D] = {0};

    for (int i = 0; i < sample_count; i++) {
        const sensor_sample_t *sample = &snapshot->samples[sample_offset + i];

        switch (stream_id) {
            case PUNCH_STREAM_ACC:
                samples[i * AXIS_COUNT_3D] = float_to_i16_scaled(sample->acc.x, ACC_SCALE_CENTI_G);
                samples[i * AXIS_COUNT_3D + 1] = float_to_i16_scaled(sample->acc.y, ACC_SCALE_CENTI_G);
                samples[i * AXIS_COUNT_3D + 2] = float_to_i16_scaled(sample->acc.z, ACC_SCALE_CENTI_G);
                break;

            case PUNCH_STREAM_GYRO:
                samples[i * AXIS_COUNT_3D] = float_to_i16_scaled(sample->gyro.x, GYRO_SCALE_DECI_DPS);
                samples[i * AXIS_COUNT_3D + 1] = float_to_i16_scaled(sample->gyro.y, GYRO_SCALE_DECI_DPS);
                samples[i * AXIS_COUNT_3D + 2] = float_to_i16_scaled(sample->gyro.z, GYRO_SCALE_DECI_DPS);
                break;

            case PUNCH_STREAM_MAG:
                samples[i * AXIS_COUNT_3D] = float_to_i16_scaled(sample->mag.x, MAG_SCALE_TEN_THOUSAND_GAUSS);
                samples[i * AXIS_COUNT_3D + 1] = float_to_i16_scaled(sample->mag.y, MAG_SCALE_TEN_THOUSAND_GAUSS);
                samples[i * AXIS_COUNT_3D + 2] = float_to_i16_scaled(sample->mag.z, MAG_SCALE_TEN_THOUSAND_GAUSS);
                break;

            default:
                return 0;
        }
    }

    return sensor_payload_encode_i16_3axis_best(
        encoded,
        encoded_max,
        samples,
        sample_count,
        SENSOR_DEADBAND_NONE
    );
}

static int build_punch_stream_packet(
    const punch_snapshot_t *snapshot,
    punch_stream_id_t stream_id,
    uint8_t packet_index,
    int sample_offset,
    int sample_count,
    uint8_t *packet,
    int packet_max
)
{
    if (packet_max < PUNCH_PACKET_PAYLOAD_MAX || sample_count <= 0 || packet_index > 0x0F) {
        return 0;
    }

    uint8_t encoded[PUNCH_STREAM_PAYLOAD_MAX] = {0};
    int encoded_len = encode_stream_samples(
        snapshot,
        stream_id,
        sample_offset,
        sample_count,
        encoded,
        sizeof(encoded)
    );

    if (encoded_len <= 0) {
        return 0;
    }

    packet[0] = encoded[0];
    packet[1] = snapshot->punch_id;
    packet[2] = (uint8_t)(((uint8_t)stream_id << 4) | (packet_index & 0x0F));
    memcpy(&packet[PUNCH_DATA_PACKET_HEADER_SIZE], &encoded[1], (size_t)(encoded_len - 1));

    return PUNCH_DATA_PACKET_HEADER_SIZE + encoded_len - 1;
}

static int build_punch_meta_packet(const punch_snapshot_t *snapshot, uint8_t *packet, int packet_max)
{
    if (packet_max < PUNCH_META_PACKET_SIZE) {
        return 0;
    }

    int total_packets = get_punch_packet_count(snapshot->count);
    int trigger_index = snapshot->trigger_index;
    if (trigger_index < 0) {
        trigger_index = 0;
    } else if (trigger_index >= snapshot->count) {
        trigger_index = snapshot->count > 0 ? snapshot->count - 1 : 0;
    }
    uint16_t trigger_force_centi_kg = (uint16_t)float_to_i16_scaled(
        snapshot->trigger_force_kg,
        FSR_SCALE_CENTI_KG
    );

    packet[0] = sensor_payload_make_header(COMPACT_MSG_META, 1);
    packet[1] = snapshot->punch_id;
    packet[2] = (uint8_t)snapshot->count;
    packet[3] = PUNCH_PACKET_SAMPLE_COUNT;
    packet[4] = (uint8_t)total_packets;
    packet[5] = (uint8_t)((1U << PUNCH_STREAM_ACC) | (1U << PUNCH_STREAM_GYRO) | (1U << PUNCH_STREAM_MAG));
    packet[6] = (uint8_t)trigger_index;
    write_u16_le(packet, 7, trigger_force_centi_kg);

    return PUNCH_META_PACKET_SIZE;
}

static esp_err_t send_punch_packet(const uint8_t *packet, uint16_t packet_len)
{
    esp_err_t last_err = ESP_ERR_INVALID_STATE;

    for (int attempt = 0; attempt < PUNCH_SEND_RETRY_COUNT; attempt++) {
        if (!can_send_data()) {
            last_err = ESP_ERR_INVALID_STATE;
        } else {
            last_err = send_raw_notification(packet, packet_len);

            if (last_err == ESP_OK) {
                return ESP_OK;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PUNCH_SEND_RETRY_DELAY_MS));
    }

    return last_err;
}

static bool is_setup_ack(const uint8_t *ack, uint16_t ack_len)
{
    return ack != NULL &&
           ack_len == PUNCH_SETUP_ACK_PACKET_SIZE &&
           sensor_payload_get_mode(ack[0]) == COMPACT_MSG_ACK &&
           sensor_payload_get_count(ack[0]) == 1 &&
           ack[1] == 0;
}

static int build_setup_packet(uint8_t *packet, int packet_max)
{
    if (packet_max < PUNCH_SETUP_PACKET_SIZE) {
        return 0;
    }

    int offset = 0;
    packet[offset++] = sensor_payload_make_header(SENSOR_PAYLOAD_MODE_SETUP, 1);
    packet[offset++] = 0;
    packet[offset++] = PUNCH_PACKET_SAMPLE_COUNT;

    write_i16_le(packet, &offset, float_to_i16_scaled(accel_offset.x, ACC_SCALE_CENTI_G));
    write_i16_le(packet, &offset, float_to_i16_scaled(accel_offset.y, ACC_SCALE_CENTI_G));
    write_i16_le(packet, &offset, float_to_i16_scaled(accel_offset.z, ACC_SCALE_CENTI_G));
    write_i16_le(packet, &offset, float_to_i16_scaled(gyro_offset.x, GYRO_SCALE_DECI_DPS));
    write_i16_le(packet, &offset, float_to_i16_scaled(gyro_offset.y, GYRO_SCALE_DECI_DPS));
    write_i16_le(packet, &offset, float_to_i16_scaled(gyro_offset.z, GYRO_SCALE_DECI_DPS));
    write_i16_le(packet, &offset, float_to_i16_scaled(mag_reference.x, MAG_SCALE_TEN_THOUSAND_GAUSS));
    write_i16_le(packet, &offset, float_to_i16_scaled(mag_reference.y, MAG_SCALE_TEN_THOUSAND_GAUSS));
    write_i16_le(packet, &offset, float_to_i16_scaled(mag_reference.z, MAG_SCALE_TEN_THOUSAND_GAUSS));

    return offset;
}

static void setup_transmit_task(void *pvParameter)
{
    (void)pvParameter;

    bool setup_acknowledged = false;
    uint8_t packet[PUNCH_SETUP_PACKET_SIZE] = {0};
    uint8_t rx_buffer[PUNCH_RX_BUFFER_SIZE] = {0};

    while (1) {
        if (!motion_calibrated || !can_send_data()) {
            setup_acknowledged = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint16_t rx_size = 0;
        if (receive_data(rx_buffer, sizeof(rx_buffer), &rx_size) == ESP_OK &&
            is_setup_ack(rx_buffer, rx_size)) {
            if (!setup_acknowledged) {
                ESP_LOGI(TAG, "ACK do setup recebido; ligacao pronta");
            }
            setup_acknowledged = true;
        }

        if (!setup_acknowledged) {
            int packet_len = build_setup_packet(packet, sizeof(packet));
            if (packet_len > 0 && send_raw_notification(packet, (uint16_t)packet_len) == ESP_OK) {
                ESP_LOGI(TAG, "Setup de calibracao enviado; a aguardar ACK");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SETUP_RESEND_INTERVAL_MS));
    }
}

static void send_punch_snapshot(const punch_snapshot_t *snapshot)
{
    if (snapshot->count <= 0) {
        ESP_LOGW(TAG, "Snapshot de soco sem amostras para enviar");
        return;
    }

    uint8_t packet[PUNCH_PACKET_PAYLOAD_MAX] = {0};
    const punch_stream_id_t streams[] = {
        PUNCH_STREAM_ACC,
        PUNCH_STREAM_GYRO,
        PUNCH_STREAM_MAG
    };
    const int total_packets = get_punch_packet_count(snapshot->count);

    ESP_LOGI(TAG,
             "A enviar soco %u por BLE: meta + ACC/GYRO/MAG, %d amostras, %d blocos de ate %d",
             snapshot->punch_id,
             snapshot->count,
             total_packets,
             PUNCH_PACKET_SAMPLE_COUNT);

    int meta_len = build_punch_meta_packet(snapshot, packet, sizeof(packet));
    if (meta_len > 0) {
        esp_err_t err = send_punch_packet(packet, (uint16_t)meta_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Falha ao enviar meta do soco %u: %s",
                     snapshot->punch_id,
                     esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(PUNCH_PACKET_DELAY_MS));
    }

    for (int packet_index = 0; packet_index < total_packets; packet_index++) {
        int sample_offset = packet_index * PUNCH_PACKET_SAMPLE_COUNT;
        int remaining = snapshot->count - sample_offset;
        int sample_count = remaining > PUNCH_PACKET_SAMPLE_COUNT ? PUNCH_PACKET_SAMPLE_COUNT : remaining;

        for (int stream = 0; stream < (int)(sizeof(streams) / sizeof(streams[0])); stream++) {
            int packet_len = build_punch_stream_packet(
                snapshot,
                streams[stream],
                (uint8_t)packet_index,
                sample_offset,
                sample_count,
                packet,
                sizeof(packet)
            );

            if (packet_len <= 0) {
                ESP_LOGE(TAG,
                         "Falha ao codificar stream %d do pacote %d do soco %u",
                         streams[stream],
                         packet_index,
                         snapshot->punch_id);
                continue;
            }

            esp_err_t err = send_punch_packet(packet, (uint16_t)packet_len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "Falha ao enviar stream %d pacote %d do soco %u: %s",
                         streams[stream],
                         packet_index,
                         snapshot->punch_id,
                         esp_err_to_name(err));
            }

            vTaskDelay(pdMS_TO_TICKS(PUNCH_PACKET_DELAY_MS));
        }
    }
}

static void punch_transmit_task(void *pvParameter)
{
    (void)pvParameter;

    while (1) {
        if (xQueueReceive(punch_tx_queue, &punch_transmit_snapshot, portMAX_DELAY) == pdTRUE) {
            send_punch_snapshot(&punch_transmit_snapshot);
        }
    }
}

static void sensor_task(void *pvParameter)
{
    (void)pvParameter;

    bool punch_active = false;
    bool punch_capture_pending = false;
    int post_trigger_samples_remaining = 0;
    int trigger_buffer_index = 0;
    float trigger_force_kg = 0.0f;

    ESP_LOGW(TAG, "Calibracao por fazer. Coloca a luva parada em cima da mesa.");
    ESP_LOGW(TAG, "A calibracao vai comecar em %d segundos.", CALIBRATION_WAIT_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(CALIBRATION_WAIT_MS));

    ESP_ERROR_CHECK(calibrate_motion_sensors());
    ESP_LOGI(TAG, "Sistema pronto. Amostragem controlada pelo DATA_READY do acelerometro.");
    mag_data_t last_mag_sample = mag_reference;

    while (1) {
        sensor_sample_t sample = {0};

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sample.force_kg = read_fsr(FSR_PIN0);

        esp_err_t acc_err = accel_get_real_data(&sample.acc);
        esp_err_t gyro_err = giro_get_real_data(&sample.gyro);
        esp_err_t mag_err = mag_get_real_data(&sample.mag);

        if (acc_err != ESP_OK || gyro_err != ESP_OK) {
            ESP_LOGW(TAG, "Falha leitura sensores ACC:%s GIRO:%s",
                     esp_err_to_name(acc_err),
                     esp_err_to_name(gyro_err));
            continue;
        }

        if (mag_err != ESP_OK) {
            sample.mag = last_mag_sample;
            ESP_LOGW(TAG, "Falha leitura MAG:%s; a usar ultima amostra valida",
                     esp_err_to_name(mag_err));
        } else {
            last_mag_sample = sample.mag;
        }

        apply_motion_calibration(&sample.acc, &sample.gyro);

        store_sample(&sample);
        int stored_sample_index = (buffer_write_index + SAMPLE_BUFFER_SIZE - 1) % SAMPLE_BUFFER_SIZE;

        if (!punch_active && !punch_capture_pending && sample.force_kg >= PUNCH_THRESHOLD_KG) {
            punch_active = true;
            punch_capture_pending = true;
            post_trigger_samples_remaining = PUNCH_POST_TRIGGER_SAMPLES + 1;
            trigger_buffer_index = stored_sample_index;
            trigger_force_kg = sample.force_kg;
            ESP_LOGI(TAG, "Inicio de soco detetado. A capturar %d amostras apos trigger.",
                     PUNCH_POST_TRIGGER_SAMPLES);
        }

        if (punch_capture_pending) {
            post_trigger_samples_remaining--;
            if (post_trigger_samples_remaining <= 0) {
                punch_capture_pending = false;
                queue_latest_punch_snapshot(trigger_force_kg, trigger_buffer_index);
                ESP_LOGI(TAG, "Janela de soco completa. Snapshot enviado para a tarefa BLE.");
            }
        }

        if (punch_active && sample.force_kg <= PUNCH_RELEASE_KG) {
            punch_active = false;
            ESP_LOGI(TAG, "Soco terminado. Sistema pronto para novo trigger.");
        }
    }
}

extern i2c_master_bus_handle_t bus_handle;

static void i2c_scan(void)
{
    ESP_LOGI("I2C_SCAN", "Iniciando scan...");

    for (uint8_t addr = 1; addr < 0x7F; addr++) {

        esp_err_t ret = i2c_master_probe(
            bus_handle,
            addr,
            pdMS_TO_TICKS(50)
        );

        if (ret == ESP_OK) {
            ESP_LOGI("I2C_SCAN", "Encontrado dispositivo em 0x%02X", addr);
        }
    }

    ESP_LOGI("I2C_SCAN", "Scan terminado");
}

void app_main(void)
{
    ESP_LOGI("MAIN", "Entrou na app_main");

    ESP_LOGI("MAIN", "Antes de init_nvs");
    esp_err_t ret = init_nvs();
    ESP_LOGI("MAIN", "init_nvs: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(ret);

    ESP_LOGI("MAIN", "Antes de transmit_driver_init");
    transmit_driver_init();
    ESP_LOGI("MAIN", "Depois de transmit_driver_init");

    ESP_LOGI("MAIN", "Antes de i2c_init");
    ret = i2c_init();
    ESP_LOGI("I2C", "SDA level = %d", gpio_get_level(I2C_MASTER_SDA_IO));
    ESP_LOGI("I2C", "SCL level = %d", gpio_get_level(I2C_MASTER_SCL_IO));

    ESP_LOGI("MAIN", "i2c_init: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(ret);

    i2c_scan();

    ESP_LOGI("MAIN", "Antes de accel_init");
    ret = accel_init();
    ESP_LOGI("MAIN", "accel_init: %s", esp_err_to_name(ret));

    if (ret != ESP_OK) {
        ESP_LOGE("MAIN", "Falha no acelerometro");
    }

    ESP_LOGI("MAIN", "Antes de giro_init");
    esp_err_t giro_ret = giro_init();
    ESP_LOGI("MAIN", "giro_init: %s", esp_err_to_name(giro_ret));

    ESP_LOGI("MAIN", "Antes de mag_init");
    esp_err_t mag_ret = mag_init();
    ESP_LOGI("MAIN", "mag_init: %s", esp_err_to_name(mag_ret));

    ESP_LOGI("MAIN", "Antes de fsr_init");
    ret = fsr_init();
    ESP_LOGI("MAIN", "fsr_init: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(ret);

    ESP_LOGI("MAIN", "Antes de led_init");
    led_init();
    ESP_LOGI("MAIN", "Depois de led_init");

    led_set_estado(LED_START_STATE);

    ESP_LOGI("MAIN", "Antes de criar queue");
    punch_tx_queue = xQueueCreate(
        PUNCH_TX_QUEUE_LENGTH,
        sizeof(punch_snapshot_t)
    );

    if (punch_tx_queue == NULL) {
        ESP_LOGE("MAIN", "Falha ao criar punch_tx_queue");
        return;
    }

    ESP_LOGI("MAIN", "Queue criada");

    BaseType_t task_ret;

    task_ret = xTaskCreate(
        punch_transmit_task,
        "punch_tx_task",
        PUNCH_TX_TASK_STACK_SIZE,
        NULL,
        PUNCH_TX_TASK_PRIORITY,
        &punch_tx_task_handle
    );

    ESP_LOGI("MAIN", "punch_transmit_task: %s",
             task_ret == pdPASS ? "OK" : "ERRO");

    task_ret = xTaskCreate(
        setup_transmit_task,
        "setup_tx_task",
        3072,
        NULL,
        3,
        &setup_tx_task_handle
    );

    ESP_LOGI("MAIN", "setup_transmit_task: %s",
             task_ret == pdPASS ? "OK" : "ERRO");

    task_ret = xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        NULL,
        5,
        &sensor_task_handle
    );

    ESP_LOGI("MAIN", "sensor_task: %s",
             task_ret == pdPASS ? "OK" : "ERRO");

    ESP_LOGI("MAIN", "Antes de accel_setup_interrupt");

    ret = accel_setup_interrupt(sensor_task_handle);

    ESP_LOGI("MAIN", "accel_setup_interrupt: %s",
             esp_err_to_name(ret));

    ESP_LOGI("MAIN", "Sistema GY-85 + FSR pronto");
}