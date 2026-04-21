#include "transmit_driver.h"
#include "nvs_driver.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "TRANSMIT_DRIVER";
static uint8_t own_addr_type;

static esp_err_t InitBLE();
static void on_sync();
static void on_reset(int reason);
static void start_advertising();

volatile bool ble_disconnect = false;
volatile bool ble_connected = false;

static void ble_task(void *param)
{
    nimble_port_run();              // Runs the NimBLE host loop
    nimble_port_freertos_deinit();  // Cleanup when it stops
}

// Switch Function that will allow us to know if device is connected / disconnected / error
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Client connected");
            ble_connected = true;
            ble_disconnect = false;
        } else {
            ESP_LOGW(TAG, "Connection failed restarting advertising");
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ble_connected = false;
        ble_disconnect = true;
        ESP_LOGI(TAG, "Client disconnected restarting advertising");
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete restarting advertising");
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

// Advertising Function. Call to start
static void start_advertising(){
    
    int rc;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    const char *device_name = DEVICE_NAME;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Complete device name
    fields.name = (const uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));

    
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type,NULL,BLE_HS_FOREVER,&adv_params,gap_event_cb,NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started");

}

// This function is called on Init and will be to start advertising the BLE
static void on_sync()
{
    // Start Advertising
    int rc;

    // Ensure we have a valid BLE address
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    // Figure out which address type to use
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    start_advertising();

}

// This function is called when a reset happens 
static void on_reset(int reason){
    ESP_LOGE(TAG,"NimBLE is being reset %d",reason);
}


static esp_err_t InitBLE()
{
    int rc;

    
    // Initialize NimBLE host stack
    nimble_port_init();

    // Initialize default GAP and GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Set the BLE device name */
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set device name; rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Start NimBLE host task
    nimble_port_freertos_init(ble_task);

    ESP_LOGI(TAG, "BLE initialization completed");

    return ESP_OK;
}


esp_err_t init_transmit_driver(void)
{
    // NimBLE requires NVS to be initialized first
    if (!has_init_nvs()) ESP_ERROR_CHECK(init_nvs());

    ESP_ERROR_CHECK(InitBLE());

    return ESP_OK;
}