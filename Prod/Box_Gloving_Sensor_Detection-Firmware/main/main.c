/**
 * @file main.c
 * @brief Low-latency acquisition, punch detection, and v2 BLE delivery.
 *
 * The ADXL345 DATA_READY edge clocks the normal acquisition path.  The
 * ITG-3200 and QMC5883L have independent clocks and no wired interrupt on this
 * board, so their records report validity/freshness instead of claiming exact
 * temporal alignment with the accelerometer.  A force trigger immediately
 * enters a small event queue; a separate bounded snapshot path collects raw
 * context and never delays event detection.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "accelerometer_driver.h"
#include "bluetooth_driver.h"
#include "fsr_driver.h"
#include "giroscopio_driver.h"
#include "glove_protocol.h"
#include "i2c_driver_init.h"
#include "led_driver.h"
#include "magnometer_driver.h"
#include "nvs_driver.h"
#include "punch_detector.h"
#include "transmit_driver.h"

#define CALIBRATION_WAIT_MS 3000u
#define CALIBRATION_VALID_SAMPLES 400u
#define CALIBRATION_TIMEOUT_MS 2500u
#define SENSOR_WAIT_TIMEOUT_MS 5u

#define PRE_TRIGGER_MS 500u
#define POST_TRIGGER_MS 250u
#define PRE_TRIGGER_SAMPLES ((PRE_TRIGGER_MS * ACCEL_SAMPLE_RATE_HZ) / 1000u)
#define POST_TRIGGER_SAMPLES ((POST_TRIGGER_MS * ACCEL_SAMPLE_RATE_HZ) / 1000u)
#define SAMPLE_RING_CAPACITY (PRE_TRIGGER_SAMPLES + 1u)
#define CAPTURE_MAX_SAMPLES (PRE_TRIGGER_SAMPLES + 1u + POST_TRIGGER_SAMPLES)
#define CAPTURE_SLOT_COUNT 2u
#define MAG_POLL_DIVIDER (ACCEL_SAMPLE_RATE_HZ / QMC5883L_SAMPLE_RATE_HZ)
#define NOMINAL_SAMPLE_PERIOD_US (1000000u / ACCEL_SAMPLE_RATE_HZ)
#define ACQUISITION_GAP_THRESHOLD_US ((NOMINAL_SAMPLE_PERIOD_US * 3u) / 2u)

#define PUNCH_TRIGGER_CENTI_KG 100u
#define PUNCH_RELEASE_CENTI_KG 25u
#define PUNCH_RELEASE_DEBOUNCE_US 5000u
#define PUNCH_MINIMUM_RETRIGGER_US 20000u

#define EVENT_QUEUE_LENGTH 8u
#define CAPTURE_QUEUE_LENGTH CAPTURE_SLOT_COUNT
#define SENSOR_TASK_STACK_SIZE 6144u
#define TRANSPORT_TASK_STACK_SIZE 4096u
#define SENSOR_TASK_PRIORITY 6u
#define TRANSPORT_TASK_PRIORITY 5u
#define TRANSPORT_BACKOFF_MS 2u
#define TRANSPORT_IDLE_WAIT_MS 2u
#define EVENT_TX_MAX_RETRIES 5u
#define EVENT_TX_DEADLINE_US 50000u
#define CAPTURE_TX_MAX_RETRIES 20u
#define CAPTURE_TX_DEADLINE_US 3000000u
#define STATS_INTERVAL_US 1000000u
#define RUNTIME_STATS_COUNT 23u
#define STATS_VALUES_PER_FRAME 3u
#define STATS_CHUNK_COUNT ((RUNTIME_STATS_COUNT + STATS_VALUES_PER_FRAME - 1u) / STATS_VALUES_PER_FRAME)

_Static_assert(ACCEL_SAMPLE_RATE_HZ % QMC5883L_SAMPLE_RATE_HZ == 0u,
               "Magnetometer polling cadence must be integral");
_Static_assert(CAPTURE_MAX_SAMPLES <= UINT16_MAX,
               "Capture sample count must fit the protocol");

typedef struct {
    glove_raw_sample_t raw;
    uint32_t timestamp_us;
} timed_sample_t;

typedef enum {
    CAPTURE_SLOT_FREE = 0,
    /* Claimed by the acquisition task before it copies the trigger context. */
    CAPTURE_SLOT_RESERVED,
    CAPTURE_SLOT_CAPTURING,
    CAPTURE_SLOT_QUEUED,
    CAPTURE_SLOT_TRANSMITTING,
} capture_slot_state_t;

typedef struct {
    volatile capture_slot_state_t state;
    uint16_t session_id;
    glove_capture_metadata_t metadata;
    glove_raw_sample_t samples[CAPTURE_MAX_SAMPLES];
    uint16_t collected_samples;
    uint16_t target_samples;
    uint16_t tx_offset;
    bool info_sent;
} capture_slot_t;

typedef struct {
    uint16_t session_id;
    uint16_t punch_id;
    uint32_t trigger_time_us;
    uint16_t fsr_adc;
    uint16_t force_centi_kg;
    uint16_t flags;
} punch_event_t;

typedef struct {
    volatile uint32_t sample_cycles;
    volatile uint32_t drdy_timeouts;
    volatile uint32_t drdy_coalesced;
    volatile uint32_t accel_errors;
    volatile uint32_t gyro_errors;
    volatile uint32_t mag_errors;
    volatile uint32_t fsr_errors;
    volatile uint32_t capture_drops;
    volatile uint32_t event_queue_drops;
    volatile uint32_t capture_queue_drops;
    volatile uint32_t tx_failures;
    volatile uint32_t tx_session_drops;
    volatile uint32_t event_tx_drops;
    volatile uint32_t capture_tx_drops;
    volatile uint32_t punch_events;
    volatile uint32_t sample_gap_events;
    volatile uint32_t max_sample_gap_us;
    volatile uint32_t acquisition_deadline_misses;
    volatile uint32_t max_acquisition_duration_us;
    volatile uint32_t accel_fresh_samples;
    volatile uint32_t gyro_fresh_samples;
    volatile uint32_t mag_fresh_samples;
    volatile uint32_t fsr_valid_samples;
} runtime_stats_t;

static const char *TAG = "GLOVE";
static TaskHandle_t sensor_task_handle;
static QueueHandle_t event_queue;
static QueueHandle_t capture_queue;

static timed_sample_t sample_ring[SAMPLE_RING_CAPACITY];
static uint16_t sample_ring_write_index;
static uint16_t sample_ring_count;
static capture_slot_t capture_slots[CAPTURE_SLOT_COUNT];
static portMUX_TYPE capture_state_lock = portMUX_INITIALIZER_UNLOCKED;
static runtime_stats_t runtime_stats;

static glove_capture_metadata_t calibration_metadata;
static bool motion_calibrated;
static uint16_t next_punch_id;

static accel_raw_data_t last_accel;
static giro_raw_data_t last_gyro;
static mag_raw_data_t last_mag;
static bool have_last_accel;
static bool have_last_gyro;
static bool have_last_mag;
static uint16_t last_accel_quality_flags;
static uint16_t last_gyro_quality_flags;
static uint16_t last_mag_quality_flags;

static capture_slot_state_t capture_slot_get_state(const capture_slot_t *slot)
{
    capture_slot_state_t state;
    portENTER_CRITICAL(&capture_state_lock);
    state = slot->state;
    portEXIT_CRITICAL(&capture_state_lock);
    return state;
}

static bool capture_slot_claim(capture_slot_t *slot)
{
    bool claimed = false;
    portENTER_CRITICAL(&capture_state_lock);
    if (slot->state == CAPTURE_SLOT_FREE) {
        slot->state = CAPTURE_SLOT_RESERVED;
        claimed = true;
    }
    portEXIT_CRITICAL(&capture_state_lock);
    return claimed;
}

static void capture_slot_set_state(capture_slot_t *slot, capture_slot_state_t state)
{
    portENTER_CRITICAL(&capture_state_lock);
    slot->state = state;
    portEXIT_CRITICAL(&capture_state_lock);
}
static uint32_t sample_number;
static uint32_t previous_sample_time_us;
static bool have_previous_sample_time;

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

static bool raw_near_limit(int16_t value, int16_t limit)
{
    return value >= limit || value <= (int16_t)-limit;
}

static int16_t clamp_i64_to_i16(int64_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void store_sample(const timed_sample_t *sample)
{
    sample_ring[sample_ring_write_index] = *sample;
    sample_ring_write_index = (uint16_t)((sample_ring_write_index + 1u) % SAMPLE_RING_CAPACITY);
    if (sample_ring_count < SAMPLE_RING_CAPACITY) {
        sample_ring_count++;
    }
}

static bool wait_for_data_ready(void)
{
    const uint32_t notifications = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SENSOR_WAIT_TIMEOUT_MS));
    if (notifications == 0u) {
        runtime_stats.drdy_timeouts++;
        return false;
    }
    if (notifications > 1u) {
        runtime_stats.drdy_coalesced += notifications - 1u;
    }
    return true;
}

static bool calibrate_motion_sensors(void)
{
    int64_t accel_sum[3] = {0};
    int64_t gyro_sum[3] = {0};
    int64_t mag_sum[3] = {0};
    uint32_t valid_samples = 0u;
    uint32_t valid_mag_samples = 0u;
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)CALIBRATION_TIMEOUT_MS * 1000LL);

    memset(&calibration_metadata, 0, sizeof(calibration_metadata));
    ESP_LOGI(TAG, "Calibration: keep glove still for about %u ms", CALIBRATION_TIMEOUT_MS);

    while (valid_samples < CALIBRATION_VALID_SAMPLES && esp_timer_get_time() < deadline_us) {
        if (!wait_for_data_ready()) {
            continue;
        }

        accel_raw_data_t accel = {0};
        giro_raw_data_t gyro = {0};
        uint8_t gyro_status = 0u;
        const esp_err_t accel_err = accel_get_raw_data(&accel);
        const esp_err_t gyro_err = giro_get_raw_data(&gyro, &gyro_status);
        if (accel_err != ESP_OK || gyro_err != ESP_OK ||
            (gyro_status & ITG3200_INT_STATUS_RAW_RDY) == 0u) {
            continue;
        }

        accel_sum[0] += accel.x;
        accel_sum[1] += accel.y;
        accel_sum[2] += accel.z;
        gyro_sum[0] += gyro.x;
        gyro_sum[1] += gyro.y;
        gyro_sum[2] += gyro.z;
        valid_samples++;

        if ((valid_samples % MAG_POLL_DIVIDER) == 0u) {
            uint8_t mag_status = 0u;
            mag_raw_data_t mag = {0};
            if (mag_get_status(&mag_status) == ESP_OK &&
                (mag_status & (QMC5883L_STATUS_DRDY | QMC5883L_STATUS_OVL | QMC5883L_STATUS_DOR)) ==
                    QMC5883L_STATUS_DRDY &&
                mag_get_raw_data(&mag) == ESP_OK) {
                mag_sum[0] += mag.x;
                mag_sum[1] += mag.y;
                mag_sum[2] += mag.z;
                valid_mag_samples++;
            }
        }
    }

    if (valid_samples < CALIBRATION_VALID_SAMPLES) {
        ESP_LOGW(TAG, "Calibration incomplete: %u/%u valid IMU samples; raw capture continues",
                 valid_samples, CALIBRATION_VALID_SAMPLES);
        return false;
    }

    calibration_metadata.accel_offset[0] = clamp_i64_to_i16(accel_sum[0] / (int64_t)valid_samples);
    calibration_metadata.accel_offset[1] = clamp_i64_to_i16(accel_sum[1] / (int64_t)valid_samples);
    calibration_metadata.accel_offset[2] = clamp_i64_to_i16(
        (accel_sum[2] / (int64_t)valid_samples) - ADXL345_REST_Z_COUNTS);
    calibration_metadata.gyro_offset[0] = clamp_i64_to_i16(gyro_sum[0] / (int64_t)valid_samples);
    calibration_metadata.gyro_offset[1] = clamp_i64_to_i16(gyro_sum[1] / (int64_t)valid_samples);
    calibration_metadata.gyro_offset[2] = clamp_i64_to_i16(gyro_sum[2] / (int64_t)valid_samples);
    if (valid_mag_samples > 0u) {
        calibration_metadata.mag_reference[0] = clamp_i64_to_i16(mag_sum[0] / (int64_t)valid_mag_samples);
        calibration_metadata.mag_reference[1] = clamp_i64_to_i16(mag_sum[1] / (int64_t)valid_mag_samples);
        calibration_metadata.mag_reference[2] = clamp_i64_to_i16(mag_sum[2] / (int64_t)valid_mag_samples);
    }

    motion_calibrated = true;
    ESP_LOGI(TAG, "Calibration complete (%u IMU, %u MAG samples)",
             valid_samples, valid_mag_samples);
    return true;
}

static void update_max_stat(volatile uint32_t *maximum, uint32_t value)
{
    /* The acquisition task is the sole writer; readers only need a recent snapshot. */
    if (value > *maximum) {
        *maximum = value;
    }
}

static timed_sample_t begin_sample(uint32_t timestamp_us, bool drdy_timeout)
{
    timed_sample_t sample = {0};
    sample.timestamp_us = timestamp_us;
    if (have_previous_sample_time) {
        sample.raw.delta_us = (uint32_t)(timestamp_us - previous_sample_time_us);
        if (sample.raw.delta_us > ACQUISITION_GAP_THRESHOLD_US) {
            runtime_stats.sample_gap_events++;
            update_max_stat(&runtime_stats.max_sample_gap_us, sample.raw.delta_us);
        }
    }
    previous_sample_time_us = timestamp_us;
    have_previous_sample_time = true;
    if (drdy_timeout) {
        sample.raw.validity_flags |= GLOVE_SAMPLE_FLAG_DRDY_TIMEOUT;
    }

    /* This read and the detector run before any potentially slow I2C transaction. */
    uint16_t fsr_adc = 0u;
    if (fsr_read_raw(FSR_PIN0, &fsr_adc) == ESP_OK) {
        sample.raw.fsr_adc = fsr_adc;
        sample.raw.validity_flags |= GLOVE_SAMPLE_FLAG_FSR_VALID;
        runtime_stats.fsr_valid_samples++;
    } else {
        runtime_stats.fsr_errors++;
    }
    return sample;
}

static void finish_sample(timed_sample_t *sample, bool drdy_timeout)
{
    accel_raw_data_t accel = {0};
    const esp_err_t accel_err = accel_get_raw_data(&accel);
    if (accel_err == ESP_OK) {
        sample->raw.accel_x = accel.x;
        sample->raw.accel_y = accel.y;
        sample->raw.accel_z = accel.z;
        sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_ACCEL_VALID;
        last_accel = accel;
        have_last_accel = true;
        last_accel_quality_flags = 0u;
        if (raw_near_limit(accel.x, ADXL345_SATURATION_COUNTS) ||
            raw_near_limit(accel.y, ADXL345_SATURATION_COUNTS) ||
            raw_near_limit(accel.z, ADXL345_SATURATION_COUNTS)) {
            sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_ACCEL_SATURATED;
            last_accel_quality_flags = GLOVE_SAMPLE_FLAG_ACCEL_SATURATED;
        }
        if (drdy_timeout) {
            sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_ACCEL_STALE;
        } else {
            runtime_stats.accel_fresh_samples++;
        }
    } else {
        runtime_stats.accel_errors++;
        if (have_last_accel) {
            sample->raw.accel_x = last_accel.x;
            sample->raw.accel_y = last_accel.y;
            sample->raw.accel_z = last_accel.z;
            sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_ACCEL_VALID |
                                         GLOVE_SAMPLE_FLAG_ACCEL_STALE |
                                         last_accel_quality_flags;
        }
    }

    giro_raw_data_t gyro = {0};
    uint8_t gyro_status = 0u;
    const esp_err_t gyro_err = giro_get_raw_data(&gyro, &gyro_status);
    if (gyro_err == ESP_OK) {
        sample->raw.gyro_x = gyro.x;
        sample->raw.gyro_y = gyro.y;
        sample->raw.gyro_z = gyro.z;
        sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_GYRO_VALID;
        last_gyro = gyro;
        have_last_gyro = true;
        last_gyro_quality_flags = 0u;
        if ((gyro_status & ITG3200_INT_STATUS_RAW_RDY) == 0u) {
            sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_GYRO_STALE;
        } else {
            runtime_stats.gyro_fresh_samples++;
        }
        if (raw_near_limit(gyro.x, ITG3200_SATURATION_COUNTS) ||
            raw_near_limit(gyro.y, ITG3200_SATURATION_COUNTS) ||
            raw_near_limit(gyro.z, ITG3200_SATURATION_COUNTS)) {
            sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_GYRO_SATURATED;
            last_gyro_quality_flags = GLOVE_SAMPLE_FLAG_GYRO_SATURATED;
        }
    } else {
        runtime_stats.gyro_errors++;
        if (have_last_gyro) {
            sample->raw.gyro_x = last_gyro.x;
            sample->raw.gyro_y = last_gyro.y;
            sample->raw.gyro_z = last_gyro.z;
            sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_GYRO_VALID |
                                         GLOVE_SAMPLE_FLAG_GYRO_STALE |
                                         last_gyro_quality_flags;
        }
    }

    const bool poll_mag = (sample_number % MAG_POLL_DIVIDER) == 0u;
    if (poll_mag) {
        uint8_t mag_status = 0u;
        mag_raw_data_t mag = {0};
        const esp_err_t status_err = mag_get_status(&mag_status);
        const esp_err_t mag_err = (status_err == ESP_OK && (mag_status & QMC5883L_STATUS_DRDY) != 0u)
                                      ? mag_get_raw_data(&mag)
                                      : status_err;
        if (mag_err == ESP_OK && (mag_status & QMC5883L_STATUS_DRDY) != 0u) {
            sample->raw.mag_x = mag.x;
            sample->raw.mag_y = mag.y;
            sample->raw.mag_z = mag.z;
            sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_MAG_VALID;
            last_mag = mag;
            have_last_mag = true;
            last_mag_quality_flags = 0u;
            if ((mag_status & (QMC5883L_STATUS_OVL | QMC5883L_STATUS_DOR)) != 0u) {
                sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_MAG_OVERFLOW;
                last_mag_quality_flags = GLOVE_SAMPLE_FLAG_MAG_OVERFLOW;
            } else {
                runtime_stats.mag_fresh_samples++;
            }
        } else {
            if (status_err != ESP_OK || mag_err != ESP_OK) {
                runtime_stats.mag_errors++;
            }
            if (have_last_mag) {
                sample->raw.mag_x = last_mag.x;
                sample->raw.mag_y = last_mag.y;
                sample->raw.mag_z = last_mag.z;
                sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_MAG_VALID |
                                             GLOVE_SAMPLE_FLAG_MAG_STALE |
                                             last_mag_quality_flags;
            }
        }
    } else if (have_last_mag) {
        sample->raw.mag_x = last_mag.x;
        sample->raw.mag_y = last_mag.y;
        sample->raw.mag_z = last_mag.z;
        sample->raw.validity_flags |= GLOVE_SAMPLE_FLAG_MAG_VALID |
                                     GLOVE_SAMPLE_FLAG_MAG_STALE |
                                     last_mag_quality_flags;
    }

    sample_number++;
    runtime_stats.sample_cycles++;
    const uint32_t acquisition_duration_us = (uint32_t)esp_timer_get_time() - sample->timestamp_us;
    update_max_stat(&runtime_stats.max_acquisition_duration_us, acquisition_duration_us);
    if (acquisition_duration_us > NOMINAL_SAMPLE_PERIOD_US) {
        runtime_stats.acquisition_deadline_misses++;
    }
}

static void queue_completed_capture(uint8_t slot_index)
{
    capture_slot_t *slot = &capture_slots[slot_index];
    /* Publish all completed sample writes before the queue transfers ownership. */
    capture_slot_set_state(slot, CAPTURE_SLOT_QUEUED);
    if (xQueueSend(capture_queue, &slot_index, 0u) != pdPASS) {
        capture_slot_set_state(slot, CAPTURE_SLOT_FREE);
        runtime_stats.capture_queue_drops++;
    }
}

static void append_to_active_captures(const timed_sample_t *sample)
{
    for (uint8_t index = 0u; index < CAPTURE_SLOT_COUNT; index++) {
        capture_slot_t *slot = &capture_slots[index];
        if (capture_slot_get_state(slot) != CAPTURE_SLOT_CAPTURING) {
            continue;
        }
        if (slot->collected_samples < slot->target_samples) {
            slot->samples[slot->collected_samples++] = sample->raw;
        }
        if (slot->collected_samples >= slot->target_samples) {
            queue_completed_capture(index);
        }
    }
}

/*
 * Reserving a slot is intentionally constant-time.  It occurs before I2C
 * reads so the immediate event can report a truthful allocation result.  The
 * same acquisition task fills the immutable snapshot after it has read and
 * stored the trigger sample.
 */
static bool reserve_capture_slot(uint8_t *slot_index)
{
    if (slot_index == NULL) {
        return false;
    }
    for (uint8_t index = 0u; index < CAPTURE_SLOT_COUNT; index++) {
        if (capture_slot_claim(&capture_slots[index])) {
            *slot_index = index;
            return true;
        }
    }
    runtime_stats.capture_drops++;
    return false;
}

static bool initialize_reserved_capture(
    uint8_t slot_index,
    uint16_t session_id,
    uint16_t capture_id,
    uint32_t trigger_time_us
)
{
    if (slot_index >= CAPTURE_SLOT_COUNT || sample_ring_count == 0u) {
        if (slot_index < CAPTURE_SLOT_COUNT) {
            capture_slot_set_state(&capture_slots[slot_index], CAPTURE_SLOT_FREE);
        }
        runtime_stats.capture_drops++;
        return false;
    }

    capture_slot_t *slot = &capture_slots[slot_index];
    if (capture_slot_get_state(slot) != CAPTURE_SLOT_RESERVED) {
        return false;
    }

    slot->session_id = session_id;
    slot->metadata = calibration_metadata;
    slot->metadata.capture_id = capture_id;
    slot->metadata.trigger_time_us = trigger_time_us;
    slot->metadata.nominal_sample_period_us = NOMINAL_SAMPLE_PERIOD_US;
    slot->metadata.capture_flags = motion_calibrated ? GLOVE_CAPTURE_FLAG_CALIBRATED : 0u;
    slot->tx_offset = 0u;
    slot->info_sent = false;

    const uint16_t wanted = sample_ring_count > (PRE_TRIGGER_SAMPLES + 1u)
                                ? (PRE_TRIGGER_SAMPLES + 1u)
                                : sample_ring_count;
    const uint16_t oldest = (uint16_t)((sample_ring_write_index + SAMPLE_RING_CAPACITY - sample_ring_count) %
                                       SAMPLE_RING_CAPACITY);
    const uint16_t start = (uint16_t)((oldest + sample_ring_count - wanted) % SAMPLE_RING_CAPACITY);
    for (uint16_t i = 0u; i < wanted; i++) {
        const timed_sample_t *source = &sample_ring[(start + i) % SAMPLE_RING_CAPACITY];
        slot->samples[i] = source->raw;
        if (i == 0u) {
            slot->samples[i].delta_us = 0u;
            slot->metadata.first_sample_time_us = source->timestamp_us;
        }
    }

    slot->metadata.pretrigger_samples = (uint16_t)(wanted - 1u);
    slot->metadata.trigger_index = slot->metadata.pretrigger_samples;
    slot->metadata.posttrigger_samples = POST_TRIGGER_SAMPLES;
    slot->metadata.sample_count = (uint16_t)(wanted + POST_TRIGGER_SAMPLES);
    slot->target_samples = slot->metadata.sample_count;
    slot->collected_samples = wanted;
    if (wanted < PRE_TRIGGER_SAMPLES + 1u) {
        slot->metadata.capture_flags |= GLOVE_CAPTURE_FLAG_SHORT_PRETRIGGER;
    }
    /* Queue consumers cannot observe this slot until it becomes QUEUED. */
    capture_slot_set_state(slot, CAPTURE_SLOT_CAPTURING);
    return true;
}

static void queue_punch_event(const punch_event_t *event)
{
    if (xQueueSend(event_queue, event, 0u) != pdPASS) {
        runtime_stats.event_queue_drops++;
    }
}

static bool session_is_current(uint16_t expected_session_id)
{
    glove_ble_session_t session = {0};
    return glove_ble_get_session(&session) && session.active && session.session_id == expected_session_id;
}

static esp_err_t send_v2_frame(
    uint16_t expected_session_id,
    uint16_t *sequence,
    uint8_t type,
    const uint8_t *payload,
    uint16_t payload_size
)
{
    glove_ble_session_t session = {0};
    if (!glove_ble_get_session(&session) || !session.active ||
        session.session_id != expected_session_id || payload == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((uint32_t)payload_size + GLOVE_PROTOCOL_FRAME_HEADER_SIZE > session.notification_payload_max ||
        (uint32_t)payload_size + GLOVE_PROTOCOL_FRAME_HEADER_SIZE > GLOVE_BLE_MAX_NOTIFICATION_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[GLOVE_BLE_MAX_NOTIFICATION_PAYLOAD] = {0};
    const size_t header_size = glove_protocol_encode_frame_header(
        frame, sizeof(frame), type, expected_session_id, *sequence);
    if (header_size == 0u) {
        return ESP_FAIL;
    }
    memcpy(&frame[header_size], payload, payload_size);

    if (!session_is_current(expected_session_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = send_raw_notification_for_session(
        expected_session_id, frame, (uint16_t)(header_size + payload_size));
    if (err == ESP_OK) {
        (*sequence)++;
    }
    return err;
}

static esp_err_t send_hello_ack(uint16_t session_id, uint16_t *sequence)
{
    glove_ble_session_t session = {0};
    if (!glove_ble_get_session(&session)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[6] = {
        GLOVE_PROTOCOL_VERSION,
        session.capabilities,
        0u, 0u, 0u, 0u,
    };
    write_u16_le(payload, 2u, ACCEL_SAMPLE_RATE_HZ);
    write_u16_le(payload, 4u, QMC5883L_SAMPLE_RATE_HZ);
    return send_v2_frame(session_id, sequence, GLOVE_PROTOCOL_MSG_HELLO_ACK, payload, sizeof(payload));
}

static esp_err_t send_event(const punch_event_t *event, uint16_t *sequence)
{
    uint8_t payload[12] = {0};
    write_u16_le(payload, 0u, event->punch_id);
    write_u32_le(payload, 2u, event->trigger_time_us);
    write_u16_le(payload, 6u, event->fsr_adc);
    write_u16_le(payload, 8u, event->force_centi_kg);
    write_u16_le(payload, 10u, event->flags);
    return send_v2_frame(event->session_id, sequence, GLOVE_PROTOCOL_MSG_EVENT, payload, sizeof(payload));
}

static size_t capture_blob_size(const capture_slot_t *slot)
{
    return glove_protocol_capture_blob_size(slot->metadata.sample_count);
}

static size_t copy_capture_blob_range(
    const capture_slot_t *slot,
    uint16_t offset,
    uint8_t *destination,
    size_t destination_size
)
{
    const size_t total_size = capture_blob_size(slot);
    if (destination == NULL || offset >= total_size || destination_size == 0u) {
        return 0u;
    }
    size_t remaining = destination_size;
    if ((size_t)offset + remaining > total_size) {
        remaining = total_size - offset;
    }

    uint8_t header[GLOVE_CAPTURE_BLOB_HEADER_SIZE] = {0};
    if (glove_protocol_encode_capture_header(header, sizeof(header), &slot->metadata) == 0u) {
        return 0u;
    }

    size_t written = 0u;
    size_t position = offset;
    while (written < remaining) {
        if (position < GLOVE_CAPTURE_BLOB_HEADER_SIZE) {
            const size_t available = GLOVE_CAPTURE_BLOB_HEADER_SIZE - position;
            const size_t take = (remaining - written) < available ? (remaining - written) : available;
            memcpy(&destination[written], &header[position], take);
            written += take;
            position += take;
            continue;
        }

        const size_t record_offset = position - GLOVE_CAPTURE_BLOB_HEADER_SIZE;
        const uint16_t record_index = (uint16_t)(record_offset / GLOVE_CAPTURE_RECORD_SIZE);
        const size_t byte_in_record = record_offset % GLOVE_CAPTURE_RECORD_SIZE;
        uint8_t record[GLOVE_CAPTURE_RECORD_SIZE] = {0};
        if (record_index >= slot->metadata.sample_count ||
            glove_protocol_encode_capture_record(record, sizeof(record), &slot->samples[record_index]) == 0u) {
            return 0u;
        }
        const size_t available = GLOVE_CAPTURE_RECORD_SIZE - byte_in_record;
        const size_t take = (remaining - written) < available ? (remaining - written) : available;
        memcpy(&destination[written], &record[byte_in_record], take);
        written += take;
        position += take;
    }
    return written;
}

static esp_err_t send_capture_info(capture_slot_t *slot, uint16_t *sequence)
{
    const size_t total_size = capture_blob_size(slot);
    if (total_size == 0u || total_size > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t payload[10] = {0};
    write_u16_le(payload, 0u, slot->metadata.capture_id);
    write_u16_le(payload, 2u, (uint16_t)total_size);
    write_u16_le(payload, 4u, slot->metadata.sample_count);
    write_u16_le(payload, 6u, slot->metadata.capture_flags);
    write_u16_le(payload, 8u, slot->metadata.trigger_index);
    return send_v2_frame(slot->session_id, sequence, GLOVE_PROTOCOL_MSG_CAPTURE_INFO, payload, sizeof(payload));
}

static esp_err_t send_capture_fragment(capture_slot_t *slot, uint16_t *sequence)
{
    glove_ble_session_t session = {0};
    const size_t total_size = capture_blob_size(slot);
    if (!glove_ble_get_session(&session) || !session.active ||
        session.session_id != slot->session_id || total_size > UINT16_MAX ||
        slot->tx_offset >= total_size) {
        return ESP_ERR_INVALID_STATE;
    }
    if (session.notification_payload_max <= GLOVE_PROTOCOL_FRAME_HEADER_SIZE + 6u) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint16_t max_data = (uint16_t)(session.notification_payload_max -
                                         GLOVE_PROTOCOL_FRAME_HEADER_SIZE - 6u);
    const uint16_t remaining = (uint16_t)(total_size - slot->tx_offset);
    const uint16_t data_length = remaining < max_data ? remaining : max_data;
    uint8_t payload[GLOVE_BLE_MAX_NOTIFICATION_PAYLOAD] = {0};
    write_u16_le(payload, 0u, slot->metadata.capture_id);
    write_u16_le(payload, 2u, slot->tx_offset);
    write_u16_le(payload, 4u, (uint16_t)total_size);
    if (copy_capture_blob_range(slot, slot->tx_offset, &payload[6], data_length) != data_length) {
        return ESP_FAIL;
    }

    const esp_err_t err = send_v2_frame(slot->session_id, sequence,
                                         GLOVE_PROTOCOL_MSG_CAPTURE_DATA,
                                         payload, (uint16_t)(6u + data_length));
    if (err == ESP_OK) {
        slot->tx_offset = (uint16_t)(slot->tx_offset + data_length);
    }
    return err;
}

static uint32_t runtime_stat_value(uint8_t index)
{
    switch (index) {
    case 0: return runtime_stats.sample_cycles;
    case 1: return runtime_stats.drdy_timeouts;
    case 2: return runtime_stats.drdy_coalesced;
    case 3: return runtime_stats.accel_errors;
    case 4: return runtime_stats.gyro_errors;
    case 5: return runtime_stats.mag_errors;
    case 6: return runtime_stats.fsr_errors;
    case 7: return runtime_stats.capture_drops;
    case 8: return runtime_stats.event_queue_drops;
    case 9: return runtime_stats.capture_queue_drops;
    case 10: return runtime_stats.tx_failures;
    case 11: return runtime_stats.tx_session_drops;
    case 12: return runtime_stats.event_tx_drops;
    case 13: return runtime_stats.capture_tx_drops;
    case 14: return runtime_stats.punch_events;
    case 15: return runtime_stats.sample_gap_events;
    case 16: return runtime_stats.max_sample_gap_us;
    case 17: return runtime_stats.acquisition_deadline_misses;
    case 18: return runtime_stats.max_acquisition_duration_us;
    case 19: return runtime_stats.accel_fresh_samples;
    case 20: return runtime_stats.gyro_fresh_samples;
    case 21: return runtime_stats.mag_fresh_samples;
    case 22: return runtime_stats.fsr_valid_samples;
    default: return 0u;
    }
}

static esp_err_t send_stats_chunk(uint16_t session_id, uint16_t *sequence, uint8_t chunk_index)
{
    const uint8_t total_chunks = STATS_CHUNK_COUNT;
    if (chunk_index >= total_chunks) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t first = (uint8_t)(chunk_index * STATS_VALUES_PER_FRAME);
    const uint8_t values = (RUNTIME_STATS_COUNT - first) < STATS_VALUES_PER_FRAME
                               ? (RUNTIME_STATS_COUNT - first)
                               : STATS_VALUES_PER_FRAME;
    uint8_t payload[2u + (STATS_VALUES_PER_FRAME * 4u)] = {0};
    payload[0] = chunk_index;
    payload[1] = total_chunks;
    for (uint8_t i = 0u; i < values; i++) {
        write_u32_le(payload, 2u + ((size_t)i * 4u), runtime_stat_value((uint8_t)(first + i)));
    }
    return send_v2_frame(session_id, sequence, GLOVE_PROTOCOL_MSG_STATS,
                         payload, (uint16_t)(2u + ((uint16_t)values * 4u)));
}

static void handle_transport_error(esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        runtime_stats.tx_failures++;
    }
}

static void transport_task(void *argument)
{
    (void)argument;
    uint16_t known_session = 0u;
    uint16_t sequence = 0u;
    uint8_t next_stats_chunk = 0u;
    uint32_t next_stats_due_us = 0u;
    bool hello_pending = false;
    int current_slot = -1;
    uint8_t capture_retries = 0u;
    uint32_t capture_first_attempt_us = 0u;
    punch_event_t pending_event = {0};
    bool pending_event_valid = false;
    uint8_t event_retries = 0u;
    uint32_t event_first_attempt_us = 0u;

    while (true) {
        glove_ble_session_t session = {0};
        if (!glove_ble_get_session(&session) || !session.active) {
            if (current_slot >= 0) {
                capture_slot_t *abandoned_slot = &capture_slots[current_slot];
                current_slot = -1;
                capture_slot_set_state(abandoned_slot, CAPTURE_SLOT_FREE);
                runtime_stats.tx_session_drops++;
            }
            known_session = 0u;
            vTaskDelay(pdMS_TO_TICKS(TRANSPORT_IDLE_WAIT_MS));
            continue;
        }
        if (known_session != session.session_id) {
            if (current_slot >= 0) {
                capture_slot_t *abandoned_slot = &capture_slots[current_slot];
                current_slot = -1;
                capture_slot_set_state(abandoned_slot, CAPTURE_SLOT_FREE);
                runtime_stats.tx_session_drops++;
            }
            if (pending_event_valid && pending_event.session_id != session.session_id) {
                pending_event_valid = false;
                runtime_stats.tx_session_drops++;
            }
            known_session = session.session_id;
            sequence = 0u;
            next_stats_chunk = 0u;
            next_stats_due_us = (uint32_t)esp_timer_get_time();
            hello_pending = true;
            capture_retries = 0u;
        }

        if (hello_pending) {
            const esp_err_t err = send_hello_ack(known_session, &sequence);
            if (err == ESP_OK) {
                hello_pending = false;
            } else {
                handle_transport_error(err);
                vTaskDelay(pdMS_TO_TICKS(TRANSPORT_BACKOFF_MS));
            }
            continue;
        }

        if (!pending_event_valid) {
            pending_event_valid = xQueueReceive(event_queue, &pending_event, 0u) == pdTRUE;
            if (pending_event_valid) {
                event_retries = 0u;
                event_first_attempt_us = (uint32_t)esp_timer_get_time();
            }
        }
        if (pending_event_valid) {
            if (pending_event.session_id == known_session &&
                (session.capabilities & GLOVE_PROTOCOL_CAP_EVENTS) != 0u) {
                const esp_err_t err = send_event(&pending_event, &sequence);
                if (err == ESP_OK) {
                    pending_event_valid = false;
                } else if (err == ESP_ERR_INVALID_STATE) {
                    pending_event_valid = false;
                    runtime_stats.tx_session_drops++;
                } else {
                    handle_transport_error(err);
                    const uint32_t elapsed_us = (uint32_t)esp_timer_get_time() - event_first_attempt_us;
                    event_retries++;
                    if (event_retries >= EVENT_TX_MAX_RETRIES || elapsed_us >= EVENT_TX_DEADLINE_US) {
                        pending_event_valid = false;
                        runtime_stats.event_tx_drops++;
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(TRANSPORT_BACKOFF_MS));
                    }
                }
            } else {
                pending_event_valid = false;
                runtime_stats.tx_session_drops++;
            }
            continue;
        }

        if (current_slot < 0) {
            uint8_t slot_index = 0u;
            if (xQueueReceive(capture_queue, &slot_index, 0u) == pdTRUE && slot_index < CAPTURE_SLOT_COUNT) {
                capture_slot_t *slot = &capture_slots[slot_index];
                if (capture_slot_get_state(slot) == CAPTURE_SLOT_QUEUED &&
                    slot->session_id == known_session &&
                    (session.capabilities & GLOVE_PROTOCOL_CAP_CAPTURES) != 0u) {
                    bool claimed = false;
                    portENTER_CRITICAL(&capture_state_lock);
                    if (slot->state == CAPTURE_SLOT_QUEUED && slot->session_id == known_session) {
                        slot->state = CAPTURE_SLOT_TRANSMITTING;
                        claimed = true;
                    }
                    portEXIT_CRITICAL(&capture_state_lock);
                    if (claimed) {
                        current_slot = slot_index;
                        capture_retries = 0u;
                        capture_first_attempt_us = (uint32_t)esp_timer_get_time();
                    }
                } else {
                    capture_slot_set_state(slot, CAPTURE_SLOT_FREE);
                    runtime_stats.tx_session_drops++;
                }
                continue;
            }
        }

        if (current_slot >= 0) {
            capture_slot_t *slot = &capture_slots[current_slot];
            if (slot->session_id != known_session || !session_is_current(known_session)) {
                current_slot = -1;
                capture_slot_set_state(slot, CAPTURE_SLOT_FREE);
                runtime_stats.tx_session_drops++;
                continue;
            }
            esp_err_t err;
            if (!slot->info_sent) {
                err = send_capture_info(slot, &sequence);
                if (err == ESP_OK) {
                    slot->info_sent = true;
                }
            } else {
                err = send_capture_fragment(slot, &sequence);
                if (err == ESP_OK && slot->tx_offset >= capture_blob_size(slot)) {
                    current_slot = -1;
                    capture_slot_set_state(slot, CAPTURE_SLOT_FREE);
                }
            }
            handle_transport_error(err);
            if (err == ESP_OK) {
                /* Limit applies to consecutive stalls, not a long healthy transfer. */
                capture_retries = 0u;
                capture_first_attempt_us = (uint32_t)esp_timer_get_time();
            }
            if (err != ESP_OK) {
                if (err == ESP_ERR_INVALID_STATE) {
                    current_slot = -1;
                    capture_slot_set_state(slot, CAPTURE_SLOT_FREE);
                } else {
                    capture_retries++;
                    const uint32_t elapsed_us = (uint32_t)esp_timer_get_time() - capture_first_attempt_us;
                    if (capture_retries >= CAPTURE_TX_MAX_RETRIES || elapsed_us >= CAPTURE_TX_DEADLINE_US) {
                        current_slot = -1;
                        capture_slot_set_state(slot, CAPTURE_SLOT_FREE);
                        runtime_stats.capture_tx_drops++;
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(TRANSPORT_BACKOFF_MS));
                    }
                }
            }
            continue;
        }

        /* Raw capture traffic wins over periodic diagnostics. */
        const uint32_t now_us = (uint32_t)esp_timer_get_time();
        if ((session.capabilities & GLOVE_PROTOCOL_CAP_STATS) != 0u) {
            if (next_stats_chunk >= STATS_CHUNK_COUNT &&
                glove_protocol_time_reached_u32(now_us, next_stats_due_us)) {
                next_stats_chunk = 0u;
            }
            if (next_stats_chunk < STATS_CHUNK_COUNT) {
                const esp_err_t err = send_stats_chunk(known_session, &sequence, next_stats_chunk);
                handle_transport_error(err);
                if (err == ESP_OK) {
                    next_stats_chunk++;
                    if (next_stats_chunk >= STATS_CHUNK_COUNT) {
                        next_stats_due_us = now_us + STATS_INTERVAL_US;
                    }
                } else if (err != ESP_ERR_INVALID_STATE) {
                    vTaskDelay(pdMS_TO_TICKS(TRANSPORT_BACKOFF_MS));
                }
                continue;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TRANSPORT_IDLE_WAIT_MS));
    }
}

static void sensor_task(void *argument)
{
    (void)argument;
    const punch_detector_config_t detector_config = {
        .trigger_force_centi_kg = PUNCH_TRIGGER_CENTI_KG,
        .release_force_centi_kg = PUNCH_RELEASE_CENTI_KG,
        .release_debounce_us = PUNCH_RELEASE_DEBOUNCE_US,
        .minimum_retrigger_us = PUNCH_MINIMUM_RETRIGGER_US,
    };
    punch_detector_t detector;
    punch_detector_init(&detector, &detector_config);

    ESP_LOGI(TAG, "Hold glove still; calibration starts in %u seconds", CALIBRATION_WAIT_MS / 1000u);
    vTaskDelay(pdMS_TO_TICKS(CALIBRATION_WAIT_MS));
    (void)calibrate_motion_sensors();
    ESP_LOGI(TAG, "Acquisition running: %u Hz DRDY, %ums pre + %ums post capture context",
             ACCEL_SAMPLE_RATE_HZ, PRE_TRIGGER_MS, POST_TRIGGER_MS);

    while (true) {
        const bool drdy_timeout = !wait_for_data_ready();
        const uint32_t timestamp_us = (uint32_t)esp_timer_get_time();
        timed_sample_t sample = begin_sample(timestamp_us, drdy_timeout);
        int reserved_capture_slot = -1;
        uint16_t reserved_capture_session = 0u;
        uint16_t reserved_capture_punch_id = 0u;

        const bool fsr_valid = (sample.raw.validity_flags & GLOVE_SAMPLE_FLAG_FSR_VALID) != 0u;
        const uint16_t force_centi_kg = fsr_valid ? fsr_raw_to_centi_kg(sample.raw.fsr_adc) : 0u;
        const punch_detector_result_t detection = punch_detector_update(
            &detector, timestamp_us, force_centi_kg, fsr_valid);
        if (detection.triggered) {
            runtime_stats.punch_events++;
            const uint16_t punch_id = next_punch_id++;
            glove_ble_session_t session = {0};
            if (glove_ble_get_session(&session) && session.active) {
                uint16_t event_flags = GLOVE_EVENT_FLAG_FSR_VALID;
                if ((session.capabilities & GLOVE_PROTOCOL_CAP_CAPTURES) != 0u) {
                    uint8_t claimed_slot = 0u;
                    if (reserve_capture_slot(&claimed_slot)) {
                        reserved_capture_slot = (int)claimed_slot;
                        reserved_capture_session = session.session_id;
                        reserved_capture_punch_id = punch_id;
                        event_flags |= GLOVE_EVENT_FLAG_CAPTURE_ALLOCATED;
                    } else {
                        event_flags |= GLOVE_EVENT_FLAG_CAPTURE_DROPPED;
                    }
                }
                if ((session.capabilities & GLOVE_PROTOCOL_CAP_EVENTS) != 0u) {
                    const punch_event_t event = {
                        .session_id = session.session_id,
                        .punch_id = punch_id,
                        .trigger_time_us = timestamp_us,
                        .fsr_adc = sample.raw.fsr_adc,
                        .force_centi_kg = force_centi_kg,
                        .flags = event_flags,
                    };
                    /* This enqueue is ahead of every I2C transfer and snapshot copy. */
                    queue_punch_event(&event);
                }
            }
        }

        finish_sample(&sample, drdy_timeout);
        store_sample(&sample);
        /* Existing captures receive the trigger sample once before a new slot starts. */
        append_to_active_captures(&sample);
        if (reserved_capture_slot >= 0) {
            (void)initialize_reserved_capture((uint8_t)reserved_capture_slot,
                                               reserved_capture_session,
                                               reserved_capture_punch_id,
                                               timestamp_us);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Smart Boxing Glove firmware v2 booting");
    memset(capture_slots, 0, sizeof(capture_slots));
    memset(&runtime_stats, 0, sizeof(runtime_stats));

    const esp_err_t nvs_err = init_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: %s; BLE may not start", esp_err_to_name(nvs_err));
    }
    const esp_err_t ble_err = nvs_err == ESP_OK ? transmit_driver_init() : ESP_ERR_INVALID_STATE;
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE unavailable: %s; acquisition continues", esp_err_to_name(ble_err));
    }

    const esp_err_t i2c_err = i2c_init();
    if (i2c_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C unavailable: %s; samples will report invalid sensors", esp_err_to_name(i2c_err));
    }
    const esp_err_t accel_err = i2c_err == ESP_OK ? accel_init() : ESP_ERR_INVALID_STATE;
    const esp_err_t gyro_err = i2c_err == ESP_OK ? giro_init() : ESP_ERR_INVALID_STATE;
    const esp_err_t mag_err = i2c_err == ESP_OK ? mag_init() : ESP_ERR_INVALID_STATE;
    const esp_err_t fsr_err = fsr_init();
    if (accel_err != ESP_OK || gyro_err != ESP_OK || mag_err != ESP_OK || fsr_err != ESP_OK) {
        ESP_LOGW(TAG, "Sensor boot status: ADXL=%s ITG=%s QMC=%s FSR=%s",
                 esp_err_to_name(accel_err), esp_err_to_name(gyro_err),
                 esp_err_to_name(mag_err), esp_err_to_name(fsr_err));
    }

    led_init();
    led_set_estado(ESTADO_BUSCA_BLE);
    event_queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(punch_event_t));
    capture_queue = xQueueCreate(CAPTURE_QUEUE_LENGTH, sizeof(uint8_t));
    if (event_queue == NULL || capture_queue == NULL) {
        ESP_LOGE(TAG, "Queue allocation failed; firmware cannot run safely");
        return;
    }

    if (xTaskCreate(transport_task, "glove_tx", TRANSPORT_TASK_STACK_SIZE, NULL,
                    TRANSPORT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create transport task");
        return;
    }
    if (xTaskCreate(sensor_task, "glove_acq", SENSOR_TASK_STACK_SIZE, NULL,
                    SENSOR_TASK_PRIORITY, &sensor_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create acquisition task");
        return;
    }
    if (accel_err == ESP_OK) {
        const esp_err_t int_err = accel_setup_interrupt(sensor_task_handle);
        if (int_err != ESP_OK) {
            ESP_LOGE(TAG, "ADXL data-ready interrupt unavailable: %s; timeout recovery active",
                     esp_err_to_name(int_err));
        }
    } else {
        ESP_LOGW(TAG, "ADXL unavailable; acquisition uses timeout recovery only");
    }

    ESP_LOGI(TAG, "Static acquisition storage=%u bytes; free 8-bit heap=%u bytes",
             (unsigned)(sizeof(sample_ring) + sizeof(capture_slots)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
