/**
 * @file accelerometer_driver.c
 * @brief ADXL345 configuration and data-ready interrupt support.
 */
#include "accelerometer_driver.h"

static const char *TAG = "ACCEL";
static i2c_master_dev_handle_t accel_dev_handle = NULL;
static TaskHandle_t sync_task_handle = NULL;

static void accel_clear_pending_interrupt(void)
{
    uint8_t raw[ADXL345_SAMPLE_LEN] = {0};
    uint8_t source = 0u;

    if (accel_dev_handle == NULL) {
        return;
    }

    (void)i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, raw, sizeof(raw));
    (void)i2c_register_read(accel_dev_handle, ADXL345_REG_INT_SOURCE, &source, 1u);
}

static void IRAM_ATTR adxl_isr_handler(void *arg)
{
    (void)arg;
    if (sync_task_handle != NULL) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(sync_task_handle, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

esp_err_t accel_setup_interrupt(TaskHandle_t task_to_notify)
{
    if (task_to_notify == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sync_task_handle = task_to_notify;
    const gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .pin_bit_mask = (1ULL << ACCEL_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(ACCEL_INT_PIN, adxl_isr_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* A device configured before GPIO setup may already assert DATA_READY. */
    accel_clear_pending_interrupt();
    return ESP_OK;
}

void accel_read_bytes(uint8_t *data, size_t len)
{
    if (data == NULL || len < ADXL345_SAMPLE_LEN || accel_dev_handle == NULL) {
        return;
    }
    (void)i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, data, ADXL345_SAMPLE_LEN);
}

uint8_t accel_get_int_source(void)
{
    uint8_t source = 0u;
    if (accel_dev_handle != NULL) {
        (void)i2c_register_read(accel_dev_handle, ADXL345_REG_INT_SOURCE, &source, 1u);
    }
    return source;
}

esp_err_t accel_init(void)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (accel_dev_handle != NULL) {
        return ESP_OK;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACCEL_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &accel_dev_handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t device_id = 0u;
    err = i2c_register_read(accel_dev_handle, ADXL345_REG_DEVID, &device_id, 1u);
    if (err != ESP_OK || device_id != ADXL345_DEVID_VALUE) {
        if (err == ESP_OK) {
            err = ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "ADXL345 identity failed: id=0x%02X err=%s",
                 device_id, esp_err_to_name(err));
        goto failed;
    }

    err = i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x00u);
    if (err == ESP_OK) {
        err = i2c_register_write_byte(accel_dev_handle, ADXL345_REG_BW_RATE, ADXL345_BW_RATE_800_HZ);
    }
    if (err == ESP_OK) {
        /* FULL_RES plus +/-16 g range. */
        err = i2c_register_write_byte(accel_dev_handle, ADXL345_REG_DATA_FORMAT, 0x0Bu);
    }
    if (err == ESP_OK) {
        err = i2c_register_write_byte(accel_dev_handle, ADXL345_REG_FIFO_CTL, 0x00u);
    }
    if (err == ESP_OK) {
        err = i2c_register_write_byte(accel_dev_handle, ADXL345_REG_INT_ENABLE, ADXL345_INT_DATA_READY);
    }
    if (err == ESP_OK) {
        err = i2c_register_write_byte(accel_dev_handle, ADXL345_REG_INT_MAP, 0x00u);
    }
    if (err == ESP_OK) {
        err = i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x08u);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADXL345 configuration failed: %s", esp_err_to_name(err));
        goto failed;
    }

    uint8_t bw_rate = 0u;
    uint8_t data_format = 0u;
    uint8_t fifo_ctl = 0u;
    uint8_t int_enable = 0u;
    uint8_t int_map = 0u;
    uint8_t power_ctl = 0u;
    err = i2c_register_read(accel_dev_handle, ADXL345_REG_BW_RATE, &bw_rate, 1u);
    if (err == ESP_OK) {
        err = i2c_register_read(accel_dev_handle, ADXL345_REG_DATA_FORMAT, &data_format, 1u);
    }
    if (err == ESP_OK) {
        err = i2c_register_read(accel_dev_handle, ADXL345_REG_FIFO_CTL, &fifo_ctl, 1u);
    }
    if (err == ESP_OK) {
        err = i2c_register_read(accel_dev_handle, ADXL345_REG_INT_ENABLE, &int_enable, 1u);
    }
    if (err == ESP_OK) {
        err = i2c_register_read(accel_dev_handle, ADXL345_REG_INT_MAP, &int_map, 1u);
    }
    if (err == ESP_OK) {
        err = i2c_register_read(accel_dev_handle, ADXL345_REG_POWER_CTL, &power_ctl, 1u);
    }
    if (err != ESP_OK || bw_rate != ADXL345_BW_RATE_800_HZ || data_format != 0x0Bu ||
        fifo_ctl != 0x00u || int_enable != ADXL345_INT_DATA_READY || int_map != 0x00u ||
        power_ctl != 0x08u) {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        ESP_LOGE(TAG,
                 "ADXL345 readback failed: bw=%02X fmt=%02X fifo=%02X ien=%02X map=%02X pwr=%02X",
                 bw_rate, data_format, fifo_ctl, int_enable, int_map, power_ctl);
        goto failed;
    }

    ESP_LOGI(TAG, "ADXL345 id=0x%02X, %u Hz, full-resolution +/-16 g",
             device_id, ACCEL_SAMPLE_RATE_HZ);
    return ESP_OK;

failed:
    (void)i2c_master_bus_rm_device(accel_dev_handle);
    accel_dev_handle = NULL;
    return err;
}

esp_err_t accel_get_raw_data(accel_raw_data_t *accel_data)
{
    if (accel_data == NULL || accel_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[ADXL345_SAMPLE_LEN] = {0};
    esp_err_t err = i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    /* A DATAX0..DATAZ1 read clears DATA_READY; no extra status transaction. */
    accel_data->x = (int16_t)(((uint16_t)raw[1] << 8u) | raw[0]);
    accel_data->y = (int16_t)(((uint16_t)raw[3] << 8u) | raw[2]);
    accel_data->z = (int16_t)(((uint16_t)raw[5] << 8u) | raw[4]);
    return ESP_OK;
}

esp_err_t accel_get_real_data(accel_data_t *accel_data)
{
    if (accel_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    accel_raw_data_t raw = {0};
    esp_err_t err = accel_get_raw_data(&raw);
    if (err != ESP_OK) {
        return err;
    }

    accel_data->x = (float)raw.x * ADXL345_FULL_RES_LSB_G;
    accel_data->y = (float)raw.y * ADXL345_FULL_RES_LSB_G;
    accel_data->z = (float)raw.z * ADXL345_FULL_RES_LSB_G;
    return ESP_OK;
}
