#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "glove_protocol.h"
#include "punch_detector.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false; \
        } \
    } while (0)

static bool equal_metadata(const glove_capture_metadata_t *left,
                           const glove_capture_metadata_t *right)
{
    return left->capture_id == right->capture_id &&
           left->trigger_time_us == right->trigger_time_us &&
           left->first_sample_time_us == right->first_sample_time_us &&
           left->nominal_sample_period_us == right->nominal_sample_period_us &&
           left->sample_count == right->sample_count &&
           left->trigger_index == right->trigger_index &&
           left->pretrigger_samples == right->pretrigger_samples &&
           left->posttrigger_samples == right->posttrigger_samples &&
           left->capture_flags == right->capture_flags &&
           memcmp(left->accel_offset, right->accel_offset, sizeof(left->accel_offset)) == 0 &&
           memcmp(left->gyro_offset, right->gyro_offset, sizeof(left->gyro_offset)) == 0 &&
           memcmp(left->mag_reference, right->mag_reference, sizeof(left->mag_reference)) == 0;
}

static bool equal_sample(const glove_raw_sample_t *left, const glove_raw_sample_t *right)
{
    return left->delta_us == right->delta_us &&
           left->accel_x == right->accel_x && left->accel_y == right->accel_y &&
           left->accel_z == right->accel_z && left->gyro_x == right->gyro_x &&
           left->gyro_y == right->gyro_y && left->gyro_z == right->gyro_z &&
           left->mag_x == right->mag_x && left->mag_y == right->mag_y &&
           left->mag_z == right->mag_z && left->fsr_adc == right->fsr_adc &&
           left->validity_flags == right->validity_flags;
}

static bool test_frame_and_hello(void)
{
    uint8_t buffer[GLOVE_PROTOCOL_FRAME_HEADER_SIZE] = {0};
    glove_protocol_frame_header_t decoded = {0};
    CHECK(glove_protocol_encode_frame_header(buffer, sizeof(buffer), GLOVE_PROTOCOL_MSG_EVENT,
                                             0x1234u, 0xFEDCu) == sizeof(buffer));
    CHECK(glove_protocol_decode_frame_header(buffer, sizeof(buffer), &decoded));
    CHECK(decoded.type == GLOVE_PROTOCOL_MSG_EVENT);
    CHECK(decoded.session_id == 0x1234u);
    CHECK(decoded.sequence == 0xFEDCu);
    buffer[0] = 0u;
    CHECK(!glove_protocol_decode_frame_header(buffer, sizeof(buffer), &decoded));

    const uint8_t hello[] = {'G', 'B', GLOVE_PROTOCOL_VERSION,
                             GLOVE_PROTOCOL_CAP_EVENTS | GLOVE_PROTOCOL_CAP_STATS};
    uint8_t capabilities = 0u;
    CHECK(glove_protocol_parse_hello(hello, sizeof(hello), &capabilities));
    CHECK(capabilities == (GLOVE_PROTOCOL_CAP_EVENTS | GLOVE_PROTOCOL_CAP_STATS));
    const uint8_t zero_caps[] = {'G', 'B', GLOVE_PROTOCOL_VERSION, 0u};
    const uint8_t unknown_caps[] = {'G', 'B', GLOVE_PROTOCOL_VERSION, 0x80u};
    CHECK(!glove_protocol_parse_hello(zero_caps, sizeof(zero_caps), &capabilities));
    CHECK(!glove_protocol_parse_hello(unknown_caps, sizeof(unknown_caps), &capabilities));
    return true;
}

static bool test_capture_codec(void)
{
    const glove_capture_metadata_t input = {
        .capture_id = 73u,
        .trigger_time_us = 0xFFFFFF20u,
        .first_sample_time_us = 0xFFFFF000u,
        .nominal_sample_period_us = 1250u,
        .sample_count = 601u,
        .trigger_index = 400u,
        .pretrigger_samples = 400u,
        .posttrigger_samples = 200u,
        .capture_flags = GLOVE_CAPTURE_FLAG_CALIBRATED,
        .accel_offset = {-11, 22, INT16_MIN},
        .gyro_offset = {INT16_MAX, -33, 44},
        .mag_reference = {-55, 66, -77},
    };
    uint8_t header[GLOVE_CAPTURE_BLOB_HEADER_SIZE] = {0};
    glove_capture_metadata_t output = {0};
    CHECK(glove_protocol_encode_capture_header(header, sizeof(header), &input) == sizeof(header));
    CHECK(glove_protocol_decode_capture_header(header, sizeof(header), &output));
    CHECK(equal_metadata(&input, &output));
    CHECK(glove_protocol_capture_blob_size(input.sample_count) == 15674u);

    header[20] = 0u;
    header[21] = 0u;
    CHECK(!glove_protocol_decode_capture_header(header, sizeof(header), &output));

    const glove_raw_sample_t sample = {
        .delta_us = UINT32_MAX,
        .accel_x = INT16_MIN,
        .accel_y = -1,
        .accel_z = INT16_MAX,
        .gyro_x = -123,
        .gyro_y = 456,
        .gyro_z = -789,
        .mag_x = 321,
        .mag_y = -654,
        .mag_z = 987,
        .fsr_adc = 4095u,
        .validity_flags = GLOVE_SAMPLE_FLAG_ACCEL_VALID | GLOVE_SAMPLE_FLAG_MAG_OVERFLOW,
    };
    uint8_t record[GLOVE_CAPTURE_RECORD_SIZE] = {0};
    glove_raw_sample_t decoded = {0};
    CHECK(glove_protocol_encode_capture_record(record, sizeof(record), &sample) == sizeof(record));
    CHECK(glove_protocol_decode_capture_record(record, sizeof(record), &decoded));
    CHECK(equal_sample(&sample, &decoded));
    return true;
}

static int hex_nibble(int character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool read_hex_fixture(uint8_t *buffer, size_t buffer_size, size_t *written)
{
    FILE *file = fopen("tests/fixtures/v2_capture_vector.hex", "r");
    if (file == NULL || buffer == NULL || written == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }

    size_t count = 0u;
    int high_nibble = -1;
    int character;
    while ((character = fgetc(file)) != EOF) {
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            continue;
        }
        const int nibble = hex_nibble(character);
        if (nibble < 0) {
            fclose(file);
            return false;
        }
        if (high_nibble < 0) {
            high_nibble = nibble;
        } else {
            if (count >= buffer_size) {
                fclose(file);
                return false;
            }
            buffer[count++] = (uint8_t)((high_nibble << 4) | nibble);
            high_nibble = -1;
        }
    }
    fclose(file);
    if (high_nibble >= 0) {
        return false;
    }
    *written = count;
    return true;
}

static bool test_shared_capture_vector(void)
{
    uint8_t fixture[100] = {0};
    size_t fixture_size = 0u;
    CHECK(read_hex_fixture(fixture, sizeof(fixture), &fixture_size));
    CHECK(fixture_size == sizeof(fixture));

    glove_capture_metadata_t metadata = {0};
    CHECK(glove_protocol_decode_capture_header(fixture, fixture_size, &metadata));
    CHECK(metadata.capture_id == 0x1234u);
    CHECK(metadata.trigger_time_us == 0x01020304u);
    CHECK(metadata.first_sample_time_us == 0xF0E0D0C0u);
    CHECK(metadata.sample_count == 2u);
    CHECK(metadata.trigger_index == 1u);
    CHECK(metadata.pretrigger_samples == 1u);
    CHECK(metadata.posttrigger_samples == 0u);

    glove_raw_sample_t first = {0};
    glove_raw_sample_t second = {0};
    CHECK(glove_protocol_decode_capture_record(&fixture[GLOVE_CAPTURE_BLOB_HEADER_SIZE],
                                               GLOVE_CAPTURE_RECORD_SIZE, &first));
    CHECK(glove_protocol_decode_capture_record(
        &fixture[GLOVE_CAPTURE_BLOB_HEADER_SIZE + GLOVE_CAPTURE_RECORD_SIZE],
        GLOVE_CAPTURE_RECORD_SIZE, &second));
    CHECK(first.delta_us == 0u && first.accel_x == INT16_MIN && first.accel_z == INT16_MAX);
    CHECK(first.fsr_adc == 4095u && first.validity_flags == 0x020Fu);
    CHECK(second.delta_us == 1250u && second.accel_x == 1 && second.mag_z == 9);
    CHECK(second.fsr_adc == 2048u && second.validity_flags == 0x0C7Fu);

    uint8_t encoded[100] = {0};
    CHECK(glove_protocol_encode_capture_header(encoded, sizeof(encoded), &metadata) ==
          GLOVE_CAPTURE_BLOB_HEADER_SIZE);
    CHECK(glove_protocol_encode_capture_record(&encoded[GLOVE_CAPTURE_BLOB_HEADER_SIZE],
                                               GLOVE_CAPTURE_RECORD_SIZE, &first) ==
          GLOVE_CAPTURE_RECORD_SIZE);
    CHECK(glove_protocol_encode_capture_record(
              &encoded[GLOVE_CAPTURE_BLOB_HEADER_SIZE + GLOVE_CAPTURE_RECORD_SIZE],
              GLOVE_CAPTURE_RECORD_SIZE, &second) == GLOVE_CAPTURE_RECORD_SIZE);
    CHECK(memcmp(encoded, fixture, sizeof(fixture)) == 0);
    return true;
}

static bool test_deadline_wrap(void)
{
    CHECK(!glove_protocol_time_reached_u32(100u, 101u));
    CHECK(glove_protocol_time_reached_u32(101u, 101u));
    CHECK(glove_protocol_time_reached_u32(102u, 101u));
    CHECK(!glove_protocol_time_reached_u32(0xFFFFFFF0u, 0x00000010u));
    CHECK(glove_protocol_time_reached_u32(0x00000010u, 0x00000010u));
    CHECK(glove_protocol_time_reached_u32(0x00000020u, 0x00000010u));
    return true;
}

static bool test_punch_detector(void)
{
    const punch_detector_config_t config = {
        .trigger_force_centi_kg = 100u,
        .release_force_centi_kg = 25u,
        .release_debounce_us = 5000u,
        .minimum_retrigger_us = 20000u,
    };
    punch_detector_t detector;
    punch_detector_init(&detector, &config);
    CHECK(punch_detector_update(&detector, 1000u, 100u, true).triggered);
    CHECK(!punch_detector_update(&detector, 2000u, 120u, true).triggered);
    CHECK(!punch_detector_update(&detector, 3000u, 20u, true).released);
    CHECK(!punch_detector_update(&detector, 6000u, 20u, true).released);
    CHECK(!punch_detector_update(&detector, 7000u, 0u, false).released);
    CHECK(!punch_detector_update(&detector, 8000u, 20u, true).released);
    CHECK(punch_detector_update(&detector, 13000u, 20u, true).released);
    CHECK(!punch_detector_update(&detector, 20000u, 100u, true).triggered);
    CHECK(punch_detector_update(&detector, 21000u, 100u, true).triggered);

    punch_detector_init(&detector, &config);
    CHECK(punch_detector_update(&detector, 0xFFFFFFF0u, 100u, true).triggered);
    CHECK(!punch_detector_update(&detector, 0xFFFFFFF1u, 20u, true).released);
    CHECK(punch_detector_update(&detector, 0x00001380u, 20u, true).released);
    CHECK(!punch_detector_update(&detector, 0x00004000u, 100u, true).triggered);
    CHECK(punch_detector_update(&detector, 0x00004E10u, 100u, true).triggered);
    return true;
}

int main(void)
{
    const bool passed = test_frame_and_hello() && test_capture_codec() && test_shared_capture_vector() &&
                        test_deadline_wrap() && test_punch_detector();
    if (!passed) {
        return 1;
    }
    puts("protocol and detector tests passed");
    return 0;
}
