/**
 * @file punch_detector.h
 * @brief Deterministic, host-testable force threshold detector.
 */
#ifndef PUNCH_DETECTOR_H
#define PUNCH_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Detector settings expressed in centi-kilograms and microseconds. */
typedef struct {
    uint16_t trigger_force_centi_kg;
    uint16_t release_force_centi_kg;
    uint32_t release_debounce_us;
    uint32_t minimum_retrigger_us;
} punch_detector_config_t;

/** Mutable detector state.  Keep one instance per glove. */
typedef struct {
    punch_detector_config_t config;
    bool active;
    bool release_pending;
    bool has_triggered;
    uint32_t release_started_us;
    uint32_t last_trigger_us;
} punch_detector_t;

/** Result of processing one force reading. */
typedef struct {
    bool triggered;
    bool released;
} punch_detector_result_t;

/** Sets a known-safe idle state. */
void punch_detector_init(punch_detector_t *detector, const punch_detector_config_t *config);

/**
 * Updates the detector.  Invalid FSR readings do not trigger or release a
 * punch, preventing an ADC fault from creating a false event.
 */
punch_detector_result_t punch_detector_update(
    punch_detector_t *detector,
    uint32_t timestamp_us,
    uint16_t force_centi_kg,
    bool force_valid
);

#ifdef __cplusplus
}
#endif

#endif /* PUNCH_DETECTOR_H */
