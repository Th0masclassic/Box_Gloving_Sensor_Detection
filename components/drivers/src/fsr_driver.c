#include "fsr_driver.h"

static const char *TAG = "FSR";
static adc_oneshot_unit_handle_t adc_handle; // Handle do ADC.

esp_err_t fsr_init() {
    // Configura a unidade ADC.
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle)); // Inicializa o ADC.
    
    // Configura o canal do FSR.
    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT, // Resolucao padrao de 12 bits.
        .atten = ADC_ATTEN_DB_12, // Permite medir ate 3.3 V.
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, FSR_PIN0, &channel_config));

    ESP_LOGI(TAG, "FSR initialized successfully");
    return ESP_OK;
}

float read_fsr(adc_channel_t channel) {
    int adc_value = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, channel, &adc_value));

    int adc_inverted = 4095 - adc_value; // Inverte o valor do ADC
    // Aplica a fórmula da curva de calibração do sensor de 6kg:
    /*float peso_kg = (0.00136f * (float)adc_inverted) - 0.587f;

    // Filtro de segurança
    if (peso_kg < 0.0f) {
        peso_kg = 0.0f;
    }*/
    return adc_inverted;
}
