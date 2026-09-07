/**
 * @file transmit_driver.c
 * @brief Small, allocation-safe wrapper around NimBLE notifications.
 */
#include "transmit_driver.h"

#include "bluetooth_driver.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"

/* Defined by bluetooth_driver.c because it owns the GATT characteristic. */
extern uint16_t glove_data_val_handle;

esp_err_t transmit_driver_init(void)
{
    return bluetooth_init();
}

bool can_send_data(void)
{
    return glove_ble_can_notify();
}

esp_err_t send_raw_notification(const uint8_t *data, uint16_t size)
{
    glove_ble_session_t session = {0};
    if (!glove_ble_get_session(&session)) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_raw_notification_for_session(session.session_id, data, size);
}

esp_err_t send_raw_notification_for_session(
    uint16_t expected_session_id,
    const uint8_t *data,
    uint16_t size
)
{
    if (data == NULL || size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    glove_ble_session_t session = {0};
    if (!glove_ble_get_session(&session) || session.session_id != expected_session_id) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size > session.notification_payload_max) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, size);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Do not retain the session lock across the NimBLE call. */
    if (!glove_ble_session_matches(expected_session_id)) {
        os_mbuf_free_chain(om);
        return ESP_ERR_INVALID_STATE;
    }
    const int rc = ble_gatts_notify_custom(session.connection_handle, glove_data_val_handle, om);
    if (rc == 0) {
        return ESP_OK;
    }
    if (rc == BLE_HS_ENOMEM) {
        return ESP_ERR_NO_MEM;
    }
    if (rc == BLE_HS_ENOTCONN) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}
