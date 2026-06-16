#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#define LED_PIN 8

typedef enum {
    ESTADO_BUSCA_BLE,     // Pisca lento enquanto procura ligacao BLE.
    ESTADO_CONECTADO_BLE, // Heartbeat para indicar BLE ligado com baixo consumo.
    ESTADO_BATERIA_FRACA  // Heartbeat com 3 piscas para indicar bateria fraca.
} estado_luva_t;

/**
 * @brief Inicializa o pino do LED e arranca a task de controlo.
 */
void led_init(void);

/**
 * @brief Altera o padrao atual do LED.
 *
 * @param novo_estado Estado pretendido para o LED.
 */
void led_set_estado(estado_luva_t novo_estado);

#endif
