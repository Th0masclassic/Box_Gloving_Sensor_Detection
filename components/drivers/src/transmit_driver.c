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

static esp_err_t InitBLE(void);
static void on_sync(void);
static void on_reset(int reason);
static void start_advertising(void);

volatile bool ble_disconnect = false;
volatile bool ble_connected = false;

/*
 * UUID que aparece no Python/Bleak:
 *
 * 12345678-1234-5678-1234-56789abcdef0
 *
 * No NimBLE, o UUID128 é colocado em little endian.
 */
static const ble_uuid128_t glove_service_uuid =
    BLE_UUID128_INIT(
        0xf0, 0xde, 0xbc, 0x9a,
        0x78, 0x56,
        0x34, 0x12,
        0x34, 0x12,
        0x78, 0x56,
        0x34, 0x12,
        0x78, 0x56
    );

/*
 * Characteristic UUID:
 *
 * 12345678-1234-5678-1234-56789abcdef1
 */
static const ble_uuid128_t glove_data_char_uuid =
    BLE_UUID128_INIT(
        0xf1, 0xde, 0xbc, 0x9a,
        0x78, 0x56,
        0x34, 0x12,
        0x34, 0x12,
        0x78, 0x56,
        0x34, 0x12,
        0x78, 0x56
    );

static uint16_t glove_data_val_handle;

static int glove_data_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        const char *msg = "Boxing glove ready";
        os_mbuf_append(ctxt->om, msg, strlen(msg));
        return 0;
    }

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ESP_LOGI(TAG, "Received data from client");
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &glove_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &glove_data_char_uuid.u,
                .access_cb = glove_data_access_cb,
                .flags = BLE_GATT_CHR_F_READ |
                         BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &glove_data_val_handle,
            },
            {
                0
            }
        },
    },
    {
        0
    }
};

static void ble_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
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
static void start_advertising(void)
{
    int rc;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Advertise custom service UUID
    fields.uuids128 = &glove_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    const char *device_name = DEVICE_NAME;

    // Device name goes in scan response to avoid exceeding 31 bytes
    rsp_fields.name = (const uint8_t *)device_name;
    rsp_fields.name_len = strlen(device_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
        own_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        gap_event_cb,
        NULL
    );

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started");
}
// This function is called on Init and will be to start advertising the BLE
static void on_sync(void)
{
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
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE is being reset %d", reason);
}

static esp_err_t InitBLE(void)
{
    int rc;

    // Initialize NimBLE host stack
    rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed; rc=%d", rc);
        return ESP_FAIL;
    }

    // Initialize default GAP and GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Register custom GATT service
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed; rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed; rc=%d", rc);
        return ESP_FAIL;
    }

    // Set the BLE device name
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
    if (!has_init_nvs()) {
        ESP_ERROR_CHECK(init_nvs());
    }

    ESP_ERROR_CHECK(InitBLE());

    return ESP_OK;
}