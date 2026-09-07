/**
 * @file punch_detector.c
 * @brief Timestamp-based punch/rearm state machine.
 */
#include "punch_detector.h"

#include <string.h>

static bool elapsed_at_least(uint32_t now, uint32_t then, uint32_t interval)
{
    /* Unsigned subtraction makes the comparison correct across a u32 wrap. */
    return (uint32_t)(now - then) >= interval;
}

void punch_detector_init(punch_detector_t *detector, const punch_detector_config_t *config)
{
    if (detector == NULL || config == NULL) {
        return;
    }

    memset(detector, 0, sizeof(*detector));
    detector->config = *config;
}

punch_detector_result_t punch_detector_update(
    punch_detector_t *detector,
    uint32_t timestamp_us,
    uint16_t force_centi_kg,
    bool force_valid
)
{
    punch_detector_result_t result = {0};

    if (detector == NULL) {
        return result;
    }

    if (!force_valid) {
        /* A gap cannot count toward the continuous low-force debounce. */
        detector->release_pending = false;
        return result;
    }

    if (!detector->active) {
        if (force_centi_kg >= detector->config.trigger_force_centi_kg &&
            (!detector->has_triggered ||
             elapsed_at_least(timestamp_us,
                                detector->last_trigger_us,
                                detector->config.minimum_retrigger_us))) {
            detector->active = true;
            detector->release_pending = false;
            detector->has_triggered = true;
            detector->last_trigger_us = timestamp_us;
            result.triggered = true;
        }
        return result;
    }

    if (force_centi_kg > detector->config.release_force_centi_kg) {
        detector->release_pending = false;
        return result;
    }

    if (!detector->release_pending) {
        detector->release_pending = true;
        detector->release_started_us = timestamp_us;
        return result;
    }

    if (elapsed_at_least(timestamp_us,
                         detector->release_started_us,
                         detector->config.release_debounce_us)) {
        detector->active = false;
        detector->release_pending = false;
        result.released = true;
    }

    return result;
}
