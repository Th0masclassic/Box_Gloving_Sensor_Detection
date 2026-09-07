/**
 * @file transmit_driver.h
 * @brief MTU-aware raw-notification transport for framed glove protocol data.
 */
#ifndef TRANSMIT_DRIVER_H
#define TRANSMIT_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initializes the BLE transport. */
esp_err_t transmit_driver_init(void);

/** True if the peer is connected and its notification CCCD is enabled. */
bool can_send_data(void);

/**
 * Sends one already-framed notification.  `size` must fit the negotiated ATT
 * payload (MTU - 3); it is never silently truncated.
 */
esp_err_t send_raw_notification(const uint8_t *data, uint16_t size);

/** Sends only if `expected_session_id` is still the active protocol session. */
esp_err_t send_raw_notification_for_session(
    uint16_t expected_session_id,
    const uint8_t *data,
    uint16_t size
);

#ifdef __cplusplus
}
#endif

#endif /* TRANSMIT_DRIVER_H */
