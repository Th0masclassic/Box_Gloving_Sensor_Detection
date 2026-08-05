#ifndef SENSOR_PAYLOAD_CODEC_H
#define SENSOR_PAYLOAD_CODEC_H

#include <stdint.h>
#include <stdbool.h>

/*
    Internal sensor payload header: 1 byte

        bits 5..7  -> payload mode
        bits 0..4  -> count_minus_1

    count is encoded as count - 1:
        0  -> 1 sample
        31 -> 32 samples
*/

#define SENSOR_PAYLOAD_MAX_COUNT 32

#define SENSOR_HEADER_MODE_SHIFT   5
#define SENSOR_HEADER_COUNT_MASK   0x1F
#define SENSOR_HEADER_MODE_MASK    0x07

#define AXIS_COUNT_1D  1
#define AXIS_COUNT_3D  3

typedef enum {
    SENSOR_PAYLOAD_MODE_RAW16  = 0x00,  // full frame: every value is int16_t
    SENSOR_PAYLOAD_MODE_DIFF6  = 0x01,  // first sample int16_t, then signed 6-bit deltas
    SENSOR_PAYLOAD_MODE_DIFF8  = 0x02,  // first sample int16_t, then signed 8-bit deltas
    SENSOR_PAYLOAD_MODE_DIFF10 = 0x03,  // first sample int16_t, then signed 10-bit deltas
    SENSOR_PAYLOAD_MODE_SETUP  = 0x04,
    SENSOR_PAYLOAD_MODE_META   = 0x05,
    SENSOR_PAYLOAD_MODE_ACK    = 0x06   // setup acknowledgement only
} sensor_payload_mode_t;


typedef struct {
    uint8_t *buffer;
    int max_bytes;
    int bit_pos;
} bit_writer_t;


uint8_t sensor_payload_make_header(uint8_t mode, uint8_t count);
uint8_t sensor_payload_get_mode(uint8_t header);
uint8_t sensor_payload_get_count(uint8_t header);

/* ---------------- 1-axis API, kept for compatibility ---------------- */

int sensor_payload_raw16_size_1axis(int count);
int sensor_payload_diff_size_1axis(int count, int bits);

int sensor_payload_encode_i16_1axis_raw16(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count
);

int sensor_payload_encode_i16_1axis_diff_bits(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count,
    int bits,
    int16_t deadband
);

int sensor_payload_encode_i16_1axis_best(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count,
    int16_t deadband
);

/* ---------------- 3-axis XYZ API ----------------

    samples_xyz is interleaved:

        samples_xyz[0] = X0
        samples_xyz[1] = Y0
        samples_xyz[2] = Z0

        samples_xyz[3] = X1
        samples_xyz[4] = Y1
        samples_xyz[5] = Z1

        ...

    count is the number of XYZ samples, not the number of int16_t values.
*/

int sensor_payload_raw16_size_3axis(int count);
int sensor_payload_diff_size_3axis(int count, int bits);

int sensor_payload_encode_i16_3axis_raw16(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples_xyz,
    int count
);

int sensor_payload_encode_i16_3axis_diff_bits(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples_xyz,
    int count,
    int bits,
    int16_t deadband
);

int sensor_payload_encode_i16_3axis_best(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples_xyz,
    int count,
    int16_t deadband
);

#endif // SENSOR_PAYLOAD_CODEC_H
