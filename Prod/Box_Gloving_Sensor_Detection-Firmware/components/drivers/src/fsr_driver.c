/**
 * @file fsr_driver.c
 * @brief Fault-tolerant one-shot ADC access for the force-sensitive resistor.
 */
#include "fsr_driver.h"

static const char *TAG = "FSR";
static adc_oneshot_unit_handle_t adc_handle = NULL;

esp_err_t fsr_init(void)
{
    if (adc_handle != NULL) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        adc_handle = NULL;
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(adc_handle, FSR_PIN0, &channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc channel configuration failed: %s", esp_err_to_name(err));
        (void)adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "FSR ADC initialized on ADC1 channel %d", FSR_PIN0);
    return ESP_OK;
}

esp_err_t fsr_read_raw(adc_channel_t channel, uint16_t *adc_value)
{
    if (adc_handle == NULL || adc_value == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int value = 0;
    esp_err_t err = adc_oneshot_read(adc_handle, channel, &value);
    if (err != ESP_OK) {
        return err;
    }
    if (value < 0) {
        return ESP_FAIL;
    }
    if (value > 4095) {
        /* Preserve the measurement contract: never silently alter raw ADC data. */
        return ESP_ERR_INVALID_RESPONSE;
    }
    *adc_value = (uint16_t)value;
    return ESP_OK;
}

uint16_t fsr_raw_to_centi_kg(uint16_t adc_value)
{
    const uint16_t clamped_adc = adc_value > 4095u ? 4095u : adc_value;
    const float inverted = (float)(4095u - clamped_adc);
    const float centi_kg = (0.136f * inverted) - 58.7f;

    if (centi_kg <= 0.0f) {
        return 0u;
    }
    if (centi_kg >= 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)(centi_kg + 0.5f);
}

float read_fsr(adc_channel_t channel)
{
    uint16_t raw = 0u;
    if (fsr_read_raw(channel, &raw) != ESP_OK) {
        return 0.0f;
    }
    return (float)fsr_raw_to_centi_kg(raw) / 100.0f;
}
