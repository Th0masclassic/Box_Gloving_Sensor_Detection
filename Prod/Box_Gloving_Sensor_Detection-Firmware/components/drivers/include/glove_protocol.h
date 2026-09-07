/**
 * @file glove_protocol.h
 * @brief Portable encoder definitions for the Smart Boxing Glove v2 BLE protocol.
 *
 * The protocol is deliberately byte-oriented: callers must never send native C
 * structures over BLE.  Every multibyte field is little endian and every
 * notification begins with the six-byte @ref glove_protocol_frame_header_t
 * layout described here.  The declarations have no ESP-IDF dependency so the
 * same codec can be tested on a desktop receiver.
 */
#ifndef GLOVE_PROTOCOL_H
#define GLOVE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** First byte of every outgoing v2 notification. */
#define GLOVE_PROTOCOL_MAGIC 0xB2u
/** Current wire-protocol version. */
#define GLOVE_PROTOCOL_VERSION 2u
/** Number of bytes in every outgoing frame header. */
#define GLOVE_PROTOCOL_FRAME_HEADER_SIZE 6u
/** BLE ATT payload available before MTU exchange. */
#define GLOVE_PROTOCOL_MIN_ATT_PAYLOAD 20u

/** Four-byte control write that enables a v2 session. */
#define GLOVE_PROTOCOL_HELLO_MAGIC_0 ((uint8_t)'G')
#define GLOVE_PROTOCOL_HELLO_MAGIC_1 ((uint8_t)'B')
#define GLOVE_PROTOCOL_HELLO_SIZE 4u
#define GLOVE_PROTOCOL_SUPPORTED_CAPABILITIES \
    (GLOVE_PROTOCOL_CAP_EVENTS | GLOVE_PROTOCOL_CAP_CAPTURES | GLOVE_PROTOCOL_CAP_STATS)

/** Capture blob marker, encoded at the start of a completed raw capture. */
#define GLOVE_CAPTURE_BLOB_MAGIC_0 ((uint8_t)'G')
#define GLOVE_CAPTURE_BLOB_MAGIC_1 ((uint8_t)'C')
#define GLOVE_CAPTURE_BLOB_VERSION 1u
#define GLOVE_CAPTURE_BLOB_HEADER_SIZE 48u
#define GLOVE_CAPTURE_RECORD_SIZE 26u

/** Capabilities requested by the client in a HELLO control write. */
typedef enum {
    GLOVE_PROTOCOL_CAP_EVENTS = 1u << 0,
    GLOVE_PROTOCOL_CAP_CAPTURES = 1u << 1,
    GLOVE_PROTOCOL_CAP_STATS = 1u << 2,
} glove_protocol_capability_t;

/** Types carried in a framed notification. */
typedef enum {
    GLOVE_PROTOCOL_MSG_HELLO_ACK = 0x01,
    GLOVE_PROTOCOL_MSG_EVENT = 0x10,
    GLOVE_PROTOCOL_MSG_CAPTURE_INFO = 0x11,
    GLOVE_PROTOCOL_MSG_CAPTURE_DATA = 0x12,
    GLOVE_PROTOCOL_MSG_STATS = 0x13,
    GLOVE_PROTOCOL_MSG_ERROR = 0x7F,
} glove_protocol_message_type_t;

/** Event flags reported with the EVENT (`0x10`) notification. */
typedef enum {
    GLOVE_EVENT_FLAG_FSR_VALID = 1u << 0,
    GLOVE_EVENT_FLAG_CAPTURE_ALLOCATED = 1u << 1,
    GLOVE_EVENT_FLAG_CAPTURE_DROPPED = 1u << 2,
} glove_event_flag_t;

/** Per-record validity and freshness bits. */
typedef enum {
    GLOVE_SAMPLE_FLAG_ACCEL_VALID = 1u << 0,
    GLOVE_SAMPLE_FLAG_GYRO_VALID = 1u << 1,
    GLOVE_SAMPLE_FLAG_MAG_VALID = 1u << 2,
    GLOVE_SAMPLE_FLAG_FSR_VALID = 1u << 3,
    GLOVE_SAMPLE_FLAG_ACCEL_STALE = 1u << 4,
    GLOVE_SAMPLE_FLAG_GYRO_STALE = 1u << 5,
    GLOVE_SAMPLE_FLAG_MAG_STALE = 1u << 6,
    GLOVE_SAMPLE_FLAG_DRDY_TIMEOUT = 1u << 7,
    GLOVE_SAMPLE_FLAG_DELTA_SATURATED = 1u << 8,
    GLOVE_SAMPLE_FLAG_ACCEL_SATURATED = 1u << 9,
    GLOVE_SAMPLE_FLAG_GYRO_SATURATED = 1u << 10,
    GLOVE_SAMPLE_FLAG_MAG_OVERFLOW = 1u << 11,
} glove_sample_flag_t;

/** Capture-level flags encoded in capture-info and blob metadata. */
typedef enum {
    GLOVE_CAPTURE_FLAG_CALIBRATED = 1u << 0,
    GLOVE_CAPTURE_FLAG_SHORT_PRETRIGGER = 1u << 1,
    GLOVE_CAPTURE_FLAG_TIMESTAMP_SATURATED = 1u << 2,
} glove_capture_flag_t;

/**
 * Raw sensor record held in firmware RAM before byte encoding.
 *
 * Accelerometer, gyroscope, and magnetometer values are native two's-complement
 * sensor counts.  `delta_us` is the interval from the preceding record (zero
 * for the first record).  No field is calibrated or clipped.
 */
typedef struct {
    uint32_t delta_us;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    int16_t mag_x;
    int16_t mag_y;
    int16_t mag_z;
    uint16_t fsr_adc;
    uint16_t validity_flags;
} glove_raw_sample_t;

/** Metadata encoded before capture records in the capture blob. */
typedef struct {
    uint16_t capture_id;
    uint32_t trigger_time_us;
    uint32_t first_sample_time_us;
    uint32_t nominal_sample_period_us;
    uint16_t sample_count;
    uint16_t trigger_index;
    uint16_t pretrigger_samples;
    uint16_t posttrigger_samples;
    uint16_t capture_flags;
    int16_t accel_offset[3];
    int16_t gyro_offset[3];
    int16_t mag_reference[3];
} glove_capture_metadata_t;

/** A decoded/constructed frame header.  It is never copied as a C struct. */
typedef struct {
    uint8_t type;
    uint16_t session_id;
    uint16_t sequence;
} glove_protocol_frame_header_t;

/** Writes a six-byte frame header and returns the resulting byte count. */
size_t glove_protocol_encode_frame_header(
    uint8_t *buffer,
    size_t buffer_size,
    uint8_t type,
    uint16_t session_id,
    uint16_t sequence
);

/** Parses a frame header, rejecting an incorrect magic byte or a short frame. */
bool glove_protocol_decode_frame_header(
    const uint8_t *buffer,
    size_t buffer_size,
    glove_protocol_frame_header_t *header
);

/** Validates and extracts the capabilities from a four-byte HELLO write. */
bool glove_protocol_parse_hello(const uint8_t *buffer, size_t buffer_size, uint8_t *capabilities);

/** Encodes the fixed 48-byte capture blob header. */
size_t glove_protocol_encode_capture_header(
    uint8_t *buffer,
    size_t buffer_size,
    const glove_capture_metadata_t *metadata
);

/** Decodes the fixed capture blob header. */
bool glove_protocol_decode_capture_header(
    const uint8_t *buffer,
    size_t buffer_size,
    glove_capture_metadata_t *metadata
);

/** Encodes one fixed-size raw sample record. */
size_t glove_protocol_encode_capture_record(
    uint8_t *buffer,
    size_t buffer_size,
    const glove_raw_sample_t *sample
);

/** Decodes one fixed-size raw sample record. */
bool glove_protocol_decode_capture_record(
    const uint8_t *buffer,
    size_t buffer_size,
    glove_raw_sample_t *sample
);

/** Returns the total number of blob bytes needed for a record count, or zero on overflow. */
size_t glove_protocol_capture_blob_size(uint16_t sample_count);

/**
 * Wrap-safe comparison for a 32-bit microsecond deadline whose interval is
 * shorter than 2^31 microseconds.  It is useful for BLE retry and telemetry
 * scheduling and deliberately works across the ESP uptime timestamp wrap.
 */
bool glove_protocol_time_reached_u32(uint32_t now, uint32_t deadline);

#ifdef __cplusplus
}
#endif

#endif /* GLOVE_PROTOCOL_H */
