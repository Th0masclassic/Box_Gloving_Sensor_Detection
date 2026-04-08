#include "fsr_driver.h"

static const char *TAG = "FSR";
static adc_oneshot_unit_handle_t adc_handle; //identificador do adc

esp_err_t fsr_init() {
    adc_oneshot_unit_init_cfg_t init_config = { //config do ADC1 (gpio0 usa ADC1 datasheet pag.21)
        .unit_id = ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle)); //inicializa o adc
    
    adc_oneshot_chan_cfg_t channel_config = { //config do canal
        .bitwidth = ADC_BITWIDTH_DEFAULT, //12 bits é o default (0-4095 niveis)
        .atten = ADC_ATTEN_DB_12, //atenuação de 12dB para medir até 3.3V
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_PIN, &channel_config)); //configura o canal
    
    ESP_LOGI(TAG, "FSR initialized successfully");
    return ESP_OK;
}

int read_fsr() {
    int adc_value = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_PIN, &adc_value)); //leitura do valor do ADC
    ESP_LOGI(TAG, "FSR reading: %d", adc_value);
    return 4095-adc_value; //divisor de tensao com resistencia pull up por isso fica invertido, maior força possivel = 0V, menor força = 3.3V
}