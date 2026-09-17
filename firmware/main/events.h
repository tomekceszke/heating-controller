#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EV_SENSORS = 0,     // bus scan finished: count found, prev_count before it
    EV_SENSOR_LOST,     // detail = label, after SENSOR_FAIL_READS bad reads in a row
    EV_SENSOR_BACK,     // detail = label
    EV_MQTT,            // on = connected
    EV_OUTBOX_FULL,     // the MQTT outbox is full and readings are being dropped
} event_type_t;

typedef struct {
    event_type_t type;
    int64_t mono_s;
    bool on;
    uint8_t count;
    uint8_t prev_count;
    bool has_temp;
    int16_t temp_x10;
    char detail[28];        // sensor label, where one applies
} event_t;

void events_init(void);

/* Never blocks: stores the event in the RAM ring and queues a notification where one is warranted
 * (sensors lost or recovered, sensor count changed, outbox full). Callable from the sensor task. */
void events_publish(const event_t *event);

/* Newest first; returns the number copied. */
size_t events_recent(event_t *out, size_t max);

/* Wall-clock time (s) of a monotonic timestamp, 0 when the clock is not synced. */
int64_t events_unix_time(int64_t mono_s);

/* Monotonic seconds since boot. */
int64_t events_mono_s(void);
