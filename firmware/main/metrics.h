#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled;
    bool connected;
    uint32_t dropped;       // readings lost because the queue was full
    int outbox_bytes;
} metrics_stats_t;

/* Starts the MQTT client and the metrics task. The topic layout and payload are the contract with
 * hc-ingest and the PostgreSQL `temperature_raw` table; they must not change. password is the
 * revealed MQTT_PASS and must stay valid for the lifetime of the client.
 *
 * Nothing here may block the caller: esp_mqtt_client_enqueue() waits on the esp-mqtt API lock with
 * no timeout, and the client task holds that lock across transport_connect, so a publish from the
 * sensor task would stall it for the network timeout and trip the task watchdog. Producers only do
 * a non-blocking queue send; this task does the talking. */
void metrics_start(const char *password);

/* Non-blocking; dropped when the queue is full or metrics are disabled. mono_s is the monotonic
 * time of the reading (events_mono_s()); the wall clock is applied when the item is sent, so
 * readings taken before the clock is set are kept rather than thrown away. */
void metrics_post(int64_t mono_s, const char *sensor_id, float value);

void metrics_stats(metrics_stats_t *out);
