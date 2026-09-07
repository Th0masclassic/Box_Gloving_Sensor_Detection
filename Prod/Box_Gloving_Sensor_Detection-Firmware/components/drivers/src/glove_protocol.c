/**
 * @file glove_protocol.c
 * @brief Portable byte encoder/decoder for the v2 protocol.
 */
#include "glove_protocol.h"

#include <limits.h>

static void write_u16_le(uint8_t *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFFu);
    buffer[offset + 1u] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t *buffer, size_t offset, uint32_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFFu);
    buffer[offset + 1u] = (uint8_t)((value >> 8u) & 0xFFu);
    buffer[offset + 2u] = (uint8_t)((value >> 16u) & 0xFFu);
    buffer[offset + 3u] = (uint8_t)((value >> 24u) & 0xFFu);
}

static uint16_t read_u16_le(const uint8_t *buffer, size_t offset)
{
    return (uint16_t)buffer[offset] | ((uint16_t)buffer[offset + 1u] << 8u);
}

static uint32_t read_u32_le(const uint8_t *buffer, size_t offset)
{
    return (uint32_t)buffer[offset] |
           ((uint32_t)buffer[offset + 1u] << 8u) |
           ((uint32_t)buffer[offset + 2u] << 16u) |
           ((uint32_t)buffer[offset + 3u] << 24u);
}

size_t glove_protocol_encode_frame_header(
    uint8_t *buffer,
    size_t buffer_size,
    uint8_t type,
    uint16_t session_id,
    uint16_t sequence
)
{
    if (buffer == NULL || buffer_size < GLOVE_PROTOCOL_FRAME_HEADER_SIZE) {
        return 0u;
    }

    buffer[0] = GLOVE_PROTOCOL_MAGIC;
    buffer[1] = type;
    write_u16_le(buffer, 2u, session_id);
    write_u16_le(buffer, 4u, sequence);
    return GLOVE_PROTOCOL_FRAME_HEADER_SIZE;
}

bool glove_protocol_decode_frame_header(
    const uint8_t *buffer,
    size_t buffer_size,
    glove_protocol_frame_header_t *header
)
{
    if (buffer == NULL || header == NULL || buffer_size < GLOVE_PROTOCOL_FRAME_HEADER_SIZE ||
        buffer[0] != GLOVE_PROTOCOL_MAGIC) {
        return false;
    }

    header->type = buffer[1];
    header->session_id = read_u16_le(buffer, 2u);
    header->sequence = read_u16_le(buffer, 4u);
    return true;
}

bool glove_protocol_parse_hello(const uint8_t *buffer, size_t buffer_size, uint8_t *capabilities)
{
    if (buffer == NULL || capabilities == NULL || buffer_size != GLOVE_PROTOCOL_HELLO_SIZE ||
        buffer[0] != GLOVE_PROTOCOL_HELLO_MAGIC_0 ||
        buffer[1] != GLOVE_PROTOCOL_HELLO_MAGIC_1 ||
        buffer[2] != GLOVE_PROTOCOL_VERSION) {
        return false;
    }

    if (buffer[3] == 0u ||
        (buffer[3] & (uint8_t)~GLOVE_PROTOCOL_SUPPORTED_CAPABILITIES) != 0u) {
        return false;
    }

    *capabilities = buffer[3];
    return true;
}

size_t glove_protocol_encode_capture_header(
    uint8_t *buffer,
    size_t buffer_size,
    const glove_capture_metadata_t *metadata
)
{
    if (buffer == NULL || metadata == NULL || buffer_size < GLOVE_CAPTURE_BLOB_HEADER_SIZE) {
        return 0u;
    }

    buffer[0] = GLOVE_CAPTURE_BLOB_MAGIC_0;
    buffer[1] = GLOVE_CAPTURE_BLOB_MAGIC_1;
    buffer[2] = GLOVE_CAPTURE_BLOB_VERSION;
    buffer[3] = GLOVE_CAPTURE_BLOB_HEADER_SIZE;
    write_u16_le(buffer, 4u, metadata->capture_id);
    write_u32_le(buffer, 6u, metadata->trigger_time_us);
    write_u32_le(buffer, 10u, metadata->first_sample_time_us);
    write_u32_le(buffer, 14u, metadata->nominal_sample_period_us);
    write_u16_le(buffer, 18u, metadata->sample_count);
    write_u16_le(buffer, 20u, metadata->trigger_index);
    write_u16_le(buffer, 22u, metadata->pretrigger_samples);
    write_u16_le(buffer, 24u, metadata->posttrigger_samples);
    buffer[26] = GLOVE_CAPTURE_RECORD_SIZE;
    buffer[27] = 1u; /* sensor representation: native signed raw counts */
    write_u16_le(buffer, 28u, metadata->capture_flags);

    for (size_t axis = 0u; axis < 3u; axis++) {
        write_u16_le(buffer, 30u + axis * 2u, (uint16_t)metadata->accel_offset[axis]);
        write_u16_le(buffer, 36u + axis * 2u, (uint16_t)metadata->gyro_offset[axis]);
        write_u16_le(buffer, 42u + axis * 2u, (uint16_t)metadata->mag_reference[axis]);
    }

    return GLOVE_CAPTURE_BLOB_HEADER_SIZE;
}

bool glove_protocol_decode_capture_header(
    const uint8_t *buffer,
    size_t buffer_size,
    glove_capture_metadata_t *metadata
)
{
    if (buffer == NULL || metadata == NULL || buffer_size < GLOVE_CAPTURE_BLOB_HEADER_SIZE ||
        buffer[0] != GLOVE_CAPTURE_BLOB_MAGIC_0 ||
        buffer[1] != GLOVE_CAPTURE_BLOB_MAGIC_1 ||
        buffer[2] != GLOVE_CAPTURE_BLOB_VERSION ||
        buffer[3] != GLOVE_CAPTURE_BLOB_HEADER_SIZE ||
        buffer[26] != GLOVE_CAPTURE_RECORD_SIZE || buffer[27] != 1u) {
        return false;
    }

    metadata->capture_id = read_u16_le(buffer, 4u);
    metadata->trigger_time_us = read_u32_le(buffer, 6u);
    metadata->first_sample_time_us = read_u32_le(buffer, 10u);
    metadata->nominal_sample_period_us = read_u32_le(buffer, 14u);
    metadata->sample_count = read_u16_le(buffer, 18u);
    metadata->trigger_index = read_u16_le(buffer, 20u);
    metadata->pretrigger_samples = read_u16_le(buffer, 22u);
    metadata->posttrigger_samples = read_u16_le(buffer, 24u);
    metadata->capture_flags = read_u16_le(buffer, 28u);

    if (metadata->sample_count == 0u || metadata->nominal_sample_period_us == 0u ||
        metadata->trigger_index != metadata->pretrigger_samples ||
        metadata->trigger_index >= metadata->sample_count ||
        (uint32_t)metadata->pretrigger_samples + 1u + metadata->posttrigger_samples !=
            metadata->sample_count) {
        return false;
    }

    for (size_t axis = 0u; axis < 3u; axis++) {
        metadata->accel_offset[axis] = (int16_t)read_u16_le(buffer, 30u + axis * 2u);
        metadata->gyro_offset[axis] = (int16_t)read_u16_le(buffer, 36u + axis * 2u);
        metadata->mag_reference[axis] = (int16_t)read_u16_le(buffer, 42u + axis * 2u);
    }

    return true;
}

size_t glove_protocol_encode_capture_record(
    uint8_t *buffer,
    size_t buffer_size,
    const glove_raw_sample_t *sample
)
{
    if (buffer == NULL || sample == NULL || buffer_size < GLOVE_CAPTURE_RECORD_SIZE) {
        return 0u;
    }

    write_u32_le(buffer, 0u, sample->delta_us);
    write_u16_le(buffer, 4u, (uint16_t)sample->accel_x);
    write_u16_le(buffer, 6u, (uint16_t)sample->accel_y);
    write_u16_le(buffer, 8u, (uint16_t)sample->accel_z);
    write_u16_le(buffer, 10u, (uint16_t)sample->gyro_x);
    write_u16_le(buffer, 12u, (uint16_t)sample->gyro_y);
    write_u16_le(buffer, 14u, (uint16_t)sample->gyro_z);
    write_u16_le(buffer, 16u, (uint16_t)sample->mag_x);
    write_u16_le(buffer, 18u, (uint16_t)sample->mag_y);
    write_u16_le(buffer, 20u, (uint16_t)sample->mag_z);
    write_u16_le(buffer, 22u, sample->fsr_adc);
    write_u16_le(buffer, 24u, sample->validity_flags);
    return GLOVE_CAPTURE_RECORD_SIZE;
}

bool glove_protocol_decode_capture_record(
    const uint8_t *buffer,
    size_t buffer_size,
    glove_raw_sample_t *sample
)
{
    if (buffer == NULL || sample == NULL || buffer_size < GLOVE_CAPTURE_RECORD_SIZE) {
        return false;
    }

    sample->delta_us = read_u32_le(buffer, 0u);
    sample->accel_x = (int16_t)read_u16_le(buffer, 4u);
    sample->accel_y = (int16_t)read_u16_le(buffer, 6u);
    sample->accel_z = (int16_t)read_u16_le(buffer, 8u);
    sample->gyro_x = (int16_t)read_u16_le(buffer, 10u);
    sample->gyro_y = (int16_t)read_u16_le(buffer, 12u);
    sample->gyro_z = (int16_t)read_u16_le(buffer, 14u);
    sample->mag_x = (int16_t)read_u16_le(buffer, 16u);
    sample->mag_y = (int16_t)read_u16_le(buffer, 18u);
    sample->mag_z = (int16_t)read_u16_le(buffer, 20u);
    sample->fsr_adc = read_u16_le(buffer, 22u);
    sample->validity_flags = read_u16_le(buffer, 24u);
    return true;
}

size_t glove_protocol_capture_blob_size(uint16_t sample_count)
{
    return GLOVE_CAPTURE_BLOB_HEADER_SIZE + ((size_t)sample_count * GLOVE_CAPTURE_RECORD_SIZE);
}

bool glove_protocol_time_reached_u32(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}
