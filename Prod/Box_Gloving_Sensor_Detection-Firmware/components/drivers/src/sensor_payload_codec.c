#include "sensor_payload_codec.h"

#include <string.h>


static int32_t apply_deadband_i32(int32_t value, int16_t deadband)
{
    if (deadband <= 0) {
        return value;
    }

    if (value >= -deadband && value <= deadband) {
        return 0;
    }

    return value;
}

static void write_i16_le(uint8_t *buf, int *idx, int16_t value)
{
    buf[(*idx)++] = (uint8_t)(value & 0xFF);
    buf[(*idx)++] = (uint8_t)((value >> 8) & 0xFF);
}

static void bit_writer_init(bit_writer_t *bw, uint8_t *buffer, int max_bytes)
{
    bw->buffer = buffer;
    bw->max_bytes = max_bytes;
    bw->bit_pos = 0;
    memset(buffer, 0, max_bytes);
}

static bool bit_writer_write_bits(bit_writer_t *bw, uint32_t value, int num_bits)
{
    for (int i = 0; i < num_bits; i++) {
        int byte_index = bw->bit_pos / 8;
        int bit_index = bw->bit_pos % 8;

        if (byte_index >= bw->max_bytes) {
            return false;
        }

        if ((value >> i) & 0x01) {
            bw->buffer[byte_index] |= (uint8_t)(1u << bit_index);
        }

        bw->bit_pos++;
    }

    return true;
}

static int bit_writer_bytes_used(const bit_writer_t *bw)
{
    return (bw->bit_pos + 7) / 8;
}

static bool fits_signed_bits(int32_t value, int bits)
{
    int32_t min = -(1 << (bits - 1));
    int32_t max =  (1 << (bits - 1)) - 1;

    return value >= min && value <= max;
}

static bool write_signed_n_bits(bit_writer_t *bw, int32_t value, int bits)
{
    if (!fits_signed_bits(value, bits)) {
        return false;
    }

    /*
        Two's complement packing.

        Example, bits = 6:
            -1 becomes 0b111111
            +5 becomes 0b000101
    */
    uint32_t mask = (1u << bits) - 1u;
    uint32_t encoded = ((uint32_t)value) & mask;

    return bit_writer_write_bits(bw, encoded, bits);
}

static uint8_t mode_from_bits(int bits)
{
    switch (bits) {
        case 6:
            return SENSOR_PAYLOAD_MODE_DIFF6;
        case 8:
            return SENSOR_PAYLOAD_MODE_DIFF8;
        case 10:
            return SENSOR_PAYLOAD_MODE_DIFF10;
        default:
            return SENSOR_PAYLOAD_MODE_RAW16;
    }
}

/*
    Generic differential bit chooser.

    samples layout:
       
        for 3-axis:
            [x0, y0, z0, x1, y1, z1, ...]

    count = number of samples
    axis_count = 1 or 3
*/
static int choose_diff_bits_multi_axis(
    const int16_t *samples,
    int count,
    int axis_count,
    int16_t deadband
)
{
    if (samples == NULL || count <= 0 || axis_count <= 0) {
        return 16;
    }

    if (axis_count > AXIS_COUNT_3D) {
        return 16;
    }

    int max_abs_delta = 0;

    int16_t previous_reconstructed[AXIS_COUNT_3D] = {0};

    for (int axis = 0; axis < axis_count; axis++) {
        previous_reconstructed[axis] = samples[axis];
    }

    for (int sample = 1; sample < count; sample++) {
        int base = sample * axis_count;

        for (int axis = 0; axis < axis_count; axis++) {
            int16_t current = samples[base + axis];
            int32_t delta = (int32_t)current - previous_reconstructed[axis];

            delta = apply_deadband_i32(delta, deadband);

            int32_t abs_delta = delta < 0 ? -delta : delta;

            if (abs_delta > max_abs_delta) {
                max_abs_delta = (int)abs_delta;
            }

            previous_reconstructed[axis] = (int16_t)(previous_reconstructed[axis] + delta);
        }
    }

    if (max_abs_delta <= 31) {
        return 6;
    }

    if (max_abs_delta <= 127) {
        return 8;
    }

    if (max_abs_delta <= 511) {
        return 10;
    }

    return 16;
}

static int raw16_size_multi_axis(int count, int axis_count)
{
    return 1 + count * axis_count * (int)sizeof(int16_t);
}

static int diff_size_multi_axis(int count, int axis_count, int bits)
{
    int header_bytes = 1;
    int first_sample_bytes = axis_count * (int)sizeof(int16_t);
    int delta_sample_count = count - 1;
    int delta_value_count = delta_sample_count * axis_count;
    int packed_delta_bytes = (delta_value_count * bits + 7) / 8;

    return header_bytes + first_sample_bytes + packed_delta_bytes;
}

static int encode_i16_multi_axis_raw16(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count,
    int axis_count
)
{
    if (payload == 0 || samples == 0 || count < 1 || count > SENSOR_PAYLOAD_MAX_COUNT) {
        return 0;
    }

    if (axis_count != AXIS_COUNT_1D && axis_count != AXIS_COUNT_3D) {
        return 0;
    }

    int needed = raw16_size_multi_axis(count, axis_count);

    if (needed > payload_max) {
        return 0;
    }

    int idx = 0;

    payload[idx++] = sensor_payload_make_header(SENSOR_PAYLOAD_MODE_RAW16, (uint8_t)count);

    int total_values = count * axis_count;

    for (int i = 0; i < total_values; i++) {
        write_i16_le(payload, &idx, samples[i]);
    }

    return idx;
}


static int encode_i16_multi_axis_diff_bits(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count,
    int axis_count,
    int bits,
    int16_t deadband
)
{
    if (payload == 0 || samples == 0 || count < 1 || count > SENSOR_PAYLOAD_MAX_COUNT) {
        return 0;
    }

    if (axis_count != AXIS_COUNT_1D && axis_count != AXIS_COUNT_3D) {
        return 0;
    }

    if (bits != 6 && bits != 8 && bits != 10) {
        return 0;
    }

    int needed = diff_size_multi_axis(count, axis_count, bits);

    if (needed > payload_max) {
        return 0;
    }

    int idx = 0;

    payload[idx++] = sensor_payload_make_header(mode_from_bits(bits), (uint8_t)count);

    int16_t previous_reconstructed[AXIS_COUNT_3D] = {0};

    /*
        First sample is sent as full int16_t values.

        1-axis:
            first Z

        3-axis:
            first X, first Y, first Z
    */
    for (int axis = 0; axis < axis_count; axis++) {
        previous_reconstructed[axis] = samples[axis];
        write_i16_le(payload, &idx, previous_reconstructed[axis]);
    }

    bit_writer_t bw;
    bit_writer_init(&bw, &payload[idx], payload_max - idx);

    for (int sample = 1; sample < count; sample++) {
        int base = sample * axis_count;

        for (int axis = 0; axis < axis_count; axis++) {
            int16_t current = samples[base + axis];
            int32_t delta = (int32_t)current - previous_reconstructed[axis];

            delta = apply_deadband_i32(delta, deadband);

            if (!write_signed_n_bits(&bw, delta, bits)) {
                return 0;
            }

            previous_reconstructed[axis] = (int16_t)(previous_reconstructed[axis] + delta);
        }
    }

    idx += bit_writer_bytes_used(&bw);
    return idx;
}

static int encode_i16_multi_axis_best(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count,
    int axis_count,
    int16_t deadband
)
{
    if (payload == 0 || samples == 0 || count < 1 || count > SENSOR_PAYLOAD_MAX_COUNT) {
        return 0;
    }

    if (axis_count != AXIS_COUNT_1D && axis_count != AXIS_COUNT_3D) {
        return 0;
    }

    if (count == 1) {
        return encode_i16_multi_axis_raw16(payload, payload_max, samples, count, axis_count);
    }

    int raw_size = raw16_size_multi_axis(count, axis_count);
    int bits = choose_diff_bits_multi_axis(samples, count, axis_count, deadband);

    if (bits == 6 || bits == 8 || bits == 10) {
        int diff_size = diff_size_multi_axis(count, axis_count, bits);

        if (diff_size < raw_size) {
            return encode_i16_multi_axis_diff_bits(
                payload,
                payload_max,
                samples,
                count,
                axis_count,
                bits,
                deadband
            );
        }
    }
    
    return encode_i16_multi_axis_raw16(payload, payload_max, samples, count, axis_count);
}

uint8_t sensor_payload_make_header(uint8_t mode, uint8_t count)
{
    if (count < 1 || count > SENSOR_PAYLOAD_MAX_COUNT) {
        return 0;
    }

    uint8_t count_minus_1 = (uint8_t)(count - 1);

    return (uint8_t)(
        ((mode & SENSOR_HEADER_MODE_MASK) << SENSOR_HEADER_MODE_SHIFT) |
        (count_minus_1 & SENSOR_HEADER_COUNT_MASK)
    );
}

uint8_t sensor_payload_get_mode(uint8_t header)
{
    return (header >> SENSOR_HEADER_MODE_SHIFT) & SENSOR_HEADER_MODE_MASK;
}

uint8_t sensor_payload_get_count(uint8_t header)
{
    return (header & SENSOR_HEADER_COUNT_MASK) + 1;
}

/* ---------------- 1-axis wrappers ---------------- */

int sensor_payload_raw16_size_1axis(int count)
{
    return raw16_size_multi_axis(count, AXIS_COUNT_1D);
}

int sensor_payload_diff_size_1axis(int count, int bits)
{
    return diff_size_multi_axis(count, AXIS_COUNT_1D, bits);
}

int sensor_payload_encode_i16_1axis_raw16(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count
)
{
    return encode_i16_multi_axis_raw16(
        payload,
        payload_max,
        samples,
        count,
        AXIS_COUNT_1D
    );
}

int sensor_payload_encode_i16_1axis_diff_bits(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count,
    int bits,
    int16_t deadband
)
{
    return encode_i16_multi_axis_diff_bits(
        payload,
        payload_max,
        samples,
        count,
        AXIS_COUNT_1D,
        bits,
        deadband
    );
}

int sensor_payload_encode_i16_1axis_best(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples,
    int count,
    int16_t deadband
)
{
    return encode_i16_multi_axis_best(
        payload,
        payload_max,
        samples,
        count,
        AXIS_COUNT_1D,
        deadband
    );
}

/* ---------------- 3-axis XYZ wrappers ---------------- */

int sensor_payload_raw16_size_3axis(int count)
{
    return raw16_size_multi_axis(count, AXIS_COUNT_3D);
}

int sensor_payload_diff_size_3axis(int count, int bits)
{
    return diff_size_multi_axis(count, AXIS_COUNT_3D, bits);
}

int sensor_payload_encode_i16_3axis_raw16(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples_xyz,
    int count
)
{
    return encode_i16_multi_axis_raw16(
        payload,
        payload_max,
        samples_xyz,
        count,
        AXIS_COUNT_3D
    );
}

int sensor_payload_encode_i16_3axis_diff_bits(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples_xyz,
    int count,
    int bits,
    int16_t deadband
)
{
    return encode_i16_multi_axis_diff_bits(
        payload,
        payload_max,
        samples_xyz,
        count,
        AXIS_COUNT_3D,
        bits,
        deadband
    );
}

int sensor_payload_encode_i16_3axis_best(
    uint8_t *payload,
    int payload_max,
    const int16_t *samples_xyz,
    int count,
    int16_t deadband
)
{
    return encode_i16_multi_axis_best(
        payload,
        payload_max,
        samples_xyz,
        count,
        AXIS_COUNT_3D,
        deadband
    );
}
