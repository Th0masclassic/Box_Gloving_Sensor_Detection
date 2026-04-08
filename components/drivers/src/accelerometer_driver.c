#include "accelerometer_driver.h"
#include "i2c_driver_init.h"
#include "esp_log.h"

static const char *TAG = "ACCEL";
//static uint16_t accel_raw_data[READ_BYTES_ARRAY_SZ];
static i2c_master_dev_handle_t accel_dev_handle = NULL; //identificador do accel no i2c
extern i2c_master_bus_handle_t bus_handle; 

SemaphoreHandle_t accel_sem = NULL; //flag do interrupt do acelerometro
static bool init = false;

void accel_read_bytes(uint8_t *data, size_t len) {
    if (data == NULL || len < ADXL345_SAMPLE_LEN) return;
    i2c_register_read(accel_dev_handle, ADXL345_REG_DATAX0, data, ADXL345_SAMPLE_LEN);
}

bool has_init(){ return init; }

uint8_t accel_get_int_source(void) {
    uint8_t int_source = 0;
    if (accel_dev_handle != NULL) {
        // 0x30 é o registo INT_SOURCE [cite: 2094, 2110]
        i2c_register_read(accel_dev_handle, 0x30, &int_source, 1);
    }
    return int_source;
}

// Lógica para lidar com a interrupção do acelerômetro
static void IRAM_ATTR accel_isr_handler(void* arg) {
    if (accel_sem != NULL) xSemaphoreGiveFromISR(accel_sem, NULL); // avisa a tarefa que os dados estão prontos
}

esp_err_t accel_init(){

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACCEL_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    accel_sem = xSemaphoreCreateBinary(); //criar interrupt

    //Adiciona sensor ao barramento i2c
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &accel_dev_handle);
    
    if (ret == ESP_OK) {

        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_POSEDGE, // Interrupção na borda de subida
            .mode = GPIO_MODE_INPUT,        // Configura o pino como entrada
            .pin_bit_mask = (1ULL << ACCEL_INT_PIN), // Máscara para o pino do interrupt
            .pull_down_en = 0,              // Desativa o pull-down
            .pull_up_en = 1                 // Ativa o pull-up
        };
        gpio_config(&io_conf); // Configura o pino do interrupt
    
        gpio_install_isr_service(0); // Instala o serviço de interrupção
        gpio_isr_handler_add(ACCEL_INT_PIN, accel_isr_handler, (void*) ACCEL_INT_PIN); // Adiciona o handler para o pino do interrupt
        
        // 1. Colocar em Standby e desligar interrupções enquanto configuramos (Recomendação do Datasheet)
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x00);
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_INT_ENABLE, 0x00); 
        
        // 2. Configurar os Registos
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_BW_RATE, 0x0A); // 100 Hz 
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_DATA_FORMAT, 0x00); // definir para a resolução 2g 
        i2c_register_write_byte(accel_dev_handle, 0x38, 0x00); // Modo FIFO Bypass
        i2c_register_write_byte(accel_dev_handle, 0x2F, 0x80); // Mapear Interrupt para o pino INT1
        
        // 3. Forçar leitura AGORA para limpar interrupções e baixar o pino para LOW (0V)
        uint8_t dummy[6];
        accel_read_bytes(dummy, 6);
        accel_get_int_source();
        
        // 4. Ligar os interrupts e o motor de medição
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_INT_ENABLE, 0x80); //ativa data ready interrupt
        i2c_register_write_byte(accel_dev_handle, ADXL345_REG_POWER_CTL, 0x08); // Definir o sensor em modo de medição 
        
        
        ESP_LOGI(TAG, "Accelerometer initialized successfully");
        init = true;
    }
    return ret;
}

void accel_get_real_data(accel_data_t *accel_data) {
    uint8_t raw_data[ADXL345_SAMPLE_LEN];
    accel_read_bytes(raw_data, ADXL345_SAMPLE_LEN);

    // Converter os dados brutos para valores de aceleração em g
    accel_data->x = (float)(int16_t)((raw_data[1] << 8) | raw_data[0]) * 0.0039; // 0.0039 é a escala para 2g
    accel_data->y = (float)(int16_t)((raw_data[3] << 8) | raw_data[2]) * 0.0039;
    accel_data->z = (float)(int16_t)((raw_data[5] << 8) | raw_data[4]) * 0.0039;

}