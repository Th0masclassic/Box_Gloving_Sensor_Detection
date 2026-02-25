#include "accelerometer_driver.h"


esp_err_t accel_init(){

    gpio_config_t accelpin;
    accelpin.pin_bit_mask = ACCEL_INIT_PIN;
    accelpin.mode = GPIO_MODE_INPUT;
    accelpin.pull_up_en = GPIO_PULLUP_DISABLE;
    accelpin.pull_down_en = GPIO_PULLDOWN_DISABLE;

    return gpio_config(&accelpin);

}