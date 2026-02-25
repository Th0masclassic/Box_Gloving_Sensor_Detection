#include "transmit_driver.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"


static esp_err_t wifi_init(){
    // Inicialize TCP/IP
    ESP_ERR_CHECK(esp_netif_init);
    ESP_ERR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Setup
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Station Mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());

    // Set the channel
    ESP_ERROR_CHECK(esp_wifi_set_channel(CHANNEL_ID, WIFI_SECOND_CHAN_NONE));

}

esp_err_t init_transmit_driver(){

    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(esp_now_init());
    
    // Continuar aqui agora

    return ESP_OK;
}