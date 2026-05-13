#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#define LED_PIN 8 // Pino do LED interno no ESP32-C3 SuperMini

// Estados possíveis para o LED da luva
typedef enum {
    ESTADO_BUSCA_BLE,     // Pisca lento
    ESTADO_CONECTADO_BLE, // Aceso fixo
    ESTADO_BATERIA_FRACA  // Pisca rápido
} estado_luva_t;

/**
 * @brief Inicializa o pino do LED e arranca a Task de controlo
 */
void led_init(void);

/**
 * @brief Altera o estado atual do LED (muda o padrão de piscar)
 * @param novo_estado O estado pretendido (ex: ESTADO_CONECTADO_BLE)
 */
void led_set_estado(estado_luva_t novo_estado);

#endif // LED_DRIVER_H