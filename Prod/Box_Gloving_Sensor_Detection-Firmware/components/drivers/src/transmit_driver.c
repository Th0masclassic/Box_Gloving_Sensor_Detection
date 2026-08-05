#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "host/ble_hs.h"

#include "bluetooth_driver.h"
#include "transmit_driver.h"

static const char *TAG = "TRANSMIT_DRIVER";
static int current_working_state = TRANSMIT_MODE_QUALITY;

extern uint8_t last_rx_frame[BLE_RX_MAX_LEN];
extern uint16_t last_rx_len;
extern volatile bool rx_frame_available;
extern volatile bool ble_connected;
extern volatile bool ble_disconnect;
extern bool notify_enabled;
extern uint16_t glove_data_val_handle;
extern uint16_t current_conn_handle;

void transmit_driver_init(void)
{
    /* Keeping your function name as-is because it seems to exist in your bluetooth_driver. */
    blueetooth_init();
}

bool can_send_data(void)
{
    return ble_connected &&
           current_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
           notify_enabled;
}

esp_err_t send_data(
    uint8_t device_id,
    const uint8_t *data,
    proto_msg_type_t type,
    uint16_t size,
    uint8_t sequence_number
)
{
    if (data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_connected || current_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "Cannot send: no BLE client connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!notify_enabled) {
        ESP_LOGW(TAG, "Cannot send: client has not enabled notifications");
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t frame_size = (uint16_t)(PROTO_HEADER_SIZE + size);

    uint8_t *frame = malloc(frame_size);
    if (frame == NULL) {
        ESP_LOGE(TAG, "Failed to allocate protocol frame");
        return ESP_ERR_NO_MEM;
    }

    frame[0] = device_id;
    frame[1] = (uint8_t)type;
    frame[2] = sequence_number;

    /* Payload size in little endian */
    frame[3] = (uint8_t)(size & 0xFF);
    frame[4] = (uint8_t)((size >> 8) & 0xFF);

    memcpy(&frame[PROTO_HEADER_SIZE], data, size);

    /* Useful while debugging protocol alignment. Remove later if too noisy. */
    ESP_LOGD(TAG, "Sending BLE frame: payload_size=%u total_size=%u", size, frame_size);
    ESP_LOG_BUFFER_HEXDUMP(TAG, frame, frame_size, ESP_LOG_DEBUG);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, frame_size);

    free(frame);

    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate BLE mbuf");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(
        current_conn_handle,
        glove_data_val_handle,
        om
    );

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_notify_custom failed: rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t send_raw_notification(const uint8_t *data, uint16_t size)
{
    if (data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_connected || current_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "Cannot send raw notification: no BLE client connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!notify_enabled) {
        ESP_LOGW(TAG, "Cannot send raw notification: client has not enabled notifications");
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, size);
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate raw BLE mbuf");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(
        current_conn_handle,
        glove_data_val_handle,
        om
    );

    if (rc != 0) {
        ESP_LOGE(TAG, "raw ble_gatts_notify_custom failed: rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t receive_data(uint8_t *buffer, uint16_t buffer_size, uint16_t *received_size)
{
    if (buffer == NULL || received_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!rx_frame_available) {
        *received_size = 0;
        return ESP_ERR_NOT_FOUND;
    }

    if (buffer_size < last_rx_len) {
        *received_size = last_rx_len;
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, last_rx_frame, last_rx_len);
    *received_size = last_rx_len;
    rx_frame_available = false;

    return ESP_OK;
}

esp_err_t set_working_state(int state)
{
    current_working_state = state;
    return ESP_OK;
}

int get_working_state(void)
{
    return current_working_state;
}
