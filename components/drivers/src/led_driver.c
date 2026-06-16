#include "led_driver.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_ON_LEVEL 0
#define LED_OFF_LEVEL 1

#define SEARCH_BLINK_MS 500

#define HEARTBEAT_ON_MS 80
#define HEARTBEAT_OFF_MS 1920

#define LOW_BATTERY_BLINK_ON_MS 80
#define LOW_BATTERY_BLINK_OFF_MS 120
#define LOW_BATTERY_PAUSE_MS 1800
#define LOW_BATTERY_BLINK_COUNT 3

static volatile estado_luva_t estado_atual = ESTADO_BUSCA_BLE;

static void led_on(void)
{
    gpio_set_level(LED_PIN, LED_ON_LEVEL);
}

static void led_off(void)
{
    gpio_set_level(LED_PIN, LED_OFF_LEVEL);
}

void led_set_estado(estado_luva_t novo_estado)
{
    estado_atual = novo_estado;
}

static void led_task(void *pvParameter)
{
    (void)pvParameter;

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    led_off();

    while (1) {
        switch (estado_atual) {
        case ESTADO_BUSCA_BLE:
            led_on();
            vTaskDelay(pdMS_TO_TICKS(SEARCH_BLINK_MS));
            led_off();
            vTaskDelay(pdMS_TO_TICKS(SEARCH_BLINK_MS));
            break;

        case ESTADO_CONECTADO_BLE:
            led_on();
            vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_ON_MS));
            led_off();
            vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_OFF_MS));
            break;

        case ESTADO_BATERIA_FRACA:
            for (int i = 0; i < LOW_BATTERY_BLINK_COUNT; i++) {
                led_on();
                vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_BLINK_ON_MS));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_BLINK_OFF_MS));
            }
            vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_PAUSE_MS));
            break;
        }
    }
}

void led_init(void)
{
    xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);
}
