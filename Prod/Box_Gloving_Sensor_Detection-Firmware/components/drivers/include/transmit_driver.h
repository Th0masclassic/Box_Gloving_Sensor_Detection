#ifndef TRANSMIT_DRIVER_H
#define TRANSMIT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define TRANSMIT_MODE_QUALITY      0x01
#define TRANSMIT_MODE_NORMAL       0x02
#define TRANSMIT_MODE_PERFORMANCE  0x03

/*
 * Protocol frame sent through BLE notification:
 *
 * Byte 0      device_id
 * Byte 1      message type
 * Byte 2      sequence number
 * Byte 3      payload size low byte
 * Byte 4      payload size high byte
 * Byte 5...   payload bytes
 */
#define PROTO_HEADER_SIZE 5

/* Optional backwards-compatible alias if old code used HEADER_SIZE. */
#define HEADER_SIZE PROTO_HEADER_SIZE

/* Message type for communication */
typedef enum {
    PROTO_MSG_SETUP = 0x01,
    PROTO_MSG_DATA  = 0x02,
    PROTO_MSG_ERROR = 0x04
} proto_msg_type_t;

/* Older enum kept in case other files still include/use it. */
typedef enum {
    MSG_TYPE_SETUP = 0x01,
    MSG_TYPE_DATA  = 0x02,
    MSG_TYPE_ERROR = 0x04
} message_type_t;

void transmit_driver_init(void);

bool can_send_data(void);

esp_err_t send_data(
    uint8_t device_id,
    const uint8_t *data,
    proto_msg_type_t type,
    uint16_t size,
    uint8_t sequence_number
);

esp_err_t send_raw_notification(const uint8_t *data, uint16_t size);

esp_err_t receive_data(
    uint8_t *buffer,
    uint16_t buffer_size,
    uint16_t *received_size
);

esp_err_t set_working_state(int state);
int get_working_state(void);

#endif
