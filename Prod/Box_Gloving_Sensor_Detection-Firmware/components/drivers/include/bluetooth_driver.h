/**
 * @file bluetooth_driver.h
 * @brief NimBLE GATT ownership and negotiated v2-session state.
 */
#ifndef BLUETOOTH_DRIVER_H
#define BLUETOOTH_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GLOVE_BLE_DEVICE_NAME "SMART_BOXING_GLOVE"
#define GLOVE_BLE_RX_MAX_LEN 32u
/* ATT MTU 247 leaves one 244-byte notification in a 251-octet LL PDU. */
#define GLOVE_BLE_MAX_NOTIFICATION_PAYLOAD 244u

/** A coherent, lock-protected snapshot of the active BLE protocol session. */
typedef struct {
    bool active;
    uint16_t session_id;
    uint16_t connection_handle;
    uint16_t notification_payload_max;
    uint8_t capabilities;
} glove_ble_session_t;

/** Initializes the NimBLE peripheral and starts advertising after host sync. */
esp_err_t bluetooth_init(void);

/** Compatibility spelling retained for callers from earlier firmware revisions. */
void blueetooth_init(void);

/** True after link connection and notification subscription. */
bool glove_ble_can_notify(void);

/** True only after a valid, subscribed `GB\x02<capabilities>` HELLO write. */
bool glove_ble_protocol_is_active(void);

/** Current protocol session.  It changes on every accepted HELLO and disconnect. */
uint16_t glove_ble_session_id(void);

/** Capability mask supplied by the HELLO that created the active session. */
uint8_t glove_ble_capabilities(void);

/** Current negotiated ATT notification payload limit, clamped to local buffers. */
uint16_t glove_ble_notification_payload_max(void);

/** Returns the active NimBLE connection handle, or BLE_HS_CONN_HANDLE_NONE. */
uint16_t glove_ble_connection_handle(void);

/** Copies a coherent session snapshot and returns whether that session is active. */
bool glove_ble_get_session(glove_ble_session_t *session);

/** Tests an expected session without retaining an internal lock across BLE calls. */
bool glove_ble_session_matches(uint16_t expected_session_id);

#ifdef __cplusplus
}
#endif

#endif /* BLUETOOTH_DRIVER_H */
