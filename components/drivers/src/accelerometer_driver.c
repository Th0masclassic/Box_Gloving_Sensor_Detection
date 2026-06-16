#include "accelerometer_driver.h"

static const char *TAG = "ACCEL";

static i2c_master_dev_handle_t accel_dev_handle = NULL; // Handle do acelerometro.
extern i2c_master_bus_handle_t bus_handle;
// Variável estática para o driver saber que Task deve acordar
static TaskHandle_t sync_task_handle = NULL;

static void accel_clear_pending_interrupt(void) {
    uint8_t dummy_data[ADXL345_SAMPLE_LEN] = {0};
    uint8_t int_source = 0;

    if (accel_dev_handle == NULL) {
        return;
    }

    (void)i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, dummy_data, sizeof(dummy_data));
    (void)i2c_register_read(accel_dev_handle, ADXL345_REG_INT_SOURCE, &int_source, 1);
}

static void IRAM_ATTR adxl_isr_handler(void* arg) {
    if (sync_task_handle != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(sync_task_handle, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

// A nova função que configura o pino e a ISR
esp_err_t accel_setup_interrupt(TaskHandle_t task_to_notify) {
    if (task_to_notify == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 1. Guarda a Task que vai ser acordada
    sync_task_handle = task_to_notify;

    // 2. Configura o pino GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE, // Dispara de 0V para 3.3V
        .pin_bit_mask = (1ULL << ACCEL_INT_PIN), // Pino 6[cite: 6]
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    // 3. Instala o serviço e anexa a ISR
    // Nota: O ESP32 pode queixar-se se tentares instalar o serviço de ISR duas vezes.
    // O ideal é usar a flag ESP_INTR_FLAG_IRAM, mas a instalação básica serve se for a única.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
        
    err = gpio_isr_handler_add(ACCEL_INT_PIN, adxl_isr_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    // O ADXL345 pode ja ter DATA_READY ativo desde accel_init().
    // Limpar depois de armar o GPIO garante que o proximo flanco e capturado.
    accel_clear_pending_interrupt();

    return ESP_OK;
}

void accel_read_bytes(uint8_t *data, size_t len) {
    if (data == NULL || len < ADXL345_SAMPLE_LEN) return;
    i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, data, ADXL345_SAMPLE_LEN);
}

uint8_t accel_get_int_source(void) {
    uint8_t int_source = 0;
    if (accel_dev_handle != NULL) {
        i2c_register_read(accel_dev_handle, ADXL345_REG_INT_SOURCE, &int_source, 1); // Le o registo INT_SOURCE.
    }
    return int_source;
}

esp_err_t accel_init(){

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACCEL_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    // Adiciona o acelerometro ao barramento I2C.
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &accel_dev_handle);
    
    if (ret == ESP_OK) {
        // Entra em standby para configurar.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x00);
        
        // Configura os registos principais.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_BW_RATE, 0x0A); // Taxa de 100 Hz.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_DATA_FORMAT, 0x0B); // Faixa de +/-16 g.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_FIFO_CTL, 0x00); // FIFO em bypass.
        
        // Ativa a interrupção DATA_READY (bit 7 do registo INT_ENABLE)
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_INT_ENABLE, ADXL345_INT_DATA_READY);
        // Mapeia a interrupção DATA_READY para o pino INT1 (escrever 0x00 no registo INT_MAP)[cite: 9]
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_INT_MAP, 0x00);

        // Ativa o modo de medicao.
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x08);
        
        ESP_LOGI(TAG, "Accelerometer initialized successfully (DATA_READY interrupt enabled)");
    }
    return ret;
}

esp_err_t accel_get_real_data(accel_data_t *accel_data) {
    if (accel_data == NULL || accel_dev_handle == NULL) {
        return ESP_FAIL;
    }

    uint8_t raw_data[ADXL345_SAMPLE_LEN];
    esp_err_t ret = i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, raw_data, ADXL345_SAMPLE_LEN);
    if (ret != ESP_OK) {
        return ret;
    }
    // Ler o registo INT_SOURCE (0x30) força o sensor a baixar o pino INT1[cite: 9]
    (void)accel_get_int_source();
    // Converte os valores raw para g.
    accel_data->x = (float)(int16_t)((raw_data[1] << 8) | raw_data[0]) * 0.0039;
    accel_data->y = (float)(int16_t)((raw_data[3] << 8) | raw_data[2]) * 0.0039;
    accel_data->z = (float)(int16_t)((raw_data[5] << 8) | raw_data[4]) * 0.0039;

    return ESP_OK;
}
