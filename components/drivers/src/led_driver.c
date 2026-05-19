#include "led_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

// Variável estática que guarda o estado
static volatile estado_luva_t estado_atual = ESTADO_BUSCA_BLE;

// Função pública para outros ficheiros mudarem o estado
void led_set_estado(estado_luva_t novo_estado) {
    estado_atual = novo_estado;
}

// A Task que controla o piscar 
static void led_task(void *pvParameter) {
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    while (1) {
        switch (estado_atual) {
            case ESTADO_BUSCA_BLE:
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case ESTADO_CONECTADO_BLE:
                gpio_set_level(LED_PIN, 0); // 0 liga o LED no ESP32-C3 SuperMini (active low)
                vTaskDelay(pdMS_TO_TICKS(200)); 
                break;

            case ESTADO_BATERIA_FRACA:
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

void led_init(void) {
    // Lança a Task do LED com prioridade baixa (1)
    xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);
}