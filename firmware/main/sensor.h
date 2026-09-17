#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/devices.h"

#define SENSOR_ID_LEN 17        // 16 hex chars + NUL, == OWB_ROM_CODE_STRING_LENGTH

typedef struct {
    char id[SENSOR_ID_LEN];     // ROM bytes 7->0 as lowercase hex, a key in temperature_raw
    const char *label;          // from config/devices.h, or the id when unknown
    const sensor_meta_t *meta;  // NULL for a sensor that is not in config/devices.h
    int display_order;
    bool valid;                 // the last read succeeded
    int16_t temp_x10;
    int64_t mono_s;             // monotonic time of the last good read
    uint32_t errors;            // failed reads since boot
    bool lost;                  // SENSOR_FAIL_READS failures in a row
} sensor_status_t;

/* Creates the sensor task: it discovers the bus (retrying forever while it is empty) and then samples
 * every SAMPLE_PERIOD_S. Never blocks the caller, so app_main can finish booting without sensors. */
void sensor_start(void);

/* True once the task has completed a cycle recently; the hi_health predicate. */
bool sensor_alive(void);

/* Discovery order, which is also the history slot order. Returns the number copied. */
size_t sensor_snapshot(sensor_status_t *out, size_t max);

/* Single read straight off the bus, for GET /sensor. Returns INVALID_TEMPERATURE_INDICATOR on any
 * failure (unknown id, bus busy, CRC error). */
float read_sensor(const char *sensor_id);
